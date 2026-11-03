/*
 * Verifies that a header can forward-declare epi_panel_t without including
 * <epaper_image.h>, and that the real header then accepts the redeclaration.
 * Built as both C11 and C++ (see panel_forward_decl_test.cpp).
 */
typedef struct epi_panel epi_panel_t;

static const epi_panel_t* forwarded(const epi_panel_t* panel) {
    return panel;
}

#include <epaper_image.h>

int main(void) {
    return forwarded(&epi_panel_el133uf1) == &epi_panel_el133uf1 ? 0 : 1;
}
