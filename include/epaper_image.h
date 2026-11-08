/**
 * @file epaper_image.h
 * @brief Image conversion for e-paper panels.
 *
 * Streaming resampling of RGB rows to panel size, followed by tone
 * adjustment, dynamic-range compression, palette matching and error
 * diffusion to panel ink codes. Rows of ink codes are then reduced to a
 * preview, packed for the panel, or both.
 */

#ifndef EPAPER_IMAGE_H
#define EPAPER_IMAGE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Library major version. */
#define EPI_VERSION_MAJOR 0
/** @brief Library minor version. */
#define EPI_VERSION_MINOR 3
/** @brief Library patch version. */
#define EPI_VERSION_PATCH 0

/**
 * @brief Error codes returned by library functions.
 */
typedef enum {
    /** Success. */
    EPI_OK = 0,
    /** An argument was invalid (e.g. a NULL pointer or out-of-range value). */
    EPI_ERR_INVALID_ARG,
    /** The caller-provided workspace buffer is too small. */
    EPI_ERR_WORKSPACE_TOO_SMALL,
    /** Pop only: no output row is available yet; push more rows. */
    EPI_ERR_NOT_READY,
    /** Push only: earlier rows were not drained; the row was rejected and no
     *  memory was touched. */
    EPI_ERR_NOT_DRAINED,
    /** All rows have already been pushed (push) or popped (pop). */
    EPI_ERR_FINISHED,
    /** The source image is larger than the resampler accepts (see
     *  #epi_resampler_config_t). */
    EPI_ERR_SOURCE_TOO_LARGE,
    /** The caller's progress callback stopped epi_colour_panel_init(). */
    EPI_ERR_CANCELLED,
} epi_err_t;

/**
 * @brief Get a human-readable description of an error code.
 *
 * @param err Error code.
 * @return A static, NUL-terminated string. Never NULL; "unknown error" is
 *         returned for values outside #epi_err_t.
 */
const char* epi_strerror(epi_err_t err);

/** @brief An 8-bit sRGB colour. */
typedef struct {
    uint8_t r, g, b;
} epi_rgb_t;

/**
 * @brief One ink (colour panels) or grey level (grey panels).
 */
typedef struct {
    /** Value written to the panel for this ink. */
    uint8_t code;
    /** Ideal colour of the ink, for nominal previews. A colour panel's init also
     *  compares these to check which ink is white: all zero skips that check. */
    epi_rgb_t nominal;
    /** What the panel shows: drives matching, error and measured previews. */
    epi_rgb_t measured;
} epi_ink_t;

/**
 * @brief Rendering intent. Every preset is a value of this.
 */
typedef struct {
    /** 0 = black-point-compensated lightness (Soft) .. 1 = midtone curve (Deep). */
    float tone;
    /** 0..1: share of local detail kept around the blurred base. */
    float detail;
    /** 0..1: adaptation to paper white below the highlights. */
    float adapt;
    /** Chroma boost sparing skin and near-neutrals: 1 = off; 0..3. */
    float vibrance;
    /** L* below paper white for source white: 0 = off; 0..3. */
    float veil;
    /** L* added to lower midtones, fading to nothing by L* 82 and sparing saturated colours: 0 = off; 0..20. */
    float lift;
} epi_intent_t;

/** @brief A named intent offered for a panel. */
typedef struct {
    const char* name;
    const epi_intent_t* intent;
} epi_preset_t;

/** @brief Tables derived from a panel's inks (opaque). */
typedef struct epi_panel_tables epi_panel_tables_t;

/**
 * @brief A panel calibration: what the panel shows, and how to render for it.
 */
typedef struct epi_panel {
    /** Glass name, e.g. "EL133UF1". */
    const char* name;
    /** Native size in pixels. */
    uint16_t width, height;
    /** Inks in matching order; grey panels list levels darkest first. */
    const epi_ink_t* inks;
    size_t ink_count;
    /** Detail blur sigma in panel pixels (informational; the tables hold the kernel). */
    float detail_sigma;
    /** Presets; [0] is the default. */
    const epi_preset_t* presets;
    size_t preset_count;
    const epi_panel_tables_t* tables;
} epi_panel_t;

