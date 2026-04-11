/*
 * stb_image implementation - compiled once as a single translation unit
 *
 * Hidden visibility to avoid symbol collisions with other extensions
 * (e.g. php-glfw) that also bundle stb_image.
 */
#define STBIDEF __attribute__((visibility("hidden")))
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
