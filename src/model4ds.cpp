#include "model4ds.h"

#include <cstdio>
#include <cstring>

namespace tcg {

namespace {

// Material flags (I3D_material::matFlags, as stored in the file).
enum : uint32_t {
    MATF_ENV_TEXTURE = 0x00080000,
    MATF_ANIMATED = 0x04000000,
    MATF_ALPHA_FRAMES = 0x02000000,     // animated: the opacity map has frames too
    MATF_COLORKEY = 0x20000000,
    MATF_ALPHA = 0x40000000,
    MATF_NO_ALPHA_NAME = 0xa1000000,    // additive, colour key or own alpha: no opacity map name
};

struct Reader {
    std::vector<uint8_t> data;
    size_t pos = 0;
    bool ok = true;

    void Read(void *out, size_t n)
    {
        if (pos + n > data.size()) {
            ok = false;
            std::memset(out, 0, n);
            pos = data.size();
            return;
        }
        std::memcpy(out, data.data() + pos, n);
        pos += n;
    }
    void Skip(size_t n) { pos + n > data.size() ? (ok = false, pos = data.size()) : pos += n; }
    // ReadName: a length byte and that many characters.
    std::string Name()
    {
        uint8_t n = 0;
        Read(&n, 1);
        std::string s(n, '\0');
        if (n)
            Read(&s[0], n);
        return s;
    }
};

// "name00.ext" ... for an animated material's frames (names shorter than 6: one empty frame).
std::string FrameName(const std::string &name, int frame)
{
    const size_t at = name.size() - 6;
    std::string s = name;
    char digits[16];
    std::snprintf(digits, sizeof digits, "%02i", frame);
    s[at] = digits[0];
    s[at + 1] = digits[1];
    s[at + 2] = '.';
    if (std::strlen(digits) > 2)
        s.resize(at + 3);               // sprintf's terminator lands on the extension (frames >= 100)
    return s;
}

}  // namespace

uint32_t MaterialTextureFlags(uint32_t flags, const DriverStates &states)
{
    uint32_t tf = flags >> 5 & ITC_STREAMED;
    if (states.dither)
        tf |= ITC_DITHER;
    if (states.trueColor || (flags & 0x100000))
        tf |= ITC_TRUECOLOR;
    else if (states.compress && !(flags & 0x200000))
        tf |= ITC_COMPRESS;
    if ((flags & 0x800000) && states.mipmap)
        tf |= ITC_MIPMAP;
    if (states.half && !(flags & 0x400000))
        tf |= ITC_HALF;
    if (flags & 0x8000)
        tf |= ITC_KEEP_PIXELS;
    if (flags & MATF_COLORKEY)
        tf |= (flags & 0x1000000) ? (ITC_ALPHA_1BIT | ITC_SOURCE_ALPHA) : ITC_ALPHA_1BIT;
    else if (flags & MATF_ALPHA)
        tf |= (flags & 0x1000000) ? ITC_SOURCE_ALPHA : ITC_USE_ALPHA;
    return tf;
}

uint32_t EnvironmentTextureFlags(uint32_t flags, const DriverStates &states)
{
    return (states.mipmap ? ITC_MIPMAP : 0) | (MaterialTextureFlags(flags, states) & 0xffffef31) | 0x80000;
}

std::string ReadModelTextures(const std::string &file, const DriverStates &states, std::vector<MaterialLoad> *out)
{
    Reader r;
    {
        FILE *f = std::fopen(file.c_str(), "rb");
        if (!f)
            return "cannot open";
        std::fseek(f, 0, SEEK_END);
        const long size = std::ftell(f);
        std::fseek(f, 0, SEEK_SET);
        r.data.resize(size > 0 ? static_cast<size_t>(size) : 0);
        const size_t got = r.data.empty() ? 0 : std::fread(r.data.data(), 1, r.data.size(), f);
        std::fclose(f);
        if (got != r.data.size())
            return "cannot read";
    }
    char magic[4];
    uint16_t version = 0;
    r.Read(magic, 4);
    r.Read(&version, 2);
    if (!r.ok || std::memcmp(magic, "4DS", 4) != 0)
        return "not a 4DS file";
    if (version != 0x29)
        return "4DS version " + std::to_string(version) + " (the game loads 0x29)";
    r.Skip(8);
    uint16_t count = 0;
    r.Read(&count, 2);
    if (count > 6000)
        return "more than 6000 materials";
    for (int i = 0; i < count && r.ok; ++i) {
        uint32_t flags = 0;
        r.Read(&flags, 4);
        const uint32_t tf = MaterialTextureFlags(flags, states);
        auto add = [&](const std::string &name, const std::string &alpha, uint32_t texFlags, bool env) {
            MaterialLoad m;
            m.material = i;
            m.materialFlags = flags;
            m.environment = env;
            m.load.name = name;
            m.load.alphaName = alpha;
            m.load.flags = texFlags;
            out->push_back(m);
        };
        const bool hasEnv = (flags & MATF_ENV_TEXTURE) != 0;
        const bool readsAlpha = (flags & MATF_ALPHA) && !(flags & MATF_NO_ALPHA_NAME);
        if (!(flags & MATF_ANIMATED)) {
            // I3D_material::Load
            r.Skip(hasEnv ? 0x4c : 0x48);
            if (hasEnv) {
                const std::string env = r.Name();
                if (!env.empty())
                    add(env, "", EnvironmentTextureFlags(flags, states), true);
            }
            const std::string name = r.Name();
            if (name.empty())
                continue;
            std::string alpha;
            uint32_t texFlags = tf;
            if (readsAlpha) {
                alpha = r.Name();
                if (alpha.empty())
                    texFlags &= ~ITC_USE_ALPHA;
            }
            add(name, alpha, texFlags, false);
        } else {
            // I3D_anim_material::Load
            r.Skip(hasEnv ? 0x4c : 0x48);
            std::string env;
            if (hasEnv)
                env = r.Name();
            const std::string name = r.Name();
            std::string alpha;
            if (!name.empty() && readsAlpha)
                alpha = r.Name();
            uint8_t frames = 0, envFrames = 0;
            r.Read(&frames, 1);
            r.Read(&envFrames, 1);
            r.Skip(0x10);
            if (hasEnv) {
                const uint32_t envFlags = EnvironmentTextureFlags(flags, states);
                if (envFrames < 2) {
                    if (!env.empty())
                        add(env, "", envFlags, true);
                } else if (env.size() >= 6) {
                    for (int k = 0; k < envFrames; ++k)
                        add(FrameName(env, k), "", envFlags, true);
                }
            }
            if (!name.empty() || !alpha.empty()) {
                if (frames < 2) {
                    add(name, alpha, tf, false);
                } else if (name.size() >= 6) {
                    const bool alphaFrames = (flags & MATF_ALPHA_FRAMES) && !(flags & MATF_NO_ALPHA_NAME) &&
                                             alpha.size() >= 6;
                    for (int k = 0; k < frames; ++k)
                        add(FrameName(name, k), alphaFrames ? FrameName(alpha, k) : alpha, tf, false);
                }
            }
        }
    }
    if (!r.ok)
        return "file ends inside the material table";
    return "";
}

}  // namespace tcg
