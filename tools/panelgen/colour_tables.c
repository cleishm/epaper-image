/*
 * The tables of one colour panel as JSON on stdout, from the library's own
 * derivation: panelgen renders them as C (panelgen/gen.py).
 *
 *   colour_tables SIGMA R G B NR NG NB [R G B NR NG NB ...]
 *
 * R G B is each ink's measured sRGB8 and NR NG NB its nominal. Exits 1 with the
 * reason on stderr if epi_colour_panel_init refuses the inks or sigma.
 */
#include "epaper_image.h"
#include "epi_panel_tables.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>

static void print_ints(const char* name, const int32_t* v, size_t n) {
    printf("\"%s\": [", name);
    for (size_t i = 0; i < n; i++) {
        printf("%s%" PRId32, i ? ", " : "", v[i]);
    }
    printf("],\n");
}

static void print_rows(const char* name, const int32_t (*m)[3], size_t rows) {
    printf("\"%s\": [", name);
    for (size_t i = 0; i < rows; i++) {
        printf("%s[%" PRId32 ", %" PRId32 ", %" PRId32 "]", i ? ", " : "", m[i][0], m[i][1], m[i][2]);
    }
    printf("],\n");
}

static void print_u16s(const char* name, const uint16_t* v, size_t n) {
    printf("\"%s\": [", name);
    for (size_t i = 0; i < n; i++) {
        printf("%s%u", i ? ", " : "", (unsigned)v[i]);
    }
    printf("],\n");
}

static int refuse(const char* why) {
    fprintf(stderr, "%s\n", why);
    return 1;
}

int main(int argc, char** argv) {
    if (argc < 2 || (argc - 2) % 6 != 0) {
        fprintf(stderr, "usage: colour_tables SIGMA R G B NR NG NB [R G B NR NG NB ...]\n");
        return 2;
    }
    const size_t n = (size_t)(argc - 2) / 6;
    static epi_ink_t inks[EPI_MAX_INKS];
    for (size_t i = 0; i < n && i < EPI_MAX_INKS; i++) {
        char** v = argv + 2 + 6 * i;
        inks[i].measured = (epi_rgb_t){(uint8_t)atoi(v[0]), (uint8_t)atoi(v[1]), (uint8_t)atoi(v[2])};
        inks[i].nominal = (epi_rgb_t){(uint8_t)atoi(v[3]), (uint8_t)atoi(v[4]), (uint8_t)atoi(v[5])};
    }

    const epi_colour_panel_config_t cfg = {"panelgen", 1, 1, inks, n, strtof(argv[1], NULL), NULL, NULL};
    const size_t size = epi_colour_panel_size(&cfg);
    void* storage = size ? malloc(size) : NULL;
    const epi_panel_t* panel = NULL;
    const char* why = NULL;
    if (epi_colour_panel_init(&cfg, storage, size, &panel, &why) != EPI_OK) {
        return refuse(why ? why : "colour panel not built");
    }
    const epi_colour_tables_t* t = epi_colour_tables(panel->tables);
    const int radius = t->base.kernel_radius;

    printf("{\n\"ink_count\": %u,\n\"kernel_radius\": %d,\n", (unsigned)t->base.ink_count, radius);
    print_u16s("kernel", t->base.kernel, 2 * (size_t)radius + 1);
    print_ints("white_xyz", t->white_xyz, 3);
    print_ints("black_xyz", t->black_xyz, 3);
    print_rows("lin_to_panel_adapt", t->lin_to_panel_adapt, 3);
    print_rows("lin_to_panel_bpc", t->lin_to_panel_bpc, 3);
    print_rows("xyz_to_lin", t->xyz_to_lin, 3);
    print_rows("lin_to_lms", t->lin_to_lms, 3);
    print_rows("lms_to_oklab", t->lms_to_oklab, 3);
    printf("\"hull_face_count\": %u,\n", (unsigned)t->hull_face_count);
    print_rows("hull_n", t->hull_n, t->hull_face_count);
    printf("\"hull_d\": [");
    for (unsigned f = 0; f < t->hull_face_count; f++) {
        printf("%s%" PRId64, f ? ", " : "", t->hull_d[f]);
    }
    printf("],\n\"lstar_min\": %" PRId32 ",\n\"lstar_max\": %" PRId32 ",\n", t->lstar_min, t->lstar_max);
    print_u16s("cusp_lstar", t->cusp_lstar, 360);
    print_rows("ink_lin", t->ink_lin, n);
    print_rows("ink_oklab", t->ink_oklab, n);
    printf("\"nhat\": [%" PRId32 ", %" PRId32 ", %" PRId32 "]\n}\n", t->nhat[0], t->nhat[1], t->nhat[2]);
    return 0;
}