/** @brief E Ink EL133UF1, Spectra 6, 13.3" 1200x1600 (reTerminal E1004). */
extern const epi_panel_t epi_panel_el133uf1;

/**
 * @brief GDEY075T7, 7.5" 800x480 grey glass, 2-level (black/white)
 *        waveform. Levels measured 2026-10-31 with the controller's OTP
 *        full-refresh waveform.
 */
extern const epi_panel_t epi_panel_gdey075t7_mono;

/**
 * @brief GDEY075T7, 7.5" 800x480 grey glass, 4 grey levels. Levels
 *        measured 2026-10-31 with idfxx_epaper_uc8179's register-loaded
 *        4-grey waveform at its default 20 Hz frame rate and ±15 V source
 *        voltage. Grey levels depend on the waveform: another waveform
 *        (frame rate, voltage, LUT or driver) needs its own calibration.
 */
extern const epi_panel_t epi_panel_gdey075t7_grey4;

/**
 * @brief A grey panel described by its measured levels (see
 *        epi_grey_panel_init()).
 */
typedef struct {
    /** Glass or mode name; not copied, so it must outlive the panel. */
    const char* name;
    /** Native size in pixels, each ≥ 1. */
    uint16_t width, height;
    /**
     * @c level_count levels, darkest first. The measured colours' luminance
     * (CIE Y of the sRGB8 value) must increase strictly from first to last;
     * codes are free (they are what the panel is sent). Copied by
     * epi_grey_panel_init(), so this array may be freed afterwards.
     */
    const epi_ink_t* levels;
    /** 2..16. */
    size_t level_count;
    /** Detail blur sigma in panel pixels, in (0, 10.5] (radius ⌈3σ⌉ ≤ 32). */
    float detail_sigma;
} epi_grey_panel_config_t;

/**
 * @brief Storage bytes epi_grey_panel_init() needs for @p cfg, or 0 if
 *        @p cfg is invalid.
 *
 * Includes alignment slack, so any storage pointer may be passed.
 */
size_t epi_grey_panel_size(const epi_grey_panel_config_t* cfg);

/**
 * @brief Build a grey panel in caller-provided storage from measured levels.
 *
 * The storage holds the #epi_panel_t, its derived tables (each level's Y
 * relative to the lightest, the decision thresholds and the Q15 Gaussian
 * detail kernel) and a copy of the levels; it must outlive the panel and
 * every converter using it, and needs no particular alignment or contents.
 * Nothing is allocated. Init uses only deterministic arithmetic, so the
 * same config gives the same panel on every target.
 *
 * The panel offers three presets: Default (tone 0.5), Soft (tone 0) and
 * Deep (tone 1), each with detail 0.75, adapt 0.5 (ignored on grey),
 * vibrance 1 (ignored on grey), veil 0 and lift 0.
 *
 * @param cfg     The panel to build.
 * @param storage Storage for the panel.
 * @param size    Bytes at @p storage.
 * @param out     Set to the panel on success, else to NULL.
 * @param reason  May be NULL. Set to NULL on entry, and on every EPI_ERR_INVALID_ARG
 *                return to a short static NUL-terminated English string saying why
 *                the config or levels were rejected; it stays NULL on EPI_OK and on
 *                EPI_ERR_WORKSPACE_TOO_SMALL. For logs and people: the wording may
 *                change, so don't compare or parse it.
 *
 * @return EPI_OK; EPI_ERR_INVALID_ARG for a NULL pointer, a zero size, a
 *         level count outside 2..16, sigma outside (0, 10.5], levels whose
 *         luminance is not strictly increasing (after rounding to 1/65536
 *         of the lightest), or a darkest level too light for the tone
 *         curve (L* of 66.29 or more, i.e. contrast below about 2.8:1);
 *         EPI_ERR_WORKSPACE_TOO_SMALL if @p size is below
 *         epi_grey_panel_size(). @p *out is NULL on error.
 */
epi_err_t epi_grey_panel_init(
    const epi_grey_panel_config_t* cfg,
    void* storage,
    size_t size,
    const epi_panel_t** out,
    const char** reason
);

