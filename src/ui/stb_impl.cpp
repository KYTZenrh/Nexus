// The single place the stb libraries (public domain / MIT) are compiled.
#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_SIMD
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG  // some homebrew ship a JPEG named icon0.png
#define STB_TRUETYPE_IMPLEMENTATION
#include "../../third_party/stb/stb_image.h"
#include "../../third_party/stb/stb_truetype.h"

// PNG writing is only needed by the PC tools (screenshots, icon export), not on the console.
#ifndef NEXUS_NO_PNG_WRITE
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "../../third_party/stb/stb_image_write.h"
#endif
