/*
 * convert: turn an image file into a frame for an e-paper panel.
 *
 * This is also the library's usage example. Everything except decoding the
 * input (stb_image, plus its EXIF orientation) and writing the preview PNG
 * (stb_image_write) goes
 * through the public API in <epaper_image.h>, and the image is streamed a
 * row at a time, as firmware would do it:
 *
 *   source rows -> epi_resampler (crop / scale to the panel)
 *               -> epi_converter (tone, gamut, error diffusion to ink codes)
 *               -> panel rows
 *
 * The converter writes one ink code per pixel into a frame buffer, because
 * --rotate and --compare rearrange the frame before it is packed. From that
 * buffer, epi_pack_row makes the frame and epi_preview makes the preview.
 * Firmware that shows the image as rendered would pack each row as it pops
 * and send it.
 *
 * OUTPUT receives the frame in the panel's native layout, packed by
 * epi_pack_row (8/bits codes per byte, leftmost in the most significant
 * bits), rows top to bottom, no header.
 */

#include "exif.h"
#include "stb_image.h"
#include "stb_image_write.h"

#include <epaper_image.h>
#include <errno.h>
#include <math.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- Panels ----------------------------------------------------------- */

typedef struct {
    const char* name;         /* --panel value */
    const epi_panel_t* panel; /* built-in calibration */
    epi_pack_format_t format; /* the packed format the panel is sent */
    const char* about;
} panel_info_t;

static const panel_info_t PANELS[] = {
    {"el133uf1", &epi_panel_el133uf1, EPI_PACK_4BPP, "13.3\" Spectra 6 colour (reTerminal E1004)"},
    {"gdey075t7-mono", &epi_panel_gdey075t7_mono, EPI_PACK_1BPP, "7.5\" black and white (XIAO EE05)"},
    {"gdey075t7-grey4", &epi_panel_gdey075t7_grey4, EPI_PACK_2BPP, "7.5\" 4-level grey (XIAO EE05)"},
};
#define PANEL_COUNT (sizeof PANELS / sizeof PANELS[0])

/* Index of the panel's lightest (or darkest) ink by measured luminance. */
static size_t ink_by_lightness(const epi_panel_t* p, bool lightest) {
    size_t best = 0;
    long best_y = 0;
    for (size_t i = 0; i < p->ink_count; i++) {
        const epi_rgb_t c = p->inks[i].measured;
        const long y = 2126L * c.r + 7152L * c.g + 722L * c.b;
        if (i == 0 || (lightest ? y > best_y : y < best_y)) {
            best = i;
            best_y = y;
        }
    }
    return best;
}

/* ---- Options ---------------------------------------------------------- */

#define MAX_CELLS 4

typedef struct {
    const panel_info_t* panel;
    const char* input;
    const char* output;
    const char* preview;
    float preview_scale;
    bool nominal;
    bool fit;
    bool rotate;
    epi_rgb_t background;
    epi_filter_t filter;
    epi_intent_t intent; /* single-image mode */
    bool intent_set;     /* --preset or an intent value was given */
    bool compare;
    bool seed_set;
    unsigned long long seed;
    const char* key;
    size_t cell_count;
    const char* cell_cfg[MAX_CELLS];
    epi_intent_t cell_intent[MAX_CELLS];
} options_t;