/**
 * @brief Progress callback of epi_colour_panel_init().
 *
 * Called regularly during the slow part of init, with @p done counting up from 1
 * and reaching @p total on success; divide by @p total, which is not a fixed
 * number. It may yield or delay the calling task. It must not touch the storage
 * that init is filling or change the config, and it runs on init's stack, on top
 * of its stack use. It has no effect on the panel built.
 *
 * @param ctx   The config's @c progress_ctx.
 * @param done  Steps finished so far.
 * @param total Steps in all.
 * @return true to continue; false to cancel, whereupon init returns
 *         #EPI_ERR_CANCELLED (also when @p done equals @p total).
 */
typedef bool (*epi_progress_fn_t)(void* ctx, unsigned done, unsigned total);

/**
 * @brief A knot of a colour panel's neutral-cast correction.
 *
 * The offset added to the target's ink-relative CIELAB a* and b* at final L*
 * @c lstar, so that greys built from coloured inks look neutral on the glass.
 * Between knots the offset is interpolated linearly in L*; below the first knot
 * and above the last it is zero. Saturated colours take none of it: the offset
 * fades out between chroma 12 and 32.
 */
typedef struct {
    float lstar; /**< Final L*, in [0, 100], strictly ascending across the knots. */
    float a;     /**< a* offset, in [−30, 30]. */
    float b;     /**< b* offset, in [−30, 30]. */
} epi_neutral_knot_t;

/**
 * @brief A colour panel described by its measured inks (see
 *        epi_colour_panel_init()).
 */
typedef struct {
    /** Glass or mode name; not copied, so it must outlive the panel. */
    const char* name;
    /** Native size in pixels, each ≥ 1. */
    uint16_t width, height;
    /**
     * @c ink_count inks, in the order the panel should match them (ties go to
     * the earlier ink); codes are free (they are what the panel is sent).
     * Copied by epi_colour_panel_init(), so this array may be freed afterwards.
     */
    const epi_ink_t* inks;
    /** 5..16: four inks can't surround the neutral axis. */
    size_t ink_count;
    /** Detail blur sigma in panel pixels, in (0, 10.5] (radius ⌈3σ⌉ ≤ 32). */
    float detail_sigma;
    /** Optional; NULL for none. Called during init (see #epi_progress_fn_t).
     *  Ignored by epi_colour_panel_size(). */
    epi_progress_fn_t progress;
    /** Passed to @c progress. */
    void* progress_ctx;
    /**
     * Optional neutral-cast correction (see #epi_neutral_knot_t): @c neutral_count
     * knots, or NULL and 0 for none. The panel keeps only a table derived from
     * them, so this array may be freed after epi_colour_panel_init().
     */
    const epi_neutral_knot_t* neutral;
    /** 0..16. */
    size_t neutral_count;
} epi_colour_panel_config_t;

/**
 * @brief Storage bytes epi_colour_panel_init() needs for @p cfg, or 0 if the
 *        structure of @p cfg is invalid.
 *
 * Checks the pointers, sizes, ink count, sigma and neutral knots. The inks are validated by
 * epi_colour_panel_init(), because that check is its slow part, so a non-zero
 * size does not mean the inks are accepted. Includes alignment slack, so any
 * storage pointer may be passed.
 */
size_t epi_colour_panel_size(const epi_colour_panel_config_t* cfg);

