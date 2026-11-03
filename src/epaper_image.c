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
    }
    return "unknown error";
}