static const char USAGE[] =
    "usage: convert [options] INPUT OUTPUT\n"
    "       convert --compare --seed N [--key KEY.json] [options] INPUT OUTPUT CFG1 CFG2 [CFG3 [CFG4]]\n"
    "       convert --list-panels\n"
    "\n"
    "Convert an image (PNG, JPEG, BMP, ...) for an e-paper panel. OUTPUT gets the\n"
    "packed frame the panel is sent, in its native layout. A JPEG's EXIF\n"
    "orientation is applied.\n"
    "\n"
    "  --panel NAME        el133uf1 (default), gdey075t7-mono, gdey075t7-grey4\n"
    "  --list-panels       list the panels and their presets\n"
    "  --preset NAME       one of the panel's presets (default: its first)\n"
    "  --tone X  --detail X  --adapt X  --vibrance X  --veil X  --lift X\n"
    "                      override one value of the preset's intent\n"
    "  --cover             fill the panel, cropping the image (default)\n"
    "  --fit               show the whole image on the background\n"
    "  --background R,G,B  background for --fit (default 255,255,255)\n"
    "  --filter NAME       box, bilinear, bicubic or lanczos3 (default)\n"
    "  --rotate            render for the panel turned a quarter turn clockwise\n"
    "                      (landscape on the el133uf1, portrait on the gdey075t7)\n"
    "  --preview FILE.png  also write what the panel will show, as rendered\n"
    "  --preview-scale X   make the preview X (0 < X <= 1) of the shown size,\n"
    "                      averaged in linear light (default 1)\n"
    "  --nominal           preview in the inks' nominal colours, not measured\n"
    "\n"
    "Blind comparison:\n"
    "  --compare           render each CFG into one cell of a 2x2 grid, cells\n"
    "                      A-D (labelled on the panel) assigned by the seed\n"
    "  --seed N            shuffle seed\n"
    "  --key KEY.json      write which CFG is in which cell (default: stdout)\n"
    "  CFG                 preset=NAME and/or intent values, comma-separated,\n"
    "                      e.g. preset=Soft or tone=0.3,vibrance=1.2\n";

static int fail(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "convert: ");
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    return 2;
}

static bool parse_float(const char* s, float* out) {
    char* end;
    errno = 0;
    const double v = strtod(s, &end);
    if (end == s || *end != '\0' || errno != 0 || !isfinite(v)) {
        return false;
    }
    *out = (float)v;
    return true;
}

/* The intent field named by key, or NULL. */
static float* intent_field(epi_intent_t* it, const char* key) {
    if (strcmp(key, "tone") == 0) {
        return &it->tone;
    }
    if (strcmp(key, "detail") == 0) {
        return &it->detail;
    }
    if (strcmp(key, "adapt") == 0) {
        return &it->adapt;
    }
    if (strcmp(key, "vibrance") == 0) {
        return &it->vibrance;
    }
    if (strcmp(key, "veil") == 0) {
        return &it->veil;
    }
    if (strcmp(key, "lift") == 0) {
        return &it->lift;
    }
    return NULL;
}

/* Copy the panel preset called name (any case) into *out. */
static bool find_preset(const epi_panel_t* p, const char* name, epi_intent_t* out) {
    const epi_intent_t* it = epi_panel_preset(p, name);
    if (it == NULL) {
        return false;
    }
    *out = *it;
    return true;
}

static void list_presets(FILE* f, const epi_panel_t* p) {
    for (size_t i = 0; i < p->preset_count; i++) {
        fprintf(f, "%s%s", i ? ", " : "", p->presets[i].name);
    }
}

/*
 * Parse a compare CFG ("preset=Soft,tone=0.3"): start from the panel's
 * default preset, or the named one wherever it appears, then apply the
 * intent values left to right.
 */
static bool parse_cfg(const epi_panel_t* p, const char* cfg, epi_intent_t* out) {
    char buf[256];
    if (strlen(cfg) >= sizeof buf || cfg[0] == '\0') {
        return false;
    }
    *out = *epi_panel_preset(p, NULL);
    for (int pass = 0; pass < 2; pass++) { /* pass 0: preset, pass 1: values */
        strcpy(buf, cfg);
        for (char* item = strtok(buf, ","); item; item = strtok(NULL, ",")) {
            char* eq = strchr(item, '=');
            if (!eq) {
                return false;
            }
            *eq = '\0';
            const char* val = eq + 1;
            if (strcmp(item, "preset") == 0) {
                if (pass == 0 && !find_preset(p, val, out)) {
                    return false;
                }
                continue;
            }
            float* field = intent_field(out, item);
            if (pass == 1 && (!field || !parse_float(val, field))) {
                return false;
            }
        }
    }
    return true;
}

static bool parse_rgb(const char* s, epi_rgb_t* out) {
    unsigned r, g, b;
    char tail;
    if (sscanf(s, "%u,%u,%u%c", &r, &g, &b, &tail) != 3 || r > 255 || g > 255 || b > 255) {
        return false;
    }
    *out = (epi_rgb_t){(uint8_t)r, (uint8_t)g, (uint8_t)b};
    return true;
}