/**
 * @brief Build a colour panel in caller-provided storage from measured inks.
 *
 * The storage holds the #epi_panel_t, its derived tables (the ink hull, the
 * adaptation and OKLab matrices, the cusp table, the inks in linear sRGB and
 * OKLab, the neutral-cast table and the Q15 Gaussian detail kernel) and a copy
 * of the inks; it must
 * outlive the panel and every converter using it, and needs no particular
 * alignment or contents. Its contents are unspecified after an error.
 * @c cfg->inks may point into the storage, to re-init from a panel's own inks.
 * Nothing is allocated. Init uses only deterministic arithmetic and no libm, so
 * the same config gives the same panel on every target.
 *
 * Deriving the cusp table costs about 400,000 in-gamut tests in double. On a target
 * without a double-precision FPU init therefore blocks for seconds (about 6 s
 * on a 240 MHz ESP32-S3), without yielding unless @c cfg->progress does. Firmware for such a target should call it
 * once at startup and have @c progress yield or feed the task watchdog, or
 * generate a const panel with tools/panelgen. Inks that are rejected are usually
 * rejected much sooner. Init uses about 2.4 KB of stack, and @c progress runs on
 * top of it.
 *
 * The panel offers four presets: Default (tone 0.5), Soft (tone 0), Deep (tone 1)
 * and Bright (tone 0.5, lift 16), each with detail 0.75, adapt 0.5, vibrance 1,
 * veil 0 and lift 8 unless stated.
 *
 * @param cfg     The panel to build.
 * @param storage Storage for the panel.
 * @param size    Bytes at @p storage.
 * @param out     Set to the panel on success, else to NULL.
 * @param reason  May be NULL. Set to NULL on entry, and on every EPI_ERR_INVALID_ARG
 *                return to a short static NUL-terminated English string saying why
 *                the config or inks were rejected; it stays NULL on EPI_OK and on
 *                EPI_ERR_WORKSPACE_TOO_SMALL and EPI_ERR_CANCELLED. For logs and
 *                people: the wording may change, so don't compare or parse it.
 *
 * @return EPI_OK; EPI_ERR_INVALID_ARG for a NULL pointer (@c cfg is judged
 *         first, then @p storage and @p out), a zero size, an ink count outside
 *         5..16, sigma outside (0, 10.5], more than 16 neutral knots, a NULL
 *         @c neutral with a non-zero count or the reverse, a knot outside the
 *         ranges of #epi_neutral_knot_t (NaN included) or knots whose L* do not
 *         strictly ascend, or inks that can't make a panel: inks
 *         not spanning three dimensions; a white ink (the largest measured Y)
 *         whose nominal Y is below another ink's; a black ink (the smallest
 *         measured Y) whose nominal Y is above another ink's; a white ink with
 *         X, Y or Z at or below 512/65536, or with a zero channel in linear sRGB;
 *         a black ink not below white's Y; an ink with twice or more of
 *         a cone response of the white ink's; a neutral axis, the line through
 *         the origin and white, that misses the hull; a neutral range whose
 *         darkest L* is below 0 or 66.29 or more; a probe at chroma 200 inside
 *         the hull; or a hue whose most saturated in-hull colour is within 0.5 L*
 *         of the ends of the neutral range;
 *         EPI_ERR_WORKSPACE_TOO_SMALL if @p size is below
 *         epi_colour_panel_size(), before the inks are looked at;
 *         EPI_ERR_CANCELLED if @c cfg->progress returned false. @p *out is NULL
 *         on error.
 */
epi_err_t epi_colour_panel_init(
    const epi_colour_panel_config_t* cfg,
    void* storage,
    size_t size,
    const epi_panel_t** out,
    const char** reason
);

/* ---- Converter: panel-sized RGB rows in, ink-code rows out --------------- */

/** @brief Converter configuration. */
typedef struct {
    const epi_panel_t* panel;
    /** Rendering intent; NULL selects the panel's default preset. */
    const epi_intent_t* intent;
    /** Image size in pixels (rows are pushed top to bottom). */
    uint16_t width, height;
} epi_config_t;

/** @brief Converter state (opaque; lives in the caller's workspace). */
typedef struct epi_converter epi_converter_t;

/**
 * @brief Workspace bytes needed for @p cfg, or 0 if @p cfg is invalid.
 *
 * Depends on the panel and the width only (not the height). Includes
 * alignment slack, so any workspace pointer may be passed to
 * epi_converter_init(). The intent's value ranges are checked by
 * epi_converter_init(), not here.
 */
size_t epi_converter_workspace_size(const epi_config_t* cfg);

/**
 * @brief Initialise a converter in caller-provided memory.
 *
 * All validation happens here. The library never allocates; the workspace
 * holds the whole converter and must outlive it (as must @c cfg->panel).
 * The workspace needs no particular alignment or contents.
 *
 * @return EPI_OK; EPI_ERR_INVALID_ARG for a NULL pointer, a zero width or
 *         height, a panel without valid tables (a built-in calibration or
 *         one from epi_colour_panel_init() or epi_grey_panel_init()), or an
 *         intent value out of range;
 *         EPI_ERR_WORKSPACE_TOO_SMALL if @p size is below
 *         epi_converter_workspace_size(). @p *out is NULL on error.
 */
