/*
 * stb_rect_pack implementation - compiled once as a single translation unit.
 *
 * Override STBRP_DEF to use hidden visibility, preventing symbol collisions
 * with other extensions that also bundle stb_rect_pack. The symbols remain
 * accessible within this shared object but are not exported to the dynamic
 * linker. Mirrors the stb_truetype_impl.c / STBTT_DEF pattern.
 */
#ifdef _MSC_VER
#define STBRP_DEF
#else
#ifndef STBRP_DEF
#define STBRP_DEF __attribute__((visibility("hidden")))
#endif
#endif
#define STB_RECT_PACK_IMPLEMENTATION
#include "stb_rect_pack.h"