static void print_panels(void) {
    for (size_t i = 0; i < PANEL_COUNT; i++) {
        const epi_panel_t* p = PANELS[i].panel;
        const size_t bytes = epi_pack_row_size(PANELS[i].format, p->width) * p->height;
        printf("%-16s %s\n", PANELS[i].name, PANELS[i].about);
        printf(
            "%-16s %ux%u, %zu inks, %u bpp, %zu-byte frame\n",
            "",
            (unsigned)p->width,
            (unsigned)p->height,
            p->ink_count,
            (unsigned)epi_pack_row_size(PANELS[i].format, 8),
            bytes
        );
        printf("%-16s presets: ", "");
        list_presets(stdout, p);
        printf("\n");
    }
}

/* Returns 0 to go on, 1 when done (--help, --list-panels), 2 on error. */
static int parse_args(int argc, char** argv, options_t* o) {
    const char* preset = NULL;
    const char* values[6] = {0};
    static const char* const value_keys[6] = {"tone", "detail", "adapt", "vibrance", "veil", "lift"};
    const char* pos[2 + MAX_CELLS] = {0};
    size_t npos = 0;

    *o = (options_t){0};
    o->panel = &PANELS[0];
    o->background = (epi_rgb_t){255, 255, 255};
    o->filter = EPI_FILTER_LANCZOS3;
    o->preview_scale = 1.0f;
    bool preview_scale_set = false;

    for (int i = 1; i < argc; i++) {
        const char* a = argv[i];
        if (a[0] != '-' || a[1] == '\0') {
            if (npos == sizeof pos / sizeof pos[0]) {
                return fail("too many arguments (at most four CFGs)");
            }
            pos[npos++] = a;
            continue;
        }
        if (strcmp(a, "-h") == 0 || strcmp(a, "--help") == 0) {
            fputs(USAGE, stdout);
            return 1;
        }
        if (strcmp(a, "--list-panels") == 0) {
            print_panels();
            return 1;
        }
        if (strcmp(a, "--cover") == 0) {
            o->fit = false;
            continue;
        }
        if (strcmp(a, "--fit") == 0) {
            o->fit = true;
            continue;
        }
        if (strcmp(a, "--rotate") == 0) {
            o->rotate = true;
            continue;
        }
        if (strcmp(a, "--nominal") == 0) {
            o->nominal = true;
            continue;
        }
        if (strcmp(a, "--compare") == 0) {
            o->compare = true;
            continue;
        }

        /* Everything else takes a value. */
        if (i + 1 == argc) {
            return fail("%s needs a value", a);
        }
        const char* v = argv[++i];
        bool known = true;
        if (strcmp(a, "--panel") == 0) {
            o->panel = NULL;
            for (size_t k = 0; k < PANEL_COUNT; k++) {
                if (strcmp(v, PANELS[k].name) == 0) {
                    o->panel = &PANELS[k];
                }
            }
            if (!o->panel) {
                return fail("unknown panel %s (see --list-panels)", v);
            }
        } else if (strcmp(a, "--preset") == 0) {
            preset = v;
        } else if (strcmp(a, "--background") == 0) {
            if (!parse_rgb(v, &o->background)) {
                return fail("--background %s: want R,G,B (0..255)", v);
            }
        } else if (strcmp(a, "--filter") == 0) {
            static const char* const filters[] = {"box", "bilinear", "bicubic", "lanczos3"};
            static const epi_filter_t ids[] = {
                EPI_FILTER_BOX, EPI_FILTER_BILINEAR, EPI_FILTER_BICUBIC, EPI_FILTER_LANCZOS3
            };
            size_t k = 0;
            while (k < 4 && strcmp(v, filters[k]) != 0) {
                k++;
            }
            if (k == 4) {
                return fail("unknown filter %s", v);
            }
            o->filter = ids[k];
        } else if (strcmp(a, "--preview") == 0) {
            o->preview = v;
        } else if (strcmp(a, "--preview-scale") == 0) {
            if (!parse_float(v, &o->preview_scale) || !(o->preview_scale > 0.0f && o->preview_scale <= 1.0f)) {
                return fail("--preview-scale %s: want a number above 0, up to 1", v);
            }
            preview_scale_set = true;
        } else if (strcmp(a, "--key") == 0) {
            o->key = v;
        } else if (strcmp(a, "--seed") == 0) {
            char* end;
            errno = 0;
            o->seed = strtoull(v, &end, 10);
            if (end == v || *end != '\0' || errno != 0 || v[0] == '-') {
                return fail("--seed %s: want an integer", v);
            }
            o->seed_set = true;
        } else {
            known = false;
            for (size_t k = 0; k < 6; k++) {
                if (strcmp(a + 2, value_keys[k]) == 0 && strncmp(a, "--", 2) == 0) {
                    values[k] = v;
                    known = true;
                }
            }
        }
        if (!known) {
            return fail("unknown option %s (see --help)", a);
        }
    }

    if (preview_scale_set && !o->preview) {
        return fail("--preview-scale needs --preview");
    }

    const epi_panel_t* p = o->panel->panel;
    o->intent_set = preset != NULL;
    o->intent = *epi_panel_preset(p, NULL);
    if (preset && !find_preset(p, preset, &o->intent)) {
        fprintf(stderr, "convert: %s has no preset %s (", o->panel->name, preset);
        list_presets(stderr, p);
        fprintf(stderr, ")\n");
        return 2;
    }
    for (size_t k = 0; k < 6; k++) {
        if (values[k]) {
            if (!parse_float(values[k], intent_field(&o->intent, value_keys[k]))) {
                return fail("--%s %s: want a number", value_keys[k], values[k]);
            }
            o->intent_set = true;
        }
    }

    if (!o->compare) {
        if (npos != 2) {
            return fail("want INPUT and OUTPUT (see --help)");
        }
        if (o->seed_set || o->key) {
            return fail("--seed and --key are for --compare");
        }
    } else {
        if (npos < 4) {
            return fail("--compare wants INPUT, OUTPUT and two to four CFGs");
        }
        if (!o->seed_set) {
            return fail("--compare needs --seed");
        }
        if (o->intent_set) {
            return fail("with --compare, give presets and values in the CFGs");
        }
        o->cell_count = npos - 2;
        for (size_t k = 0; k < o->cell_count; k++) {
            o->cell_cfg[k] = pos[2 + k];
            if (!parse_cfg(p, pos[2 + k], &o->cell_intent[k])) {
                return fail("bad CFG %s (want e.g. preset=Soft or tone=0.3,vibrance=1.2)", pos[2 + k]);
            }
        }
    }
    o->input = pos[0];
    o->output = pos[1];
    return 0;
}

