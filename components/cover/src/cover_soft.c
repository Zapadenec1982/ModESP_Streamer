/**
 * @file cover_soft.c
 * @brief The software JPEG decoder, for the covers the hardware engine will not read.
 *
 * The P4's JPEG engine decodes baseline JPEG and nothing else — a progressive file makes it
 * report "SOS encountered before SOF0", because it never finds the SOF0 marker it is looking
 * for. Progressive is what a lot of ripping software writes, so a player that cannot read it
 * is a player that shows no artwork on whole albums.
 *
 * stb_image reads both, is one file, and is public domain. It is slow next to the hardware —
 * a few hundred milliseconds for a large cover — which is exactly why it is the FALLBACK and
 * not the path: the fast engine is tried first and this runs only when it refuses.
 *
 * Source: https://github.com/nothings/stb (stb_image.h, MIT or public domain, v2.30).
 * Vendored rather than fetched so the firmware build needs no network, as with the fonts.
 */

#include "esp_heap_caps.h"

// JPEG plus PNG (used by MediaServer artwork). No stdio: callers fetch bounded bytes.
#define STBI_ONLY_JPEG
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#define STBI_NO_LINEAR
#define STBI_NO_HDR
#define STBI_NO_GIF
#define STBI_NO_PSD
#define STBI_NO_PIC
#define STBI_NO_PNM
#define STBI_ASSERT(x) ((void)0)

// Bound the DEFLATE blocks stb will walk in a PNG's IDAT. stb rebuilds both fixed Huffman
// tables for every block (stbi__parse_zlib), so a crafted PNG of empty blocks spends CPU in
// proportion to its byte length, not its size, and can keep this core-0 task busy past the
// 5 s task watchdog. cover.cpp refuses anything over
// MAX_SIDE (2048) a side before decoding, so a PNG here has at most ~2048 rows; an encoder
// that flushes a block every row still fits several times over inside this cap.
#define STBI_MAX_DEFLATE_BLOCKS 8192

// A progressive cover needs several megabytes of coefficients. That is PSRAM's job; the
// internal heap is worth more than the cache misses cost.
static void* cover_malloc(size_t n) {
    return heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}
static void* cover_realloc(void* p, size_t n) {
    return heap_caps_realloc(p, n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}
#define STBI_MALLOC(sz)        cover_malloc(sz)
#define STBI_REALLOC(p, newsz) cover_realloc(p, newsz)
#define STBI_FREE(p)           free(p)

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

int cover_soft_info(const unsigned char* jpg, int len, int* w, int* h) {
    int comp = 0;
    return stbi_info_from_memory(jpg, len, w, h, &comp);
}

unsigned char* cover_soft_decode(const unsigned char* jpg, int len, int* w, int* h) {
    int comp = 0;
    return stbi_load_from_memory(jpg, len, w, h, &comp, 3);   // forced to RGB, 8 bits each
}

void cover_soft_free(unsigned char* px) { stbi_image_free(px); }

const char* cover_soft_reason(void) { return stbi_failure_reason(); }
