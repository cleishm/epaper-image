/*
 * SHA-256 (FIPS 180-4), for the panel ID (src/epi_panel_id.c). Portable:
 * integer operations only, the same digest on every target and byte order.
 */
#ifndef EPI_SHA256_H
#define EPI_SHA256_H

#include <stddef.h>
#include <stdint.h>

#define EPI_SHA256_LEN 32

/* The SHA-256 digest of the len bytes at data (data may be NULL when len is 0). */
void epi_sha256(const uint8_t* data, size_t len, uint8_t out[EPI_SHA256_LEN]);

#endif /* EPI_SHA256_H */