epi_err_t epi_converter_init(const epi_config_t* cfg, void* workspace, size_t size, epi_converter_t** out);

/**
 * @brief Push the next input row (@c width pixels), top to bottom.
 *
 * Output rows lag input rows by the detail blur's radius r (18 on the
 * EL133UF1): output row y is ready once input row min(y + r, height − 1)
 * has been pushed, so pushing the last row releases all remaining rows.
 *
 * Contract: call epi_converter_pop() until it returns EPI_ERR_NOT_READY (or
 * EPI_ERR_FINISHED) after every push. A push the converter cannot hold
 * because earlier rows were not drained is a programming error: it asserts
 * in debug builds and is rejected (no memory is touched, push returns
 * EPI_ERR_NOT_DRAINED) otherwise. Pushes after @c height rows return
 * EPI_ERR_FINISHED, without asserting.
 *
 * @return EPI_OK if the row was accepted; EPI_ERR_INVALID_ARG for a NULL
 *         argument; EPI_ERR_FINISHED for a push past the last row;
 *         EPI_ERR_NOT_DRAINED for a push rejected because earlier rows were
 *         not drained.
 */
epi_err_t epi_converter_push(epi_converter_t* c, const epi_rgb_t* row);

/**
 * @brief Pop the next output row into @p out_row, if one is ready.
 *
 * @p out_row receives @c width bytes, one ink code per pixel. Pass them to
 * epi_preview_push() for a preview and to epi_pack_row() for the panel.
 *
 * @return EPI_OK if a row was written; EPI_ERR_INVALID_ARG for a NULL
 *         argument; EPI_ERR_FINISHED if all @c height rows have been popped;
 *         EPI_ERR_NOT_READY if none is ready yet.
 */
epi_err_t epi_converter_pop(epi_converter_t* c, uint8_t* out_row);

/* ---- Resampler: source rows in, panel-sized rows out --------------------- */

/**
 * @brief Resampling filter: the kernel and its support (0.5, 1, 2 and 3
 *        source pixels, scaled by the reduction factor when shrinking).
 *        BOX is 1 on (−0.5, 0.5]; BILINEAR is 1 − |x|; BICUBIC is the Keys
 *        cubic with a = −0.5; LANCZOS3 is sinc(x)·sinc(x/3).
 */
typedef enum {
    EPI_FILTER_BOX,
    EPI_FILTER_BILINEAR,
    EPI_FILTER_BICUBIC,
    EPI_FILTER_LANCZOS3,
} epi_filter_t;

/** @brief The default limit on the source area, in pixels (100 megapixels). */
#define EPI_DEFAULT_MAX_SRC_PIXELS 100000000u

/** @brief The largest source width or height the resampler accepts. */
#define EPI_MAX_SRC_DIMENSION 65535u

/**
 * @brief Resampler configuration.
 *
 * The source rectangle is resampled to dst_w × dst_h by separable
 * convolution: a horizontal pass, rounded and clamped to 8 bits, then a
 * vertical pass. Along each axis, scale = rectangle extent / output size;
 * output pixel i is centred at src_x + (i + 0.5)·scale and reads the source
 * pixels within the filter's support of it (scaled by max(1, scale)), with
 * weights normalised to sum 1 and held in 22-bit fixed point. An axis at
 * scale 1 from offset 0, or a crop at integer offsets the size of the
 * output, is copied. When src_height > 100·src_width and the height
 * shrinks, the vertical pass runs first. The result is pasted at
 * (dst_x, dst_y) on a dst_width × dst_height canvas of @c background. The
 * output is byte-identical on every platform.
 */
