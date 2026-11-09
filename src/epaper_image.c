#include "epaper_image.h"

const char* epi_strerror(epi_err_t err) {
    switch (err) {
    case EPI_OK:
        return "success";
    case EPI_ERR_INVALID_ARG:
        return "invalid argument";
    case EPI_ERR_WORKSPACE_TOO_SMALL:
        return "workspace too small";
    case EPI_ERR_NOT_READY:
        return "no output row ready";
    case EPI_ERR_NOT_DRAINED:
        return "earlier rows not drained";
    case EPI_ERR_FINISHED:
        return "all rows already processed";
    case EPI_ERR_SOURCE_TOO_LARGE:
        return "source too large";
    case EPI_ERR_CANCELLED:
        return "cancelled";
    }
    return "unknown error";
}

/* ASCII letters fold to lower case; no locale is consulted. */
static char fold(char c) {
    return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

static bool same_name(const char* a, const char* b) {
    for (; *a && *b; a++, b++) {
        if (fold(*a) != fold(*b)) {
            return false;
        }
    }
    return *a == *b;
}

const epi_intent_t* epi_panel_preset(const epi_panel_t* panel, const char* name) {
    if (panel == NULL || panel->presets == NULL || panel->preset_count == 0) {
        return NULL;
    }
    if (name == NULL) {
        return panel->presets[0].intent;
    }
    for (size_t i = 0; i < panel->preset_count; i++) {
        const char* n = panel->presets[i].name;
        if (n != NULL && same_name(n, name)) {
            return panel->presets[i].intent;
        }
    }
    return NULL;
}