/* ---- Rendering: the library pipeline ---------------------------------- */

typedef struct {
    const epi_rgb_t* pixels; /* decoded source, rows top to bottom */
    uint16_t width, height;
} image_t;

/*
 * Resample the image to a w x h cell and convert it once per intent. Each
 * source row is pushed into the resampler; each row it releases is pushed
 * into every converter; each row a converter releases (one ink code per
 * pixel) lands in that intent's cell: out[k], rows `stride` bytes apart.
 */
static int render(
    const image_t* img,
    const options_t* o,
    uint16_t w,
    uint16_t h,
    size_t n,
    const epi_intent_t* intents,
    uint8_t* const* out,
    size_t stride
) {
    int rc = 2;
    void* rs_mem = NULL;
    void* cv_mem[MAX_CELLS] = {0};
    epi_converter_t* cv[MAX_CELLS] = {0};
    uint16_t done[MAX_CELLS] = {0};
    epi_rgb_t* row = malloc(w * sizeof *row);

    /* The resampler: which part of the source lands where on the cell. */
    epi_resampler_config_t rcfg = {0};
    rcfg.src_width = img->width;
    rcfg.src_height = img->height;
    rcfg.dst_width = w;
    rcfg.dst_height = h;
    rcfg.background = o->background;
    rcfg.filter = o->filter;
    rcfg.max_src_pixels = UINT32_MAX; /* the image is already decoded on a desktop machine */
    if (o->fit) {
        epi_layout_fit(&rcfg); /* whole image, centred on the background */
    } else {
        epi_layout_cover(&rcfg); /* fill the cell, centre-cropped */
    }
    /*
     * The library never allocates: the caller sizes and owns each workspace.
     * (A size of 0 means an invalid config; init then says why.)
     */
    const size_t rs_size = epi_resampler_workspace_size(&rcfg);
    rs_mem = malloc(rs_size ? rs_size : 1);
    if (!row || !rs_mem) {
        fprintf(stderr, "convert: out of memory\n");
        goto out;
    }
    epi_resampler_t* rs = NULL;
    epi_err_t err = epi_resampler_init(&rcfg, rs_mem, rs_size, &rs);
    if (err != EPI_OK) {
        fprintf(stderr, "convert: epi_resampler_init: %s\n", epi_strerror(err));
        goto out;
    }

    /* One converter per intent, all fed the same resampled rows. */
    for (size_t k = 0; k < n; k++) {
        const epi_config_t ccfg = {
            .panel = o->panel->panel,
            .intent = &intents[k],
            .width = w,
            .height = h,
        };
        const size_t size = epi_converter_workspace_size(&ccfg);
        cv_mem[k] = malloc(size ? size : 1);
        if (!cv_mem[k]) {
            fprintf(stderr, "convert: out of memory\n");
            goto out;
        }
        /* Init validates everything, including the intent's value ranges. */
        err = epi_converter_init(&ccfg, cv_mem[k], size, &cv[k]);
        if (err != EPI_OK) {
            fprintf(
                stderr,
                "convert: epi_converter_init: %s%s\n",
                epi_strerror(err),
                err == EPI_ERR_INVALID_ARG ? " (an intent value out of range?)" : ""
            );
            goto out;
        }
    }

    /*
     * The streaming loop. After every push, pop until nothing is ready:
     * that is the contract for both stages. (With --fit, background rows
     * above the image are ready before the first push, hence y == -1.)
     */
    for (int32_t y = -1; y < img->height; y++) {
        if (y >= 0) {
            epi_resampler_push(rs, img->pixels + (size_t)y * img->width);
        }
        while (epi_resampler_pop(rs, row) == EPI_OK) {
            for (size_t k = 0; k < n; k++) {
                epi_converter_push(cv[k], row);
                while (done[k] < h && epi_converter_pop(cv[k], out[k] + (size_t)done[k] * stride) == EPI_OK) {
                    done[k]++;
                }
            }
        }
    }
    for (size_t k = 0; k < n; k++) {
        if (done[k] != h) {
            fprintf(stderr, "convert: converter released %u of %u rows\n", (unsigned)done[k], (unsigned)h);
            goto out;
        }
    }
    rc = 0;
out:
    for (size_t k = 0; k < n; k++) {
        free(cv_mem[k]);
    }
    free(rs_mem);
    free(row);
    return rc;
}