typedef struct {
    /**
     * Source image size, ≥ 1 and at most #EPI_MAX_SRC_DIMENSION, with
     * width × height within @c max_src_pixels. These are 32-bit so that a
     * 32-bit declared size can be assigned directly: a large or negative
     * value is rejected, not wrapped to a valid size.
     */
    uint32_t src_width, src_height;
    /**
     * Source rectangle to resample (crop / zoom / pan), in source pixels:
     * finite, src_x, src_y ≥ 0, src_w, src_h > 0, and
     * src_x + src_w ≤ src_width, src_y + src_h ≤ src_height (sums in float).
     */
    float src_x, src_y, src_w, src_h;
    /** Output (panel) size, ≥ 1. */
    uint16_t dst_width, dst_height;
    /**
     * Placement of the resampled dst_w × dst_h rectangle (each ≥ 1) in the
     * output. It may overhang or miss the output entirely; it is clipped,
     * and only its visible part is computed.
     */
    int32_t dst_x, dst_y;
    uint16_t dst_w, dst_h;
    /** Output area outside the placement. */
    epi_rgb_t background;
    epi_filter_t filter;
    /**
     * The largest source area accepted, src_width × src_height. 0 means
     * #EPI_DEFAULT_MAX_SRC_PIXELS; UINT32_MAX allows every size up to
     * 65535×65535. Zero-initialise the config (for example
     * `epi_resampler_config_t cfg = {0};`) so this field is not left
     * indeterminate.
     */
    uint32_t max_src_pixels;
} epi_resampler_config_t;

/** @brief Resampler state (opaque; lives in the caller's workspace). */
typedef struct epi_resampler epi_resampler_t;

/**
 * @brief Workspace bytes needed for @p cfg, or 0 if @p cfg is invalid or the
 *        source is too large (see #epi_resampler_config_t).
 *
 * Covers the horizontal coefficient table (each visible column's taps), a ring
 * of resampled rows as tall as the largest vertical window, a scratch array
 * of one double per tap of the larger window, and alignment slack (any start
 * address works). For 4000×3000 → 1200×1600 cover: about
 * 21 KB (BOX) to 102 KB (LANCZOS3).
 *
 * The size grows with the source dimensions, because shrinking widens the
 * filter windows: about 2 MB for 65535×65535 → 1200×1600 with LANCZOS3 (with
 * the limit raised). A long, thin image needs a large workspace for its area.
 * For an image from an untrusted source, set the source size from the file's
 * declared size before decoding, and check this size against your memory
 * budget before allocating.
 *
 * Time: when shrinking, the horizontal pass costs about 3 (BOX), 6
 * (BILINEAR), 12 (BICUBIC) or 18 (LANCZOS3) multiply-adds for each source
 * pixel it reads.
 */
size_t epi_resampler_workspace_size(const epi_resampler_config_t* cfg);

/**
 * @brief Initialise a resampler in caller-provided memory.
 *
 * @return EPI_OK; EPI_ERR_INVALID_ARG if @p cfg is invalid (see
 *         #epi_resampler_config_t) or a pointer is NULL;
 *         EPI_ERR_SOURCE_TOO_LARGE if the source is over the limit;
 *         EPI_ERR_WORKSPACE_TOO_SMALL if @p size is below
 *         epi_resampler_workspace_size(). @p *out is NULL on error.
 */
epi_err_t epi_resampler_init(const epi_resampler_config_t* cfg, void* workspace, size_t size, epi_resampler_t** out);

/**
 * @brief Push the next source row (@c src_width pixels), top to bottom.
 *
 * Only rows the output reads are processed; the others (outside the source
 * rectangle's vertical window, or below the visible placement) are counted
 * and skipped. Pushing the last source row releases every remaining output
 * row.
 *
 * Contract: call epi_resampler_pop() until it returns EPI_ERR_NOT_READY (or
 * EPI_ERR_FINISHED) after every push. A push the ring cannot hold because
 * earlier rows were not drained is a programming error: it asserts in debug
 * builds and is rejected (no memory is touched, push returns
 * EPI_ERR_NOT_DRAINED) otherwise. Pushes after @c src_height rows return
 * EPI_ERR_FINISHED, without asserting.
 *
 * @return EPI_OK if the row was accepted, including a row counted and
 *         skipped because it lies outside the window; EPI_ERR_INVALID_ARG
 *         for a NULL argument; EPI_ERR_FINISHED for a push past the last
 *         source row; EPI_ERR_NOT_DRAINED for a push rejected because
 *         earlier rows were not drained.
 */
