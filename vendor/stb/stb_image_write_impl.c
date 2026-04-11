/*
 * stb_image_write implementation - compiled once as a single translation unit
 *
 * Hidden visibility to avoid symbol collisions with other extensions
 * (e.g. php-glfw) that also bundle stb_image_write.
 */
#ifdef _MSC_VER
#define STBIWDEF
#else
#define STBIWDEF __attribute__((visibility("hidden")))
#endif
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"
