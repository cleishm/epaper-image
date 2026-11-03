/* Verifies the public header's extern "C" guards work for C++ consumers. */
#include <epaper_image.h>

int main() {
    const char* msg = epi_strerror(EPI_OK);
    return msg != nullptr ? 0 : 1;
}