/* ---- Compare grid ----------------------------------------------------- */

/*
 * Cell labels: A-D in a 24x28 box of the lightest ink at each cell's outer
 * corner, the letter in the darkest ink. 16 px wide glyph rows, MSB left,
 * drawn at (4, 7) in the box.
 */
#define LABEL_W 24
#define LABEL_H 28
static const uint16_t GLYPHS[4][14] = {
    {0x0380, 0x0380, 0x02c0, 0x06c0, 0x0640, 0x0460, 0x0c60, 0x0c60, 0x0ff0, 0x1830, 0x1810, 0x3018, 0x3018, 0x3008},
    {0x1fc0, 0x1860, 0x1830, 0x1830, 0x1830, 0x1860, 0x1fc0, 0x1830, 0x1818, 0x1818, 0x1818, 0x1818, 0x1830, 0x1fe0},
    {0x03e0, 0x0e18, 0x180c, 0x180c, 0x300c, 0x3000, 0x3000, 0x3000, 0x3000, 0x300c, 0x180c, 0x1808, 0x0e18, 0x03e0},
    {0x1fc0, 0x1830, 0x1818, 0x1818, 0x180c, 0x180c, 0x180c, 0x180c, 0x180c, 0x180c, 0x1818, 0x1818, 0x1830, 0x1fc0},
};

