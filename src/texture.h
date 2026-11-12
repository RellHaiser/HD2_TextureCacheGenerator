// The texture cache files of LS3DF.dll's ITexture::Create (0x10073a80): which file a texture load
// writes (.DX1 / .DX2 / .DX3 / .565 / .555) and its exact contents, computed without Direct3D.
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace tcg {

// ITexture::Create / IGraph::LoadTexture flags (ITC_*), as far as they matter here.
enum : uint32_t {
    ITC_FROM_MEMORY = 0x00000001,
    ITC_SOURCE_ALPHA = 0x00000002,      // the image's own alpha (32-bit; 16-bit: its top bit)
    ITC_USE_ALPHA = 0x00000004,         // a separate opacity map
    ITC_ALPHA_1BIT = 0x00000008,        // colour key
    ITC_STREAMED = 0x00000040,          // texture thread: no cache files
    ITC_DITHER = 0x00000080,
    ITC_MIPMAP = 0x00000100,
    ITC_COMPRESS = 0x00000200,
    ITC_TRUECOLOR = 0x00000400,         // A8R8G8B8: no cache files
    ITC_KEEP_PIXELS = 0x00001000,
    ITC_HALF = 0x00002000,
    ITC_EMPTY = 0x00008000,
    ITC_DEFAULT = 0x00010000,
    ITC_NOCACHE = 0x00200000,
    ITC_NORESIZE = 0x00400000,
    // Options a .dds file does not take (LoadTexture clears them).
    ITC_DDS_IGNORED = ITC_DITHER | ITC_MIPMAP | ITC_COMPRESS | ITC_TRUECOLOR | ITC_HALF,
};

enum class Format { Unknown, R5G6B5, X1R5G5B5, A1R5G5B5, A4R4G4B4, A8R8G8B8, DXT1, DXT3 };
const char *FormatName(Format f);

// What IGraph::Init found on the display adapter. Modern adapters (and the usual Direct3D 8
// wrappers) give the defaults.
struct Device {
    Format fmt16 = Format::R5G6B5;      // g_fmt16Bit: the first of R5G6B5, X1R5G5B5, A1R5G5B5
    bool dxt = true;                    // DXT1 and DXT3 textures (and IGCAPS_DXT)
    uint32_t maxWidth = 16384;          // the adapter's maximum texture size
    uint32_t maxHeight = 16384;
};

// One IGraph::LoadTexture call: the names as the caller gives them (a material's texture and
// opacity map names) and the ITC_ flags.
struct TextureLoad {
    std::string name;
    std::string alphaName;              // "" for none
    uint32_t flags = 0;
};

// Finds a game-relative path ("Maps\\X.BMP") among the data folders; returns the file's path on
// disk or "" if there is no such file.
using FileLocator = std::function<std::string(const std::string &relativePath)>;
// IGraph's texture search (FindTextureFile 0x1006fbd0): the game-relative path of a texture file
// name, "" if not found.
using TextureFinder = std::function<std::string(const std::string &fileName)>;

enum class Outcome {
    Cache,                              // the load writes this cache file
    NoCache,                            // the load succeeds but writes no cache file
    Error,                              // the load fails in the game
};

struct CacheResult {
    Outcome outcome = Outcome::Error;
    std::string message;                // why NoCache / Error
    std::string cachePath;              // game-relative, e.g. "Maps\\X_8L.DX1"
    std::string imagePath;              // game-relative paths the texture was loaded from
    std::string alphaPath;
    std::string imageFile;              // ... and those files on disk
    std::string alphaFile;
    uint32_t flags = 0;                 // ITC_ flags in effect (ITexture::flags)
    Format format = Format::Unknown;
    int kind = 0;                       // 0 uncompressed, 1 DXT1, 2 DXT1 with 1-bit alpha, 3 DXT3
    uint16_t width = 0, height = 0, levels = 0;
    std::vector<uint8_t> bytes;         // the cache file (when asked for)
    std::vector<std::string> warnings;
};

struct BuildOptions {
    bool contents = true;               // compute the file (else only its name)
    bool resetEncoder = true;           // start the S3TC encoder from a fresh DLL's state
};

// IGraph::LoadTexture + ITexture::Create for one texture: the cache file it writes.
CacheResult BuildCache(const TextureLoad &load, const Device &device, const TextureFinder &find,
                       const FileLocator &locate, const BuildOptions &options);

// ITexture's cache file name (0x10073f00 inlined): the image path without its extension, for DXT3
// "_" and the opacity map's file name without extension, "_" levels, "L" for ITC_HALF, then
// ".DX<kind>" or ".565" / ".555".
std::string CacheFileName(const std::string &imagePath, const std::string &alphaFileName, int kind, uint16_t levels,
                          bool half, Format format);

// The header of a cache file.
struct CacheHeader {
    char magic[4] = {};
    uint16_t version = 0;
    uint32_t width = 0, height = 0;
    uint64_t time = 0, alphaTime = 0;
    bool hasAlphaTime = false;
    size_t dataOffset = 0;
};
bool ParseCacheHeader(const std::vector<uint8_t> &file, bool dxt3, CacheHeader *header);

}  // namespace tcg
