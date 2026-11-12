// The texture loads of a .4ds model (LS3D version 0x29): its material table read the way
// LS3DF.dll's loader does (ReadModelFile 0x1005d970, I3D_material::Load, I3D_anim_material::Load),
// with the ITC_ flags of I3D_material::TextureFlags for given driver settings.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "texture.h"

namespace tcg {

// The I3D driver states that pick the texture options (DRVSTATE_TEX_*). HD2 sets mip maps on,
// leaves dithering at the driver's default (on) and takes compression, 32-bit textures and half
// size from its setup (C_App 0x006dbd70).
struct DriverStates {
    bool mipmap = true;                 // 0x0D
    bool dither = true;                 // 0x10
    bool trueColor = false;             // 0x11
    bool half = false;                  // 0x15
    bool compress = true;               // 0x19
};

// I3D_material::TextureFlags: a material's (file) flags to ITC_ flags.
uint32_t MaterialTextureFlags(uint32_t materialFlags, const DriverStates &states);
// The flags of a material's environment (second) texture.
uint32_t EnvironmentTextureFlags(uint32_t materialFlags, const DriverStates &states);

struct MaterialLoad {
    int material = 0;                   // index in the model's table
    uint32_t materialFlags = 0;
    bool environment = false;           // the material's second texture
    TextureLoad load;
};

// The texture loads of a model file's materials. Returns an error text or "".
std::string ReadModelTextures(const std::string &file, const DriverStates &states, std::vector<MaterialLoad> *out);

}  // namespace tcg