static void draw_label(uint8_t* frame, size_t stride, size_t letter, size_t x0, size_t y0, uint8_t paper, uint8_t ink) {
    for (size_t y = 0; y < LABEL_H; y++) {
        for (size_t x = 0; x < LABEL_W; x++) {
            bool on = false;
            if (y >= 7 && y < 21 && x >= 4 && x < 20) {
                on = (GLYPHS[letter][y - 7] >> (15 - (x - 4))) & 1;
            }
            frame[(y0 + y) * stride + x0 + x] = on ? ink : paper;
        }
    }
}

/* splitmix64: a small, fixed generator, so a seed means the same layout everywhere. */
static uint64_t next_random(uint64_t* state) {
    uint64_t z = (*state += 0x9E3779B97F4A7C15u);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9u;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBu;
    return z ^ (z >> 31);
}

/* cell_of[k]: the cell (0 = A .. 3 = D) config k goes in (Fisher-Yates). */
static void shuffle_cells(uint64_t seed, size_t n, size_t* cell_of) {
    uint64_t state = seed;
    for (size_t i = 0; i < n; i++) {
        cell_of[i] = i;
    }
    for (size_t i = n - 1; i > 0; i--) {
        const size_t j = (size_t)(next_random(&state) % (i + 1));
        const size_t t = cell_of[i];
        cell_of[i] = cell_of[j];
        cell_of[j] = t;
    }
}

static void json_string(FILE* f, const char* s) {
    fputc('"', f);
    for (; *s; s++) {
        const unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') {
            fprintf(f, "\\%c", c);
        } else if (c < 0x20) {
            fprintf(f, "\\u%04x", c);
        } else {
            fputc(c, f);
        }
    }
    fputc('"', f);
}

static int write_key(const options_t* o, const size_t* cell_of) {
    FILE* f = o->key ? fopen(o->key, "w") : stdout;
    if (!f) {
        return fail("cannot write %s: %s", o->key, strerror(errno));
    }
    fprintf(f, "{\n  \"panel\": \"%s\",\n  \"seed\": %llu,\n  \"input\": ", o->panel->name, o->seed);
    json_string(f, o->input);
    fprintf(f, ",\n  \"rotate\": %s,\n  \"cells\": {", o->rotate ? "true" : "false");
    bool first = true;
    for (size_t cell = 0; cell < MAX_CELLS; cell++) {
        for (size_t k = 0; k < o->cell_count; k++) {
            if (cell_of[k] != cell) {
                continue;
            }
            const epi_intent_t* it = &o->cell_intent[k];
            fprintf(f, "%s\n    \"%c\": {\"config\": ", first ? "" : ",", (int)('A' + cell));
            json_string(f, o->cell_cfg[k]);
            fprintf(
                f,
                ", \"intent\": {\"tone\": %.9g, \"detail\": %.9g, \"adapt\": %.9g, \"vibrance\": %.9g, "
                "\"veil\": %.9g, \"lift\": %.9g}}",
                it->tone,
                it->detail,
                it->adapt,
                it->vibrance,
                it->veil,
                it->lift
            );
            first = false;
        }
    }
    fprintf(f, "\n  }\n}\n");
    if (o->key && fclose(f) != 0) {
        return fail("cannot write %s: %s", o->key, strerror(errno));
    }
    return 0;
}

/* ---- Output ----------------------------------------------------------- */

/*
 * Write the frame in the panel's native layout. With --rotate the frame
 * was rendered turned a quarter turn clockwise (vw x vh = native height x
 * width): view pixel (x, y) is native pixel (y, H - 1 - x).
 */
