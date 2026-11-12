// .bmp / .tga reading as LS3DF.dll does it (IGraph::GetImageInfo 0x1006fe00, LoadImage 0x100775f0;
// after the LS3DF port's igraph_image.cpp and image_load.cpp). The reader trusts the headers the way
// the original does: it does not seek to the BMP's pixel offset, reads TGA files as uncompressed and
// takes 256 colour map entries for any TGA of 8 bits or less. Such files load wrong in the game too;
// the warnings say so.
#include "image.h"

#include <windows.h>

#include <cstdio>
#include <cstring>

namespace tcg {

namespace {

// A whole file in memory, read in order like a dta file handle.
struct Reader {
    std::vector<uint8_t> data;
    size_t pos = 0;

    // dtaRead: as many bytes as there are, up to `size`; returns the count.
    size_t Read(void *out, size_t size)
    {
        const size_t n = pos + size <= data.size() ? size : (pos < data.size() ? data.size() - pos : 0);
        if (n)
            std::memcpy(out, data.data() + pos, n);
        pos += n;
        return n;
    }
    void Skip(size_t n) { pos += n; }
};

bool ReadAll(const std::string &path, std::vector<uint8_t> *out)
{
    FILE *f = std::fopen(path.c_str(), "rb");
    if (!f)
        return false;
    std::fseek(f, 0, SEEK_END);
    const long size = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    out->resize(size > 0 ? static_cast<size_t>(size) : 0);
    const bool ok = out->empty() || std::fread(out->data(), 1, out->size(), f) == out->size();
    std::fclose(f);
    return ok;
}

#pragma pack(push, 1)
struct S_bmpFileHeader {
    uint16_t type;
    uint32_t size;
    uint16_t reserved1, reserved2;
    uint32_t offBits;
};
struct S_bmpInfoHeader {
    uint32_t size;
    int32_t width, height;
    uint16_t planes, bitCount;
    uint32_t compression, sizeImage;
    int32_t xPelsPerMeter, yPelsPerMeter;
    uint32_t clrUsed, clrImportant;
};
#pragma pack(pop)
static_assert(sizeof(S_bmpFileHeader) == 14 && sizeof(S_bmpInfoHeader) == 40, "BMP headers");

bool IsTga(const std::string &path, bool *known)
{
    const size_t dot = path.rfind('.');
    *known = false;
    if (dot == std::string::npos || dot + 1 >= path.size())
        return false;
    const char c = path[dot + 1];
    *known = c == 't' || c == 'T' || c == 'b' || c == 'B';
    return c == 't' || c == 'T';
}

}  // namespace

uint64_t FileTimeOf(const std::string &path)
{
    WIN32_FILE_ATTRIBUTE_DATA a;
    if (!GetFileAttributesExA(path.c_str(), GetFileExInfoStandard, &a))
        return 0;
    return static_cast<uint64_t>(a.ftLastWriteTime.dwHighDateTime) << 32 | a.ftLastWriteTime.dwLowDateTime;
}

std::string ReadImageInfo(const std::string &path, ImageInfo *info)
{
    bool known;
    const bool tga = IsTga(path, &known);
    if (!known)
        return "not a .bmp or .tga file";
    Reader r;
    if (!ReadAll(path, &r.data))
        return "cannot read the file";
    if (tga) {
        uint8_t header[0x12];
        if (!r.Read(header, sizeof header))
            return "empty file";
        std::memcpy(&info->width, header + 0xc, 2);
        std::memcpy(&info->height, header + 0xe, 2);
        info->bitDepth = header[0x10];
    } else {
        S_bmpFileHeader fh;
        S_bmpInfoHeader ih;
        if (!r.Read(&fh, sizeof fh) || !r.Read(&ih, sizeof ih))
            return "empty file";
        info->width = static_cast<uint16_t>(ih.width);
        info->height = static_cast<uint16_t>(ih.height);
        info->bitDepth = static_cast<uint8_t>(ih.bitCount);
    }
    return "";
}

std::string LoadImageFile(const std::string &path, int width, int height, uint32_t flags, Image *image,
                      std::vector<std::string> *warnings)
{
    bool known;
    const bool tga = IsTga(path, &known);
    if (!known)
        return "not a .bmp or .tga file";
    Reader r;
    if (!ReadAll(path, &r.data))
        return "file not found";
    image->fileTime = FileTimeOf(path);
    auto warn = [&](const std::string &text) {
        if (warnings)
            warnings->push_back(path + ": " + text);
    };

    S_bmpInfoHeader info = {};
    bool topDown = false;
    if (tga) {
        uint8_t header[0x12];
        if (r.Read(header, sizeof header) == 0)
            return "empty file";
        if (static_cast<int8_t>(header[0]) > 0)
            r.Skip(static_cast<int8_t>(header[0]));             // the image id
        if (header[0x11] & 0x10)
            warn("right-to-left TGA: the game does not mirror it");
        if (header[2] != 1 && header[2] != 2 && header[2] != 3)
            warn("TGA image type " + std::to_string(header[2]) + " (RLE?): the game reads the data as uncompressed");
        topDown = (header[0x11] & 0x20) != 0;
        int16_t w, h;
        std::memcpy(&w, header + 0xc, 2);
        std::memcpy(&h, header + 0xe, 2);
        const int bpp = static_cast<int8_t>(header[0x10]);
        if (bpp <= 8 && (header[1] != 1 || header[7] != 24))
            warn("8-bit TGA without a 24-bit colour map: the game reads 768 bytes as the palette anyway");
        info.size = 0x28;
        info.width = w;
        info.height = h;
        info.planes = 1;
        info.bitCount = static_cast<uint16_t>(bpp);
        info.xPelsPerMeter = (bpp / 8) * w;
        info.sizeImage = info.xPelsPerMeter * h;
    } else {
        S_bmpFileHeader fh;
        if (r.Read(&fh, sizeof fh) == 0 || r.Read(&info, sizeof info) == 0)
            return "empty file";
        if (info.size != 40)
            warn("BMP info header of " + std::to_string(info.size) +
                 " bytes: the game reads the palette and pixels right after the first 40");
        if (info.height < 0)
            return "top-down BMP (negative height): the game cannot load it";
        const uint32_t clr = info.clrUsed ? info.clrUsed : (info.bitCount <= 8 ? 1u << info.bitCount : 0);
        if (info.size == 40 && fh.offBits != 14 + 40 + clr * 4)
            warn("BMP pixel offset " + std::to_string(fh.offBits) + " is not right after the palette: the game "
                 "ignores the offset");
        if (info.compression)
            return "compressed BMP (" + std::to_string(info.compression) + "): unsupported by the game";
        // BMP rows are padded to 4 bytes.
        const int rowBytes = info.bitCount * info.width / 8;
        const int padded = rowBytes / 4 * 4;
        info.xPelsPerMeter = (!(flags & kBmpUnpadded) && padded != rowBytes) ? padded + 4 : rowBytes;
    }
    const int pitch = info.xPelsPerMeter;

    image->palette.clear();
    if (!info.clrUsed && info.bitCount <= 8)
        info.clrUsed = 1u << info.bitCount;
    if (info.clrUsed) {
        if (info.clrUsed > 256)
            return "palette of " + std::to_string(info.clrUsed) + " entries (more than 256)";
        image->palette.assign(0x400, 0);
        if (!tga) {
            if (r.Read(image->palette.data(), info.clrUsed * 4) != info.clrUsed * 4)
                return "error reading the palette";
        } else {
            for (uint32_t i = 0; i < info.clrUsed; ++i) {
                if (r.Read(image->palette.data() + i * 4, 3) != 3)
                    return "error reading the palette";
            }
        }
    }

    if (pitch <= 0 || info.height <= 0)
        return "empty image";
    const size_t size = static_cast<size_t>(pitch) * info.height;
    std::vector<uint8_t> bits(size);
    if (r.Read(bits.data(), size) != size)
        return "error reading the bitmap (file too short)";
    if (topDown) {
        std::vector<uint8_t> row(pitch);
        uint8_t *a = bits.data(), *b = bits.data() + (info.height - 1) * static_cast<size_t>(pitch);
        for (int i = info.height / 2; i; --i, a += pitch, b -= pitch) {
            std::memcpy(row.data(), a, pitch);
            std::memcpy(a, b, pitch);
            std::memcpy(b, row.data(), pitch);
        }
    }
    image->width = info.width;
    image->height = info.height;
    image->bitCount = info.bitCount;
    if (!width || !height || (width == info.width && height == info.height)) {
        image->pitch = pitch;
        image->bits = std::move(bits);
        return "";
    }
    // Resized to width x height (nearest pixel), rows without padding.
    const int bpp = info.bitCount;
    std::vector<uint8_t> resized(static_cast<size_t>(bpp * width * height >> 3));
    const float stepX = static_cast<float>(info.width) / static_cast<float>(width);
    const float stepY = static_cast<float>(info.height) / static_cast<float>(height);
    uint8_t *out = resized.data();
    for (int y = 0; y < height; ++y) {
        const int row = static_cast<int>(static_cast<float>(y) * stepY) * pitch;
        for (int x = 0; x < width; ++x) {
            const int col = static_cast<int>(static_cast<float>(x) * stepX);
            std::memcpy(out, bits.data() + row + (col * bpp >> 3), bpp >> 3);
            out += bpp >> 3;
        }
    }
    image->width = width;
    image->height = height;
    image->pitch = bpp * width >> 3;
    image->bits = std::move(resized);
    return "";
}

}  // namespace tcg
