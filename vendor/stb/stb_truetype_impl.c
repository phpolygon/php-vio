/*
 * stb_truetype implementation - compiled once as a single translation unit
 *
 * Override STBTT_DEF to use hidden visibility, preventing symbol collisions
 * with other extensions (e.g. php-glfw) that also bundle stb_truetype.
 * The symbols remain accessible within this shared object but are not
 * exported to the dynamic linker.
 */
#ifdef _MSC_VER
#define STBTT_DEF
#else
#ifndef STBTT_DEF
#define STBTT_DEF __attribute__((visibility("hidden")))
#endif
#endif
#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"