static int write_frame(const options_t* o, const uint8_t* view, uint16_t vw) {
    const epi_panel_t* p = o->panel->panel;
    const size_t packed = epi_pack_row_size(o->panel->format, p->width);
    uint8_t* codes = malloc(p->width);
    uint8_t* row = malloc(packed);
    FILE* f = fopen(o->output, "wb");
    int rc = 0;
    if (!codes || !row || !f) {
        rc = fail("cannot write %s: %s", o->output, strerror(errno));
        goto out;
    }
    for (size_t y = 0; y < p->height; y++) {
        const uint8_t* src = view + y * vw;
        if (o->rotate) {
            for (size_t x = 0; x < p->width; x++) {
                codes[x] = view[x * vw + (size_t)(vw - 1 - y)];
            }
            src = codes;
        }
        const epi_err_t err = epi_pack_row(p, o->panel->format, src, p->width, row);
        if (err != EPI_OK) {
            rc = fail("epi_pack_row: %s", epi_strerror(err));
            goto out;
        }
        if (fwrite(row, 1, packed, f) != packed) {
            rc = fail("cannot write %s: %s", o->output, strerror(errno));
            goto out;
        }
    }
out:
    if (f && fclose(f) != 0 && rc == 0) {
        rc = fail("cannot write %s: %s", o->output, strerror(errno));
    }
    free(row);
    free(codes);
    return rc;
}

/* One side of the preview: --preview-scale of the shown side, at least 1. */
static uint16_t preview_side(uint16_t side, float scale) {
    const long n = lround((double)side * scale);
    return (uint16_t)(n < 1 ? 1 : n);
}

/*
 * The preview, upright: the codes in the inks' measured (or nominal) colours,
 * averaged in linear light when smaller than the shown image. After every push,
 * pop until nothing is ready.
 */
static int write_preview(const options_t* o, const uint8_t* view, uint16_t vw, uint16_t vh) {
    const epi_preview_config_t cfg = {
        .panel = o->panel->panel,
        .src_width = vw,
        .src_height = vh,
        .dst_width = preview_side(vw, o->preview_scale),
        .dst_height = preview_side(vh, o->preview_scale),
        .nominal = o->nominal,
    };
    const size_t size = epi_preview_workspace_size(&cfg);
    void* mem = malloc(size ? size : 1);
    epi_rgb_t* rgb = malloc((size_t)cfg.dst_width * cfg.dst_height * sizeof *rgb);
    int rc = 0;
    if (!mem || !rgb) {
        rc = fail("out of memory");
        goto out;
    }
    epi_preview_t* preview = NULL;
    epi_err_t err = epi_preview_init(&cfg, mem, size, &preview);
    size_t done = 0;
    for (size_t y = 0; err == EPI_OK && y < vh; y++) {
        err = epi_preview_push(preview, view + y * vw);
        while (err == EPI_OK && epi_preview_pop(preview, rgb + done * cfg.dst_width) == EPI_OK) {
            done++;
        }
    }
    if (err != EPI_OK) {
        rc = fail("epi_preview: %s", epi_strerror(err));
    } else if (!stbi_write_png(o->preview, cfg.dst_width, cfg.dst_height, 3, rgb, cfg.dst_width * 3)) {
        rc = fail("cannot write %s", o->preview);
    }
out:
    free(rgb);
    free(mem);
    return rc;
}

/* The whole file, malloc'd (NULL with errno set on failure). */
static uint8_t* read_file(const char* path, size_t* size) {
    FILE* f = fopen(path, "rb");
    if (!f) {
        return NULL;
    }
    uint8_t* data = NULL;
    size_t n = 0, cap = 0;
    for (;;) {
        if (n == cap) {
            cap = cap ? cap * 2 : (size_t)1 << 20;
            uint8_t* grown = realloc(data, cap);
            if (!grown) {
                free(data);
                fclose(f);
                errno = ENOMEM;
                return NULL;
            }
            data = grown;
        }
        const size_t got = fread(data + n, 1, cap - n, f);
        n += got;
        if (got == 0) {
            break;
        }
    }
    const int err = ferror(f);
    fclose(f);
    if (err) {
        free(data);
        errno = EIO;
        return NULL;
    }
    *size = n;
    return data;
}

/* ---- Main ------------------------------------------------------------- */

