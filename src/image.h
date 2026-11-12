// .bmp / .tga files as LS3DF.dll reads them for ITexture::Create (IGraph::GetImageInfo 0x1006fe00,
// LoadImage 0x100775f0), from files on disk instead of the game's .dta archives.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace tcg {

// LoadImage's flags argument: only this ITC_ bit changes how a file is read.
constexpr uint32_t kBmpUnpadded = 0x00040000;   // ITC_BMP_UNPADDED

struct ImageInfo {
    uint16_t width = 0;
    uint16_t height = 0;
    uint8_t bitDepth = 0;
};

// What LoadImage leaves in its BITMAPINFOHEADER and buffers.
struct Image {
    int32_t width = 0;                  // biWidth
    int32_t height = 0;                 // biHeight
    uint16_t bitCount = 0;              // biBitCount
    int32_t pitch = 0;                  // biXPelsPerMeter: the row pitch in bytes
    std::vector<uint8_t> bits;          // rows bottom-up (pitch apart; unpadded after a resize)
    std::vector<uint8_t> palette;       // 256 BGRA entries (0x400 bytes) when the image has one
    uint64_t fileTime = 0;              // FILETIME of the file (dtaGetTime)
};

// The last-write time of a file as a FILETIME; 0 if unknown.
uint64_t FileTimeOf(const std::string &path);

// IGraph::GetImageInfo: the size and bit depth of a .bmp / .tga file. Returns an error text or "".
std::string ReadImageInfo(const std::string &path, ImageInfo *info);

// LoadImage: the image's rows, palette and header; resized to width x height (nearest pixel) when
// both are given and differ. Returns an error text or "". `warnings` gets notes about files the
// game reads differently from an image viewer.
std::string LoadImageFile(const std::string &path, int width, int height, uint32_t flags, Image *image,
                      std::vector<std::string> *warnings);

}  // namespace tcg
