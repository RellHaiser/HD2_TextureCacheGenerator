// The S3TC (DXT) compressor statically linked into LS3DF (S3 Inc.'s S3TC library: S3TCencode
// 0x10080ed0 and its helpers up to 0x10086500), used by ITexture to compress textures.
#pragma once

#include <cstddef>
#include <cstdint>

namespace ls3d::s3tc {

// The DDSURFACEDESC fields the encoder uses (0x6C bytes, DirectDraw layout).
struct S_surfaceDesc {
    uint32_t size;                  // +0x00 0x6C
    uint32_t flags;                 // +0x04
    uint32_t height;                // +0x08
    uint32_t width;                 // +0x0C
    int32_t pitch;                  // +0x10
    uint32_t backBufferCount;       // +0x14
    uint32_t mipMapCount;           // +0x18
    uint32_t alphaBitDepth;         // +0x1C
    uint32_t reserved;              // +0x20
    void *surface;                  // +0x24
    uint32_t colorKeys[6];          // +0x28 dest overlay / dest blt / src overlay
    uint32_t keyLow;                // +0x40 source blt colour key range (encode type 1)
    uint32_t keyHigh;               // +0x44
    uint32_t pfSize;                // +0x48 DDPIXELFORMAT
    uint32_t pfFlags;               // +0x4C
    uint32_t pfFourCC;              // +0x50
    uint32_t bitCount;              // +0x54 (the encode type in a compressed description)
    uint32_t mask[4];               // +0x58 red, green, blue, alpha
    uint32_t caps;                  // +0x68
};
static_assert(sizeof(S_surfaceDesc) == 0x6C, "DDSURFACEDESC layout");

// Encode types: the low byte selects the colour blocks' transparency, the high byte an alpha block.
enum : uint32_t {
    S3TC_OPAQUE = 0x000,            // DXT1, every pixel opaque
    S3TC_COLOR_KEY = 0x001,         // DXT1, pixels in [keyLow, keyHigh] transparent
    S3TC_ALPHA_KEY = 0x002,         // DXT1, pixels at or below g_alphaThreshold transparent
    S3TC_ALPHA_EXPLICIT = 0x100,    // DXT3: 4 bits of alpha per pixel
    S3TC_ALPHA_INTERPOLATED = 0x200,// DXT5
};

using ProgressFn = int(__cdecl *)(float percent, void *context);

// Compresses `source` (16 / 32-bit pixels with the masks of its pixel format, or 8-bit indices
// into `palette`'s 4-byte entries) into `bits` and describes the result in `dest`. `weights`: the
// error weights of red, green, blue. 0x10080f00 (S3TCencode 0x10080ed0: no progress callback).
// Returns 0, or -1 when the callback asks to stop.
int Encode(const S_surfaceDesc *source, const uint8_t *palette, S_surfaceDesc *dest, void *bits, uint32_t type,
           const float *weights, ProgressFn progress = nullptr, void *context = nullptr);

// (HD2_TextureCacheGenerator) puts this thread's encoder state back to the state of a freshly
// loaded LS3DF.dll. The original never resets it: a few values carry over from one block (and one
// texture) to the next, so a texture's blocks can depend on what was compressed before it.
void ResetState();

// (Test hook) the library's work area, a byte-exact mirror of the original's 0x100d2cd8 - 0x100d7340,
// and the two best-ratio denominators of its initialised data (0x100a99a0, 0x100a99a8).
const void *DebugWorkArea(size_t *size, const double *bestDens[2]);
// (Test hook) the alpha block's 1-D fit (0x10085a80).
double DebugFitAlpha(int levels, int n, const double *values, int *ends, int *indices);

// The alpha level of S3TC_ALPHA_KEY, 0 - 255 (0x100d2cd0; nothing in LS3DF sets it).
extern uint32_t g_alphaThreshold;

}  // namespace ls3d::s3tc