int main(int argc, char** argv) {
    options_t o;
    int rc = parse_args(argc, argv, &o);
    if (rc != 0) {
        if (rc == 2) {
            fputs("(see convert --help)\n", stderr);
        }
        return rc == 1 ? 0 : rc;
    }
    const epi_panel_t* p = o.panel->panel;

    /* Decode to 8-bit RGB (epi_rgb_t is three bytes, so rows map directly),
     * upright: stb_image ignores a JPEG's EXIF orientation, so apply it.
     * (stbi_image_free is free(), so it also frees exif_apply's buffer.) */
    size_t file_size = 0;
    uint8_t* file = read_file(o.input, &file_size);
    if (!file) {
        return fail("cannot read %s: %s", o.input, strerror(errno));
    }
    int iw = 0, ih = 0, channels = 0;
    stbi_uc* pixels =
        file_size > INT32_MAX ? NULL : stbi_load_from_memory(file, (int)file_size, &iw, &ih, &channels, 3);
    const int orientation = exif_orientation(file, file_size);
    free(file);
    if (!pixels) {
        return fail("cannot read %s: %s", o.input, stbi_failure_reason());
    }
    if (orientation != 1) {
        int uw, uh;
        uint8_t* upright = exif_apply(pixels, iw, ih, 3, orientation, &uw, &uh);
        stbi_image_free(pixels);
        if (!upright) {
            return fail("out of memory");
        }
        pixels = upright;
        iw = uw;
        ih = uh;
    }
    if (iw > UINT16_MAX || ih > UINT16_MAX) {
        stbi_image_free(pixels);
        return fail("%s: larger than 65535 pixels", o.input);
    }
    const image_t img = {(const epi_rgb_t*)pixels, (uint16_t)iw, (uint16_t)ih};

    /* The frame as viewed: native size, or turned a quarter turn. */
    const uint16_t vw = (uint16_t)(o.rotate ? p->height : p->width);
    const uint16_t vh = (uint16_t)(o.rotate ? p->width : p->height);
    uint8_t* view = malloc((size_t)vw * vh);
    if (!view) {
        stbi_image_free(pixels);
        return fail("out of memory");
    }
    const uint8_t paper = p->inks[ink_by_lightness(p, true)].code;
    const uint8_t ink = p->inks[ink_by_lightness(p, false)].code;
    memset(view, paper, (size_t)vw * vh);

    size_t cell_of[MAX_CELLS] = {0};
    if (!o.compare) {
        uint8_t* out = view;
        rc = render(&img, &o, vw, vh, 1, &o.intent, &out, vw);
    } else {
        /*
         * Cells A B / C D fill the frame. The image is resampled once, to
         * cell size; each config's converter gets the same rows.
         */
        const uint16_t cw = (uint16_t)(vw / 2), ch = (uint16_t)(vh / 2);
        uint8_t* out[MAX_CELLS];
        shuffle_cells(o.seed, o.cell_count, cell_of);
        for (size_t k = 0; k < o.cell_count; k++) {
            out[k] = view + (cell_of[k] / 2) * ch * (size_t)vw + (cell_of[k] % 2) * cw;
        }
        rc = render(&img, &o, cw, ch, o.cell_count, o.cell_intent, out, vw);
        for (size_t k = 0; rc == 0 && k < o.cell_count; k++) {
            const size_t c = cell_of[k];
            const size_t x0 = (c % 2) * cw + (c % 2 ? cw - LABEL_W : 0);
            const size_t y0 = (c / 2) * ch + (c / 2 ? ch - LABEL_H : 0);
            draw_label(view, vw, c, x0, y0, paper, ink);
        }
    }
    stbi_image_free(pixels);

    if (rc == 0) {
        rc = write_frame(&o, view, vw);
    }
    if (rc == 0 && o.preview) {
        rc = write_preview(&o, view, vw, vh);
    }
    if (rc == 0 && o.compare) {
        rc = write_key(&o, cell_of);
    }
    if (rc == 0) {
        fprintf(
            stderr,
            "wrote %s (%s, %ux%u%s)\n",
            o.output,
            o.panel->name,
            (unsigned)p->width,
            (unsigned)p->height,
            o.rotate ? ", rendered rotated" : ""
        );
    }
    free(view);
    return rc;
}
