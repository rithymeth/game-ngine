// Provides stb_image's implementation exactly once for the whole engine
// library; every other translation unit includes only the declarations via
// <stb_image.h>.
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>