epi_err_t epi_resampler_push(epi_resampler_t* r, const epi_rgb_t* src_row);

/**
 * @brief Pop the next output row (@c dst_width pixels), if one is ready.
 *
 * Rows come out top to bottom. A background row above the placement is
 * ready at once (even before any push); a placement row as soon as the last
 * source row of its vertical window has been pushed; rows below the
 * placement right after the last placement row.
 *
 * @return EPI_OK if a row was written; EPI_ERR_INVALID_ARG for a NULL
 *         argument; EPI_ERR_FINISHED if all @c dst_height rows have been
 *         popped; EPI_ERR_NOT_READY if none is ready yet.
 *
 * Typical loop (the converter must be drained after every converter push,
 * not once per source row, because one source row can release many output
 * rows):
 * @code
 * for (each source row src) {
 *     epi_resampler_push(r, src);
 *     while (epi_resampler_pop(r, rrow) == EPI_OK) {
 *         epi_converter_push(c, rrow);
 *         while (epi_converter_pop(c, out) == EPI_OK) {
 *             // use the output row
 *         }
 *     }
 * }
 * @endcode
 */
epi_err_t epi_resampler_pop(epi_resampler_t* r, epi_rgb_t* dst_row);

/**
 * @brief Fill the source and placement rectangles to cover the output:
 *        scale to the larger of the two ratios and centre-crop.
 *
 * Reads src_width/src_height/dst_width/dst_height; writes src_x..src_h and
 * dst_x..dst_h. With scale = max(dst_width/src_width, dst_height/src_height)
 * (float), the axis that sets the scale takes the whole source extent
 * exactly, and the other takes dst/scale (at most the source extent),
 * centred: (src − size)/2. The placement is the whole output. Zero sizes
 * leave @p cfg unchanged. It does not check the source size limit; the
 * resampler does.
 */
void epi_layout_cover(epi_resampler_config_t* cfg);

/**
 * @brief Fill the source and placement rectangles to fit inside the output:
 *        scale to the smaller ratio and centre on the background.
 *
 * The source rectangle is the whole source. With
 * scale = min(dst_width/src_width, dst_height/src_height) (float),
 * dst_w = round(src_width·scale), half away from zero, kept in
 * [1, dst_width]; dst_x = (dst_width − dst_w)/2, rounded down; likewise y.
 * Zero sizes leave @p cfg unchanged. It does not check the source size limit;
 * the resampler does.
 */
void epi_layout_fit(epi_resampler_config_t* cfg);

/* ---- Preview reducer: ink-code rows in, smaller RGB rows out ------------- */

/**
 * @brief Preview reducer configuration.
 *
 * The reducer averages an image of ink codes down to a preview in linear light,
 * so fine dither keeps the brightness it has on the panel. On each axis, source
 * pixel i covers [i·dst, (i+1)·dst) and preview pixel j covers
 * [j·src, (j+1)·src). A preview pixel is the mean of its source pixels' linear
 * colours (each ink's sRGB8 colour through the sRGB EOTF, Q16), weighted by the
 * exact integer overlaps and rounded to the nearest integer, halves up. Its
 * value is the sRGB8 code nearest in linear light; a mean midway between two
 * codes takes the lower. A block of one ink gives exactly that ink's colour, and
 * a preview the size of the source is exactly the inks' colours. No floating
 * point is used, so the output is byte-identical on every platform.
 */
typedef struct {
    /** Supplies the inks' colours; read only by epi_preview_init(). */
    const epi_panel_t* panel;
    /** Size of the ink-code image in pixels, each ≥ 1. */
    uint16_t src_width, src_height;
    /** Size of the preview in pixels, each from 1 up to the source's. The axes
     *  are independent, so the aspect ratio may change. */
    uint16_t dst_width, dst_height;
    /** Colours: false for the inks' measured colours (what the panel shows), true
     *  for their nominal colours. */
    bool nominal;
} epi_preview_config_t;

/** @brief Preview reducer state (opaque; lives in the caller's workspace). */
typedef struct epi_preview epi_preview_t;

