/*
 * Every grey panel's presets, runtime or generated (docs/design.md, "Grey
 * panels"). Their own translation unit, so a const grey panel, which references
 * them, does not link epi_grey_panel_init.
 */
#include "epaper_image.h"
#include "epi_panel_tables.h"

#define GREY_INTENT(t) {.tone = (t), .detail = 0.75f, .adapt = 0.5f, .vibrance = 1.0f, .veil = 0.0f, .lift = 0.0f}

static const epi_intent_t grey_intent_default = GREY_INTENT(0.5f);
static const epi_intent_t grey_intent_soft = GREY_INTENT(0.0f);
static const epi_intent_t grey_intent_deep = GREY_INTENT(1.0f);

const epi_preset_t epi_grey_presets[EPI_GREY_PRESET_COUNT] = {
    {"Default", &grey_intent_default},
    {"Soft", &grey_intent_soft},
    {"Deep", &grey_intent_deep},
};
