/*
 * Hidden visibility to avoid symbol collisions with other extensions
 * (e.g. php-glfw) that also bundle stb_image_write.
 */
#define STBIWDEF __attribute__((visibility("hidden")))
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"