/**
 * @brief Workspace bytes needed for @p cfg, or 0 if @p cfg is invalid.
 *
 * Depends on dst_width only: about 3.4 KB of state, mostly a table of the inks'
 * linear colours, and 27 bytes per preview column. Includes alignment slack, so
 * any workspace pointer may be passed to epi_preview_init().
 */
size_t epi_preview_workspace_size(const epi_preview_config_t* cfg);

/**
 * @brief Initialise a preview reducer in caller-provided memory.
 *
 * The library never allocates; the workspace holds the whole reducer and must
 * outlive it. The workspace needs no particular alignment or contents.
 *
 * @return EPI_OK; EPI_ERR_INVALID_ARG for a NULL pointer, a panel with no inks,
 *         a zero size, or a preview larger than the source on either axis;
 *         EPI_ERR_WORKSPACE_TOO_SMALL if @p size is below
 *         epi_preview_workspace_size(). @p *out is NULL on error.
 */
epi_err_t epi_preview_init(const epi_preview_config_t* cfg, void* workspace, size_t size, epi_preview_t** out);

/**
 * @brief Push the next row of @c src_width ink codes (as epi_converter_pop()
 *        gives them), top to bottom.
 *
 * One push finishes at most one preview row. A row holding a code that is not
 * one of the panel's inks is rejected and leaves the reducer unchanged.
 *
 * Contract: call epi_preview_pop() until it stops returning EPI_OK after every
 * push. A push while a finished row is undrained is a programming error: it
 * asserts in debug builds and is rejected (no memory is touched, push returns
 * EPI_ERR_NOT_DRAINED) otherwise. Pushes after @c src_height rows return
 * EPI_ERR_FINISHED, without asserting.
 *
 * @return EPI_OK if the row was accepted; EPI_ERR_INVALID_ARG for a NULL
 *         argument or a code that is not one of the panel's inks;
 *         EPI_ERR_FINISHED for a push past the last source row;
 *         EPI_ERR_NOT_DRAINED for a push rejected because a finished row was
 *         not drained.
 */
epi_err_t epi_preview_push(epi_preview_t* p, const uint8_t* codes);

/**
 * @brief Pop the next preview row (@c dst_width pixels), if one is ready.
 *
 * @return EPI_OK if a row was written; EPI_ERR_INVALID_ARG for a NULL argument;
 *         EPI_ERR_FINISHED if all @c dst_height rows have been popped;
 *         EPI_ERR_NOT_READY if none is ready yet.
 */
epi_err_t epi_preview_pop(epi_preview_t* p, epi_rgb_t* dst_row);

/* ---- Packer: ink-code rows in, panel rows out ---------------------------- */

/** @brief Packed row format, by bits per pixel. */
typedef enum {
    /** Two codes per byte, high nibble first; codes below 16. */
    EPI_PACK_4BPP,
    /** Four codes per byte, most significant bits first; codes below 4. */
    EPI_PACK_2BPP,
    /** Eight codes per byte, most significant bit first; codes below 2. */
    EPI_PACK_1BPP,
} epi_pack_format_t;

/**
 * @brief Bytes in one packed row of @p format at @p width (0 for an invalid
 *        @p format).
 */
size_t epi_pack_row_size(epi_pack_format_t format, uint16_t width);

/**
 * @brief Pack one row of ink codes (as epi_converter_pop() gives them) for the
 *        panel.
 *
 * Stateless. @p out_row receives epi_pack_row_size() bytes, and a partial last
 * byte is padded with the code of the panel's lightest ink. Nothing is written
 * on error.
 *
 * @return EPI_OK; EPI_ERR_INVALID_ARG for a NULL pointer, a zero width, a panel
 *         with no inks, an invalid @p format, or a code too wide for the format,
 *         whether in @p codes or among the panel's inks.
 */
epi_err_t epi_pack_row(
    const epi_panel_t* panel,
    epi_pack_format_t format,
    const uint8_t* codes,
    uint16_t width,
    uint8_t* out_row
);

#ifdef __cplusplus
}
#endif

#endif /* EPAPER_IMAGE_H */
