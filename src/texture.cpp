// ITexture::Create's image path and cache files (LS3DF.dll 0x10073a80 - 0x100775f0), computed on
// the CPU. Follows the LS3DF port (src/igraph/itexture.cpp, image_convert.cpp, resize_image.cpp,
// texture_compress.cpp), which reproduces the original's arithmetic, with the Direct3D surfaces
// replaced by buffers and the port's enhancements left out.
#include "texture.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "igraph/s3tc.h"
#include "image.h"

namespace tcg {

const char *FormatName(Format f)
{
    switch (f) {
    case Format::R5G6B5: return "R5G6B5";
    case Format::X1R5G5B5: return "X1R5G5B5";
    case Format::A1R5G5B5: return "A1R5G5B5";
    case Format::A4R4G4B4: return "A4R4G4B4";
    case Format::A8R8G8B8: return "A8R8G8B8";
    case Format::DXT1: return "DXT1";
    case Format::DXT3: return "DXT3";
    default: return "unknown";
    }
}

namespace {

// Versions of the compressed-file caches (0x1009dc30 / 0x1009dc32).
constexpr uint16_t kDxtCacheVersion = 9;
constexpr uint16_t kTexCacheVersion = 1;

// The part of a path after its last '\' (CopyBaseName: '/' is not a separator for the game).
std::string BaseName(const std::string &path)
{
    const size_t slash = path.rfind('\\');
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

// strncpy(buf, name, strlen(name) - 4): the name without its 3-letter extension.
std::string WithoutExtension(const std::string &name)
{
    return name.size() >= 4 ? name.substr(0, name.size() - 4) : std::string();
}

// The texture size for an image: halved with ITC_HALF, scaled down to the device maximum (both
// sides by the same factor), then rounded up to powers of two (at least 4 with DXT support).
void FitTextureSize(uint16_t width, uint16_t height, uint32_t flags, const Device &device, uint16_t *outWidth,
                    uint16_t *outHeight)
{
    uint16_t w = width, h = height;
    if (flags & ITC_HALF) {
        w >>= 1;
        h >>= 1;
    }
    float scaleX = 1.0f;
    if (static_cast<uint32_t>(w) > device.maxWidth)
        scaleX = static_cast<float>(device.maxWidth) / static_cast<float>(w);
    float scaleY = 1.0f;
    if (static_cast<uint32_t>(h) > device.maxHeight)
        scaleY = static_cast<float>(device.maxHeight) / static_cast<float>(h);
    const float scale = scaleX < scaleY ? scaleX : scaleY;
    const uint16_t fitW = static_cast<uint16_t>(static_cast<int>(static_cast<float>(w) * scale));
    const uint16_t fitH = static_cast<uint16_t>(static_cast<int>(static_cast<float>(h) * scale));
    const uint32_t minSize = device.dxt ? 4 : 1;
    uint32_t tw = minSize, th = minSize;
    while (fitW > static_cast<uint16_t>(tw) && tw < 0x8000)
        tw <<= 1;
    while (fitH > static_cast<uint16_t>(th) && th < 0x8000)
        th <<= 1;
    *outWidth = static_cast<uint16_t>(tw);
    *outHeight = static_cast<uint16_t>(th);
}

// A full mip chain's level count (CreateTexture with 0 levels).
uint16_t FullChain(uint16_t width, uint16_t height)
{
    uint32_t m = width > height ? width : height;
    uint16_t n = 1;
    while (m > 1) {
        m >>= 1;
        ++n;
    }
    return n;
}

// Palette entry / pixel in the bits of a texture format, for the colour-key compare.
uint32_t KeyColor(Format format, uint8_t r, uint8_t g, uint8_t b, bool *known)
{
    *known = true;
    switch (format) {
    case Format::A8R8G8B8: return static_cast<uint32_t>(r) << 16 | static_cast<uint32_t>(g) << 8 | b;
    case Format::R5G6B5: return ((static_cast<uint32_t>(r & 0xf8) << 5 | (g & 0xfc)) << 3) | (b >> 3);
    case Format::X1R5G5B5:
    case Format::A1R5G5B5:
    case Format::DXT1: return ((static_cast<uint32_t>(r & 0xf8) << 5 | (g & 0xf8)) << 2) | (b >> 3);
    case Format::A4R4G4B4:
    case Format::DXT3: return (static_cast<uint32_t>(r & 0xf0) << 4 | (g & 0xf0)) | (b >> 4);
    default: *known = false; return 0;
    }
}

// The mip filter of Create: a 2x2 block to one pixel with weights 3/8 for the pixels right of and
// below the block's first one, 1/8 for the other two; every term truncated, summed in 8 bits.
// Rows of the destination are half the source row bytes apart.
void Downsample(const uint8_t *src, int width, int height, int rowBytes, uint8_t *dst)
{
    if (height - 1 <= 0)
        return;
    const int rows = ((height - 2) >> 1) + 1;
    const int cols = width - 1 > 0 ? ((width - 2) >> 1) + 1 : 0;
    const int dstRowBytes = rowBytes >> 1;
    for (int y = 0; y < rows; ++y) {
        const uint8_t *r0 = src + y * 2 * rowBytes;
        const uint8_t *r1 = r0 + rowBytes;
        uint8_t *d = dst + y * dstRowBytes;
        for (int x = 0; x < cols; ++x) {
            const uint8_t *p00 = r0 + x * 8, *p10 = p00 + 4;
            const uint8_t *p01 = r1 + x * 8, *p11 = p01 + 4;
            for (int c = 0; c < 4; ++c)
                d[x * 4 + c] = static_cast<uint8_t>((3 * p10[c] >> 3) + (3 * p01[c] >> 3) + (p11[c] >> 3) +
                                                    (p00[c] >> 3));
        }
    }
}

// IGraph::ResizeImage (0x10070160), 4 bytes per pixel, filtered.
void ResizeImage(int srcWidth, int srcHeight, int srcPitch, const uint8_t *src, int dstWidth, int dstHeight,
                 int dstPitch, uint8_t *dst)
{
    const int bpp = 4;
    const int rowSkip = dstPitch - dstWidth * bpp;
    if (srcWidth < dstWidth || srcHeight < dstHeight) {
        // Nearest source pixel.
        const float stepX = static_cast<float>(srcWidth) / static_cast<float>(dstWidth);
        const float stepY = static_cast<float>(srcHeight) / static_cast<float>(dstHeight);
        for (int y = 0; y < dstHeight; ++y) {
            if (dstWidth > 0) {
                const int row = static_cast<int>(static_cast<float>(y) * stepY) * srcPitch;
                for (int x = 0; x < dstWidth; ++x) {
                    const int col = static_cast<int>(static_cast<float>(x) * stepX);
                    std::memcpy(dst, src + col * bpp + row, bpp);
                    dst += bpp;
                }
            }
            dst += rowSkip;
        }
        return;
    }

    // Box filter over the source area of each destination pixel, partial pixels at the edges
    // weighted by their covered fraction. The scale maps onto srcWidth - 1 (srcHeight - 1) pixels.
    // Float operations in the original's order.
    const size_t srcSize = static_cast<size_t>(srcPitch) * srcHeight;
    const float scaleX = static_cast<float>(srcWidth - 1) / static_cast<float>(dstWidth);
    const float scaleY = static_cast<float>(srcHeight - 1) / static_cast<float>(dstHeight);
    constexpr float kEps = 1e-08f;
    for (int y = 0; y < dstHeight; ++y) {
        for (int x = 0; x < dstWidth; ++x) {
            const float top = static_cast<float>(y) * scaleY;
            const float bottom = static_cast<float>(y + 1) * scaleY;
            const int y0 = static_cast<int>(top) + 1;
            const int y1 = static_cast<int>(bottom);
            float wTop, wBottom;
            if (y0 > y1) {
                wTop = bottom - top;
                wBottom = 0.0f;
            } else {
                wTop = static_cast<float>(y0) - top;
                wBottom = bottom - static_cast<float>(y1);
            }
            const int rows = y1 - y0 < 0 ? 0 : y1 - y0;
            const float left = static_cast<float>(x) * scaleX;
            const float right = static_cast<float>(x + 1) * scaleX;
            const int x0 = static_cast<int>(left) + 1;
            const int x1 = static_cast<int>(right);
            float wLeft, wRight;
            if (x0 > x1) {
                wLeft = right - left;
                wRight = 0.0f;
            } else {
                wLeft = static_cast<float>(x0) - left;
                wRight = right - static_cast<float>(x1);
            }
            const int cols = x1 - x0 < 0 ? 0 : x1 - x0;

            float b = 0.0f, g = 0.0f, r = 0.0f, a = 0.0f, weight = 0.0f;
            auto add = [&](int px, int py, float w) {
                const uint8_t *p = src + py * srcPitch + px * bpp;
                r = r + static_cast<float>(p[2]) * w;
                g = g + static_cast<float>(p[1]) * w;
                b = b + static_cast<float>(p[0]) * w;
                a = a + static_cast<float>(p[3]) * w;
            };
            auto addCorner = [&](int px, int py, float wx, float wy) {
                const uint8_t *p = src + py * srcPitch + px * bpp;
                r = r + static_cast<float>(p[2]) * wx * wy;
                g = g + static_cast<float>(p[1]) * wx * wy;
                b = b + static_cast<float>(p[0]) * wx * wy;
                a = a + static_cast<float>(p[3]) * wx * wy;
                weight = wx * wy + weight;
            };
            if (wTop > kEps) {
                if (wLeft > kEps)
                    addCorner(x0 - 1, y0 - 1, wLeft, wTop);
                if (cols > 0) {
                    for (int c = 0; c < cols; ++c)
                        add(x0 + c, y0 - 1, wTop);
                    weight = static_cast<float>(cols) * wTop + weight;
                }
                if (wRight > kEps)
                    addCorner(x0 + cols, y0 - 1, wRight, wTop);
            }
            if (rows > 0) {
                if (wLeft > kEps) {
                    for (int k = 0; k < rows; ++k)
                        add(x0 - 1, y0 + k, wLeft);
                    weight = static_cast<float>(rows) * wLeft + weight;
                }
                if (cols > 0) {
                    // Original bug kept: the inner block advances its pixel pointer twice per
                    // column (pixels x0, x0 + 2 ...). Beyond the source buffer the original read
                    // whatever memory followed; this reads 0, as the port does.
                    for (int k = 0; k < rows; ++k) {
                        for (int c = 0; c < cols; ++c) {
                            const size_t at = static_cast<size_t>((y0 + k) * srcPitch + (x0 + 2 * c) * bpp);
                            auto byte = [&](int i) { return at + i < srcSize ? src[at + i] : 0; };
                            r = r + static_cast<float>(byte(2));
                            g = g + static_cast<float>(byte(1));
                            b = b + static_cast<float>(byte(0));
                            a = a + static_cast<float>(byte(3));
                        }
                    }
                    weight = static_cast<float>(cols * rows) + weight;
                }
                if (wRight > kEps) {
                    for (int k = 0; k < rows; ++k)
                        add(x0 + cols, y0 + k, wRight);
                    weight = static_cast<float>(rows) * wRight + weight;
                }
            }
            if (wBottom > kEps) {
                const int py = y0 + rows;
                if (wLeft > kEps)
                    addCorner(x0 - 1, py, wLeft, wBottom);
                if (cols > 0) {
                    for (int c = 0; c < cols; ++c)
                        add(x0 + c, py, wBottom);
                    weight = static_cast<float>(cols) * wBottom + weight;
                }
                if (wRight > kEps)
                    addCorner(x0 + cols, py, wRight, wBottom);
            }
            if (weight > kEps || std::isnan(weight)) {
                const float inv = 1.0f / weight;
                r = inv * r;
                g = inv * g;
                b = inv * b;
                a = inv * a;
            } else {
                r = 255.0f;
                g = 0.0f;
                b = 255.0f;
                a = 255.0f;
            }
            dst[2] = static_cast<uint8_t>(std::lrintf(r));
            dst[1] = static_cast<uint8_t>(std::lrintf(g));
            dst[0] = static_cast<uint8_t>(std::lrintf(b));
            dst[3] = static_cast<uint8_t>(std::lrintf(a));
            dst += bpp;
        }
        dst += rowSkip;
    }
}

// Error-diffusion buffer of the dithered conversions: (height + 1) rows of (width + 2) entries of
// red, green, blue, alpha. Pixel (x, y) reads entry (x + 1, y) and passes its error to (x + 2, y),
// (x, y + 1), (x + 1, y + 1), (x + 2, y + 1) with weights 7, 3, 5, 1 (/16).
struct Errors {
    Errors(uint32_t width, uint32_t height) : stride(width + 2), data((height + 1) * (width + 2) * 4, 0.0f) {}
    float *At(uint32_t x, uint32_t y) { return &data[(y * stride + x) * 4]; }
    uint32_t stride;
    std::vector<float> data;
};

enum { kR = 0, kG = 1, kB = 2, kA = 3 };

int DitherChannel(Errors &err, uint32_t x, uint32_t y, int channel, uint8_t value, float step)
{
    const float v = static_cast<float>(value) + err.At(x + 1, y)[channel];
    const float e = std::fmod(v, step) * 0.0625f;
    float *right = err.At(x + 2, y), *below = err.At(x, y + 1);
    right[channel] = e * 7.0f + right[channel];
    below[channel] = e * 3.0f + below[channel];
    below[4 + channel] = e * 5.0f + below[4 + channel];
    below[8 + channel] = e + below[8 + channel];
    const float clamped = v > 255.0f ? 255.0f : v;
    return static_cast<int>(std::lrintf(clamped));
}

// WriteSurface (0x10077d00): a 32-bit BGRA image into a 16-bit surface of `format` (rows
// width * 2 bytes apart), Floyd-Steinberg dithered with `dither` and kind 0.
std::vector<uint16_t> WriteSurface16(const uint8_t *bgra, Format format, uint32_t width, uint32_t height, bool dither,
                                     int kind)
{
    std::vector<uint16_t> out(static_cast<size_t>(width) * height);
    const bool diffuse = dither && kind == 0;
    Errors errors(diffuse ? width : 0, diffuse ? height : 0);
    const uint8_t *p = bgra;
    for (uint32_t y = 0; y < height; ++y) {
        uint16_t *o = out.data() + static_cast<size_t>(y) * width;
        for (uint32_t x = 0; x < width; ++x, p += 4) {
            switch (format) {
            case Format::R5G6B5:
                if (diffuse) {
                    const int r = DitherChannel(errors, x, y, kR, p[2], 8.0f);
                    const int g = DitherChannel(errors, x, y, kG, p[1], 4.0f);
                    const int b = DitherChannel(errors, x, y, kB, p[0], 8.0f);
                    o[x] = static_cast<uint16_t>(((r & 0xfff8) << 5 | (g & 0xfc)) << 3 | ((b >> 3) & 0x1f));
                } else {
                    o[x] = static_cast<uint16_t>(((p[2] & 0xf8) << 5 | (p[1] & 0xfc)) << 3 | (p[0] >> 3));
                }
                break;
            case Format::X1R5G5B5:
            case Format::A1R5G5B5:
                if (diffuse) {
                    const int r = DitherChannel(errors, x, y, kR, p[2], 8.0f);
                    const int g = DitherChannel(errors, x, y, kG, p[1], 8.0f);
                    const int b = DitherChannel(errors, x, y, kB, p[0], 8.0f);
                    const int a = DitherChannel(errors, x, y, kA, p[3], 128.0f);
                    o[x] = static_cast<uint16_t>(((((a & 0xff80) << 1 | (r & 0xf8)) << 5 | (g & 0xf8)) << 2) |
                                                 ((b >> 3) & 0x1f));
                } else {
                    // (Blue in the red bits and red in the blue bits, as in the original.)
                    o[x] = static_cast<uint16_t>(((((p[3] & 0x80) << 1 | (p[0] & 0xf8)) << 5 | (p[1] & 0xf8)) << 2) |
                                                 (p[2] >> 3));
                }
                break;
            case Format::A4R4G4B4:
                if (diffuse) {
                    // (The original takes a step of 8 for every 4-bit channel, not 16.)
                    const int r = DitherChannel(errors, x, y, kR, p[2], 8.0f);
                    const int g = DitherChannel(errors, x, y, kG, p[1], 8.0f);
                    const int b = DitherChannel(errors, x, y, kB, p[0], 8.0f);
                    const int a = DitherChannel(errors, x, y, kA, p[3], 8.0f);
                    o[x] = static_cast<uint16_t>(((a & 0xfff0) << 8 | (r & 0xf0) << 4) | (g & 0xf0) | ((b >> 4) & 0xf));
                } else {
                    o[x] = static_cast<uint16_t>(((p[3] & 0xf0) << 4 | (p[2] & 0xf0)) << 4 | (p[1] & 0xf0) | (p[0] >> 4));
                }
                break;
            default:
                break;
            }
        }
    }
    return out;
}

// CompressSurface (0x100788b0): a 16-bit staging level through the S3TC encoder. `kind` 1:
// X1R5G5B5 to DXT1, 2: A1R5G5B5 to DXT1 with transparency, 3: A4R4G4B4 to DXT3.
void Compress(int kind, const std::vector<uint16_t> &source, uint32_t width, uint32_t height, uint8_t *out)
{
    static const float kWeights[3] = {0.3086f, 0.6094f, 0.082f};
    using namespace ls3d::s3tc;
    S_surfaceDesc src = {};
    src.size = sizeof src;
    src.flags = 0x180e;
    src.height = height;
    src.width = width;
    src.pitch = static_cast<int32_t>(width * 2);
    src.surface = const_cast<uint16_t *>(source.data());
    src.pfSize = 0x20;
    uint32_t type = S3TC_OPAQUE;
    switch (kind) {
    case 2:
        type = S3TC_ALPHA_KEY;
        [[fallthrough]];
    case 1:
        src.mask[0] = 0x7c00;
        src.mask[1] = 0x3e0;
        src.mask[2] = 0x1f;
        src.mask[3] = 0x8000;
        src.pfFlags = 0x41;
        src.bitCount = 16;
        break;
    case 3:
        type = S3TC_ALPHA_EXPLICIT;
        src.mask[0] = 0xf00;
        src.mask[1] = 0xf0;
        src.mask[2] = 0xf;
        src.mask[3] = 0xf000;
        src.pfFlags = 0x41;
        src.bitCount = 16;
        break;
    default:
        break;
    }
    S_surfaceDesc dst = {};
    dst.size = sizeof dst;
    Encode(&src, nullptr, &dst, out, type, kWeights);
}

// The row pitch of a 16-bit level: whole DWORDs, so a 1-pixel-wide level has 2 bytes of padding
// per row (seen in the shipped G_AF5_143_8.565, whose 1x2 and 1x1 levels are 8 and 4 bytes).
uint32_t Pitch16(uint32_t width)
{
    return (width * 2 + 3) & ~3u;
}

// The bytes of one level of a texture (D3DSURFACE_DESC::Size).
size_t LevelSize(Format format, uint32_t width, uint32_t height)
{
    const size_t bw = (width + 3) / 4 ? (width + 3) / 4 : 1, bh = (height + 3) / 4 ? (height + 3) / 4 : 1;
    switch (format) {
    case Format::DXT1: return bw * bh * 8;
    case Format::DXT3: return bw * bh * 16;
    case Format::A8R8G8B8: return static_cast<size_t>(width) * height * 4;
    default: return static_cast<size_t>(Pitch16(width)) * height;
    }
}

void Put16(std::vector<uint8_t> &out, uint16_t v)
{
    out.push_back(static_cast<uint8_t>(v));
    out.push_back(static_cast<uint8_t>(v >> 8));
}
void Put32(std::vector<uint8_t> &out, uint32_t v)
{
    Put16(out, static_cast<uint16_t>(v));
    Put16(out, static_cast<uint16_t>(v >> 16));
}
void Put64(std::vector<uint8_t> &out, uint64_t v)
{
    Put32(out, static_cast<uint32_t>(v));
    Put32(out, static_cast<uint32_t>(v >> 32));
}

}  // namespace

std::string CacheFileName(const std::string &imagePath, const std::string &alphaFileName, int kind, uint16_t levels,
                          bool half, Format format)
{
    std::string s = WithoutExtension(imagePath);
    const bool dxt = kind >= 1;
    if (dxt && kind == 3 && !alphaFileName.empty()) {
        s += "_";
        s += WithoutExtension(alphaFileName);
    }
    s += "_" + std::to_string(levels);
    if (half)
        s += "L";
    s += ".";
    if (dxt) {
        s += "DX";
        s += static_cast<char>('0' + kind);
    } else {
        s += format == Format::R5G6B5 ? "565" : "555";
    }
    return s;
}

bool ParseCacheHeader(const std::vector<uint8_t> &file, bool dxt3, CacheHeader *h)
{
    const size_t need = dxt3 ? 30 : 22;
    if (file.size() < need)
        return false;
    std::memcpy(h->magic, file.data(), 4);
    std::memcpy(&h->version, file.data() + 4, 2);
    std::memcpy(&h->width, file.data() + 6, 4);
    std::memcpy(&h->height, file.data() + 10, 4);
    std::memcpy(&h->time, file.data() + 14, 8);
    h->hasAlphaTime = dxt3;
    if (dxt3)
        std::memcpy(&h->alphaTime, file.data() + 22, 8);
    h->dataOffset = need;
    return true;
}

CacheResult BuildCache(const TextureLoad &load, const Device &device, const TextureFinder &find,
                       const FileLocator &locate, const BuildOptions &options)
{
    CacheResult res;
    auto fail = [&](Outcome outcome, const std::string &message) {
        res.outcome = outcome;
        res.message = message;
        return res;
    };

    // ---- IGraph::LoadTexture (0x10069f70)
    uint32_t flags = load.flags;
    const std::string &name = load.name;
    if (!name.empty() && (name.back() == 's' || name.back() == 'S'))
        flags &= ~ITC_DDS_IGNORED;
    if (flags & ITC_DEFAULT)
        return fail(Outcome::NoCache, "IGraph's default texture");
    if (flags & ITC_FROM_MEMORY)
        return fail(Outcome::NoCache, "a texture from memory");
    if (!load.alphaName.empty())
        flags |= ITC_USE_ALPHA;
    if (name.empty())
        return fail(Outcome::Error, "no file name");
    std::string path = find(name);
    if (path.empty())
        path = "Maps\\" + name;
    std::string alphaPath;
    if (!load.alphaName.empty()) {
        alphaPath = find(load.alphaName);
        if (alphaPath.empty())
            alphaPath = "Maps\\" + load.alphaName;
    }
    // Create gets the opacity map only with ITC_USE_ALPHA.
    const std::string createAlpha = (flags & ITC_USE_ALPHA) ? alphaPath : std::string();
    res.imagePath = path;
    res.alphaPath = createAlpha;

    // ---- ITexture::Create
    if (flags & ITC_STREAMED)
        return fail(Outcome::NoCache, "streamed texture (loaded by the texture thread)");
    {
        const size_t dot = path.rfind('.');
        if (dot != std::string::npos && dot + 1 < path.size() && (path[dot + 1] == 'd' || path[dot + 1] == 'D'))
            return fail(Outcome::NoCache, ".dds file (loaded in its own format)");
    }
    res.imageFile = locate(path);
    if (res.imageFile.empty())
        return fail(Outcome::Error, "file not found: " + path);
    ImageInfo info;
    std::string err = ReadImageInfo(res.imageFile, &info);
    if (!err.empty())
        return fail(Outcome::Error, path + ": " + err);
    if (flags & ITC_SOURCE_ALPHA) {
        if (info.bitDepth == 8 || info.bitDepth == 24)
            flags &= ~ITC_SOURCE_ALPHA;
        if (info.bitDepth == 16)
            flags |= ITC_ALPHA_1BIT;
    }
    uint16_t width = info.width, height = info.height;
    if (!(flags & ITC_NORESIZE))
        FitTextureSize(width, height, flags, device, &width, &height);

    Image image;
    err = LoadImageFile(res.imageFile, 0, 0, flags, &image, &res.warnings);
    if (!err.empty())
        return fail(Outcome::Error, path + ": " + err);
    switch (image.bitCount) {
    case 8:
    case 16:
    case 24:
    case 32: break;
    default: return fail(Outcome::Error, path + ": " + std::to_string(image.bitCount) + "-bit image (only 8, 16, 24, 32)");
    }

    // ---- the opacity map, as 8-bit gray (luminance 77 R + 154 G + 25 B, / 256)
    std::vector<uint8_t> gray;
    int grayWidth = 0, grayHeight = 0;
    std::string alphaFileName;
    uint64_t alphaTime = 0;
    if (flags & ITC_USE_ALPHA) {
        if (!createAlpha.empty()) {
            alphaFileName = BaseName(createAlpha);
            res.alphaFile = locate(createAlpha);
            if (res.alphaFile.empty())
                return fail(Outcome::Error, "opacity map not found: " + createAlpha);
            Image alpha;
            err = LoadImageFile(res.alphaFile, image.width, image.height, flags, &alpha, &res.warnings);
            if (!err.empty())
                return fail(Outcome::Error, createAlpha + ": " + err);
            alphaTime = alpha.fileTime;
            auto luma = [](uint8_t r, uint8_t g, uint8_t b) {
                return static_cast<uint8_t>(((r + g * 2) * 0x4d + b * 0x19) >> 8);
            };
            grayWidth = alpha.width;
            grayHeight = alpha.height;
            const size_t count = static_cast<size_t>(alpha.width) * alpha.height;
            gray.assign(count, 0);
            // (Read without the rows' padding, as in the original.)
            switch (alpha.bitCount >> 3) {
            case 1:
                for (size_t i = 0; i < count; ++i) {
                    const uint8_t *e = alpha.palette.data() + alpha.bits[i] * 4;
                    gray[i] = luma(e[2], e[1], e[0]);
                }
                break;
            case 3:
                for (size_t i = 0; i < count; ++i)
                    gray[i] = luma(alpha.bits[i * 3 + 2], alpha.bits[i * 3 + 1], alpha.bits[i * 3]);
                break;
            default:
                return fail(Outcome::Error, createAlpha + ": " + std::to_string(alpha.bitCount) +
                                                "-bit opacity map (only 8 and 24 bits)");
            }
        } else {
            flags &= ~ITC_USE_ALPHA;
        }
    }

    // ---- the texture format; kind 0 uncompressed, 1 DXT1, 2 DXT1 with 1-bit alpha, 3 DXT3
    const Format fmtDxt1 = device.dxt ? Format::DXT1 : Format::Unknown;
    const Format fmtDxt3 = device.dxt ? Format::DXT3 : Format::Unknown;
    const Format fmt1BitAlpha = Format::A1R5G5B5, fmtAlpha = Format::A4R4G4B4;
    Format staging = Format::Unknown;
    int kind = 0;
    Format fmt;
    auto alphaFormat = [&]() {
        if ((flags & ITC_COMPRESS) && fmtDxt3 != Format::Unknown) {
            staging = fmtAlpha;
            kind = 3;
            return fmtDxt3;
        }
        return fmtAlpha;
    };
    if (flags & ITC_TRUECOLOR) {
        fmt = Format::A8R8G8B8;
    } else {
        const bool oneBit = (flags & ITC_ALPHA_1BIT) != 0;
        if ((flags & (ITC_SOURCE_ALPHA | ITC_USE_ALPHA)) && !oneBit) {
            fmt = alphaFormat();
        } else if (oneBit) {
            if (flags & ITC_MIPMAP) {
                fmt = alphaFormat();
            } else if ((flags & ITC_COMPRESS) && fmtDxt1 != Format::Unknown) {
                staging = fmt1BitAlpha;
                kind = 2;
                fmt = fmtDxt1;
            } else {
                fmt = fmt1BitAlpha;
            }
        } else if ((flags & ITC_COMPRESS) && fmtDxt1 != Format::Unknown) {
            staging = fmt1BitAlpha;
            kind = 1;
            fmt = fmtDxt1;
        } else {
            fmt = device.fmt16;
        }
    }
    const uint16_t levels = (flags & ITC_MIPMAP) ? FullChain(width, height) : 1;
    res.flags = flags;
    res.format = fmt;
    res.kind = kind;
    res.width = width;
    res.height = height;
    res.levels = levels;
    const bool sixteen = fmt == Format::R5G6B5 || fmt == Format::X1R5G5B5 || fmt == Format::A1R5G5B5;
    if (kind == 0 && !sixteen)
        return fail(Outcome::NoCache, std::string(FormatName(fmt)) + " texture (only DXT and 16-bit RGB textures are cached)");
    if (kind >= 1 && ((width & 3) || (height & 3)))
        return fail(Outcome::Error, "DXT texture of " + std::to_string(width) + "x" + std::to_string(height) +
                                        " (ITC_NORESIZE): Direct3D refuses it");
    res.cachePath = CacheFileName(path, alphaFileName, kind, levels, (flags & ITC_HALF) != 0, fmt);
    res.outcome = Outcome::Cache;
    if (!options.contents)
        return res;

    // ---- the image as 32-bit BGRA, top row first (the sources are bottom-up)
    const int w = image.width, h = image.height;
    std::vector<uint8_t> pixels(static_cast<size_t>(w) * h * 4);
    {
        uint32_t colorKey = 0;
        const int pitch = image.pitch;
        const int rowBack = (image.bitCount >> 3) * w + pitch;
        const int grayBack = grayWidth * 2;
        uint8_t *out = pixels.data();
        const uint8_t *alpha = gray.empty() ? nullptr : gray.data() + static_cast<size_t>(grayHeight - 1) * grayWidth;
        const uint8_t *bits = image.bits.data();
        switch (image.bitCount) {
        case 8: {
            const uint8_t *pal = image.palette.data();
            bool known;
            const uint32_t key = KeyColor(fmt, pal[2], pal[1], pal[0], &known);
            if (known)
                colorKey = key;
            const uint8_t *src = bits + static_cast<size_t>(h - 1) * pitch;
            for (int y = h; y; --y) {
                for (int x = 0; x < w; ++x, ++src, out += 4) {
                    const uint8_t *e = pal + *src * 4;
                    out[0] = e[0];
                    out[1] = e[1];
                    out[2] = e[2];
                    if (alpha) {
                        out[3] = *alpha++;
                    } else if (flags & ITC_ALPHA_1BIT) {
                        const uint32_t c = KeyColor(fmt, e[2], e[1], e[0], &known);
                        out[3] = c == colorKey ? 0 : 0xff;
                    } else {
                        out[3] = 0xff;
                    }
                }
                src -= rowBack;
                if (alpha)
                    alpha -= grayBack;
            }
            break;
        }
        case 16: {
            const uint8_t *src = bits + static_cast<size_t>(h - 1) * pitch;
            for (int y = h; y; --y) {
                for (int x = 0; x < w; ++x, out += 4) {
                    uint16_t c;
                    std::memcpy(&c, src + x * 2, 2);
                    out[0] = static_cast<uint8_t>(c << 3);
                    out[1] = static_cast<uint8_t>(c >> 2) & 0xf8;
                    out[2] = static_cast<uint8_t>(c >> 7) & 0xf8;
                    if (flags & ITC_SOURCE_ALPHA)
                        out[3] = (c & 0x8000) ? 0xff : 0;
                    else if (alpha)
                        out[3] = *alpha++;
                    else if (flags & ITC_ALPHA_1BIT)
                        out[3] = c == colorKey ? 0 : 0xff;
                    else
                        out[3] = 0xff;
                }
                src += w * 2 - rowBack;
                if (alpha)
                    alpha -= grayBack;
            }
            break;
        }
        case 24: {
            const uint8_t *src = bits + static_cast<size_t>(h - 1) * pitch;
            for (int y = h; y; --y) {
                for (int x = 0; x < w; ++x, src += 3, out += 4) {
                    out[0] = src[0];
                    out[1] = src[1];
                    out[2] = src[2];
                    if (alpha)
                        out[3] = *alpha++;
                    else if (flags & ITC_ALPHA_1BIT)
                        out[3] = src[0] == colorKey ? 0 : 0xff;   // (the blue byte against the key)
                    else
                        out[3] = 0xff;
                }
                src -= rowBack;
                if (alpha)
                    alpha -= grayBack;
            }
            break;
        }
        case 32: {
            const uint8_t *src = bits + static_cast<size_t>(h - 1) * pitch;
            for (int y = h; y; --y) {
                for (int x = 0; x < w; ++x, src += 4, out += 4) {
                    out[0] = src[0];
                    out[1] = src[1];
                    out[2] = src[2];
                    if (flags & ITC_SOURCE_ALPHA)
                        out[3] = src[3];
                    else if (alpha)
                        out[3] = *alpha++;
                    else if (flags & ITC_ALPHA_1BIT)
                        out[3] = src[0] == colorKey ? 0 : 0xff;
                    else
                        out[3] = 0xff;
                }
                src -= rowBack;
                if (alpha)
                    alpha -= grayBack;
            }
            break;
        }
        }
    }
    // Scaled to the texture size (box filter).
    if (width != w || height != h) {
        std::vector<uint8_t> resized(static_cast<size_t>(width) * height * 4);
        ResizeImage(w, h, w * 4, pixels.data(), width, height, width * 4, resized.data());
        pixels.swap(resized);
    }

    // ---- the levels: converted (and compressed through a staging texture), mip maps filtered
    if (options.resetEncoder)
        ls3d::s3tc::ResetState();
    std::vector<uint8_t> data;
    int rowBytes = width * 4;
    int levelW = width, levelH = height;
    std::vector<uint8_t> cur, next;
    if (levels > 1) {
        cur = pixels;
        // (Uninitialised in the original until written; the levels it does not compute.)
        next.assign(static_cast<size_t>(width) * height * 2, 0);
    }
    const bool dither = (flags & ITC_DITHER) != 0;
    for (int level = 0; level < levels; ++level) {
        const uint8_t *img = pixels.data();
        if (level) {
            Downsample(cur.data(), levelW, levelH, rowBytes, next.data());
            levelW = levelW >> 1 ? levelW >> 1 : 1;
            levelH = levelH >> 1 ? levelH >> 1 : 1;
            rowBytes >>= 1;
            img = next.data();
        }
        const size_t size = LevelSize(fmt, levelW, levelH);
        const size_t at = data.size();
        data.resize(at + size);
        if (staging != Format::Unknown) {
            const std::vector<uint16_t> stage = WriteSurface16(img, staging, levelW, levelH, dither, 0);
            Compress(kind, stage, levelW, levelH, data.data() + at);
        } else {
            const std::vector<uint16_t> surface = WriteSurface16(img, fmt, levelW, levelH, dither, kind);
            // Rows at the surface's pitch, the padding left 0.
            const uint32_t pitch = Pitch16(levelW);
            for (int y = 0; y < levelH; ++y)
                std::memcpy(data.data() + at + static_cast<size_t>(y) * pitch, surface.data() + static_cast<size_t>(y) * levelW,
                            static_cast<size_t>(levelW) * 2);
        }
        if (level)
            cur.swap(next);
    }

    // ---- the cache file (the end of Create)
    std::vector<uint8_t> &file = res.bytes;
    file.clear();
    const char *magic = kind >= 1 ? "DXT" : "TEX";
    file.insert(file.end(), magic, magic + 4);                  // with the terminating 0
    Put16(file, kind >= 1 ? kDxtCacheVersion : kTexCacheVersion);
    Put32(file, width);
    Put32(file, height);
    Put64(file, image.fileTime);
    if (fmt == Format::DXT3)
        Put64(file, alphaTime);
    file.insert(file.end(), data.begin(), data.end());
    return res;
}

}  // namespace tcg
