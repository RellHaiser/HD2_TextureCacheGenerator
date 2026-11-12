// hd2tcg: texture cache files (.DX1 .DX2 .DX3 .565 .555) for Hidden & Dangerous 2's unmodified
// LS3DF.dll, made without the game.
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "datafs.h"
#include "image.h"
#include "model4ds.h"
#include "texture.h"

using namespace tcg;
namespace fs = std::filesystem;

namespace {

const char kUsage[] =
    "hd2tcg - texture cache files for Hidden & Dangerous 2 (unmodified LS3DF.dll)\n"
    "\n"
    "  hd2tcg generate [options] <model.4ds | folder>...\n"
    "      The cache files the models' materials make the game write, next to their images.\n"
    "  hd2tcg texture [options] <name> [--alpha <name>] --flags <flags>\n"
    "      The cache file of one IGraph::LoadTexture call (textures the game loads itself).\n"
    "  hd2tcg verify [options] <cache file | folder>...\n"
    "      Remakes existing cache files from their images and compares them byte for byte.\n"
    "  hd2tcg info <cache file>...\n"
    "      Shows a cache file's header.\n"
    "\n"
    "Options:\n"
    "  --data <dir>        A folder standing for the game folder (with Maps, Maps_C, Maps_U);\n"
    "                      repeat for more, the first that has a file wins (put a mod folder\n"
    "                      before the game's unpacked data). Default: the current folder.\n"
    "  --out <dir>         Write cache files under <dir> instead of next to their images.\n"
    "  --all-roots         generate/texture: also write caches for images found in the second\n"
    "                      and later --data folders (default: only the first).\n"
    "  --half both|on|off  The \"half size textures\" setting the caches are for (default both,\n"
    "                      as the game's own caches).\n"
    "  --no-compress       Texture compression off: 16-bit caches instead of DXT.\n"
    "  --no-dither         Dithering off (the game never turns it off).\n"
    "  --fmt16 565|555     The adapter's 16-bit texture format (default 565).\n"
    "  --max-texture <n>   The adapter's maximum texture size (default 16384).\n"
    "  --carry-encoder     Do not reset the S3TC encoder between textures (it carries state over\n"
    "                      from texture to texture, as within one game session).\n"
    "  --dry-run           List the cache files, write nothing.\n"
    "  --jobs <n>          Threads (default: all cores).\n"
    "  --report <file>     verify: a tab-separated line per cache file.\n"
    "  -v                  More output.\n"
    "\n"
    "Flags for 'texture': a hex number or a list of names joined by ',' or '|':\n"
    "  source_alpha use_alpha alpha_1bit (colorkey) dither mipmap compress truecolor half noresize\n";

struct Options {
    std::vector<std::string> data;
    std::string out;
    bool allRoots = false;
    std::vector<bool> halves = {false, true};
    DriverStates states;
    Device device;
    BuildOptions build;
    bool dryRun = false;
    int jobs = 0;
    std::string report;
    bool verbose = false;
    // texture
    std::string alpha;
    std::string flags;
    bool flagsGiven = false;
    std::vector<std::string> args;
};

[[noreturn]] void Usage(const char *error = nullptr)
{
    if (error)
        std::fprintf(stderr, "hd2tcg: %s\n\n", error);
    std::fputs(kUsage, error ? stderr : stdout);
    std::exit(error ? 2 : 0);
}

bool ParseFlags(const std::string &text, uint32_t *out)
{
    if (text.empty())
        return false;
    char *end = nullptr;
    const unsigned long v = std::strtoul(text.c_str(), &end, 16);
    if (end && !*end) {
        *out = static_cast<uint32_t>(v);
        return true;
    }
    static const std::map<std::string, uint32_t> kNames = {
        {"source_alpha", ITC_SOURCE_ALPHA}, {"use_alpha", ITC_USE_ALPHA}, {"alpha_1bit", ITC_ALPHA_1BIT},
        {"colorkey", ITC_ALPHA_1BIT},       {"dither", ITC_DITHER},       {"mipmap", ITC_MIPMAP},
        {"compress", ITC_COMPRESS},         {"truecolor", ITC_TRUECOLOR}, {"half", ITC_HALF},
        {"noresize", ITC_NORESIZE},         {"0", 0},
    };
    uint32_t flags = 0;
    size_t start = 0;
    while (start <= text.size()) {
        size_t sep = text.find_first_of(",|+", start);
        if (sep == std::string::npos)
            sep = text.size();
        const std::string word = Lower(text.substr(start, sep - start));
        const auto it = kNames.find(word);
        if (it == kNames.end())
            return false;
        flags |= it->second;
        start = sep + 1;
    }
    *out = flags;
    return true;
}

std::string FlagNames(uint32_t f)
{
    static const std::pair<uint32_t, const char *> kNames[] = {
        {ITC_SOURCE_ALPHA, "source_alpha"}, {ITC_USE_ALPHA, "use_alpha"}, {ITC_ALPHA_1BIT, "alpha_1bit"},
        {ITC_STREAMED, "streamed"},         {ITC_DITHER, "dither"},       {ITC_MIPMAP, "mipmap"},
        {ITC_COMPRESS, "compress"},         {ITC_TRUECOLOR, "truecolor"}, {ITC_KEEP_PIXELS, "keep_pixels"},
        {ITC_HALF, "half"},                 {ITC_NORESIZE, "noresize"},
    };
    std::string s;
    for (const auto &n : kNames) {
        if (f & n.first) {
            if (!s.empty())
                s += "|";
            s += n.second;
        }
    }
    return s.empty() ? "0" : s;
}

Options ParseArgs(int argc, char **argv, int first)
{
    Options o;
    for (int i = first; i < argc; ++i) {
        const std::string a = argv[i];
        auto value = [&]() -> std::string {
            if (i + 1 >= argc)
                Usage((a + " needs a value").c_str());
            return argv[++i];
        };
        if (a == "--data") {
            o.data.push_back(value());
        } else if (a == "--out") {
            o.out = value();
        } else if (a == "--all-roots") {
            o.allRoots = true;
        } else if (a == "--half") {
            const std::string v = value();
            if (v == "both")
                o.halves = {false, true};
            else if (v == "on")
                o.halves = {true};
            else if (v == "off")
                o.halves = {false};
            else
                Usage("--half takes both, on or off");
        } else if (a == "--no-compress") {
            o.states.compress = false;
        } else if (a == "--no-dither") {
            o.states.dither = false;
        } else if (a == "--fmt16") {
            const std::string v = value();
            if (v == "565")
                o.device.fmt16 = Format::R5G6B5;
            else if (v == "555")
                o.device.fmt16 = Format::X1R5G5B5;
            else
                Usage("--fmt16 takes 565 or 555");
        } else if (a == "--max-texture") {
            const int n = std::atoi(value().c_str());
            if (n < 4)
                Usage("--max-texture must be at least 4");
            o.device.maxWidth = o.device.maxHeight = static_cast<uint32_t>(n);
        } else if (a == "--carry-encoder") {
            o.build.resetEncoder = false;
        } else if (a == "--dry-run") {
            o.dryRun = true;
        } else if (a == "--jobs") {
            o.jobs = std::atoi(value().c_str());
        } else if (a == "--report") {
            o.report = value();
        } else if (a == "--alpha") {
            o.alpha = value();
        } else if (a == "--flags") {
            o.flags = value();
            o.flagsGiven = true;
        } else if (a == "-v" || a == "--verbose") {
            o.verbose = true;
        } else if (a == "-h" || a == "--help") {
            Usage();
        } else if (a.size() > 1 && a[0] == '-') {
            Usage(("unknown option " + a).c_str());
        } else {
            o.args.push_back(a);
        }
    }
    if (o.data.empty())
        o.data.push_back(".");
    if (!o.build.resetEncoder)
        o.jobs = 1;                     // one encoder state, in a fixed order
    if (o.jobs <= 0)
        o.jobs = static_cast<int>(std::max(1u, std::thread::hardware_concurrency()));
    return o;
}

// Runs fn(i) for i in [0, n) on `jobs` threads.
template <class Fn>
void ParallelFor(size_t n, int jobs, Fn fn)
{
    std::atomic<size_t> next{0};
    auto worker = [&]() {
        for (size_t i; (i = next++) < n;)
            fn(i);
    };
    std::vector<std::thread> threads;
    const int t = static_cast<int>(std::min<size_t>(static_cast<size_t>(jobs), n));
    for (int k = 1; k < t; ++k)
        threads.emplace_back(worker);
    worker();
    for (auto &th : threads)
        th.join();
}

bool ReadWholeFile(const std::string &path, std::vector<uint8_t> *out)
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

bool WriteWholeFile(const std::string &path, const std::vector<uint8_t> &bytes)
{
    std::error_code ec;
    fs::create_directories(fs::path(path).parent_path(), ec);
    FILE *f = std::fopen(path.c_str(), "wb");
    if (!f)
        return false;
    const bool ok = std::fwrite(bytes.data(), 1, bytes.size(), f) == bytes.size();
    return std::fclose(f) == 0 && ok;
}

std::vector<std::string> CollectFiles(const std::vector<std::string> &args, const std::vector<std::string> &exts)
{
    auto wanted = [&](const fs::path &p) {
        const std::string e = Lower(p.extension().string());
        return std::find(exts.begin(), exts.end(), e) != exts.end();
    };
    std::vector<std::string> files;
    for (const std::string &a : args) {
        std::error_code ec;
        if (a.size() > 1 && a[0] == '@') {
            // A text file listing paths, one per line.
            FILE *f = std::fopen(a.c_str() + 1, "r");
            if (!f) {
                std::fprintf(stderr, "hd2tcg: cannot read %s\n", a.c_str() + 1);
                continue;
            }
            char line[1024];
            while (std::fgets(line, sizeof line, f)) {
                std::string s = line;
                while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' '))
                    s.pop_back();
                if (!s.empty() && fs::is_regular_file(s, ec) && wanted(fs::path(s)))
                    files.push_back(s);
            }
            std::fclose(f);
        } else if (fs::is_directory(a, ec)) {
            for (auto it = fs::recursive_directory_iterator(a, ec); it != fs::recursive_directory_iterator();
                 it.increment(ec)) {
                if (ec)
                    break;
                if (it->is_regular_file(ec) && wanted(it->path()))
                    files.push_back(it->path().string());
            }
        } else if (fs::is_regular_file(a, ec)) {
            files.push_back(a);
        } else {
            std::fprintf(stderr, "hd2tcg: not found: %s\n", a.c_str());
        }
    }
    std::sort(files.begin(), files.end());
    return files;
}

std::string HexTime(uint64_t t)
{
    if (!t)
        return "0";
    FILETIME ft = {static_cast<DWORD>(t), static_cast<DWORD>(t >> 32)};
    SYSTEMTIME st;
    if (!FileTimeToSystemTime(&ft, &st))
        return "?";
    char buf[64];
    std::snprintf(buf, sizeof buf, "%04u-%02u-%02u %02u:%02u:%02u UTC", st.wYear, st.wMonth, st.wDay, st.wHour,
                  st.wMinute, st.wSecond);
    return buf;
}

// ---------------------------------------------------------------- generate / texture

struct Job {
    TextureLoad load;
    std::string source;                 // the model (and material) asking for it
};

int RunLoads(const Options &o, std::vector<Job> jobs)
{
    DataFs data(o.data);
    // One cache per distinct load.
    {
        std::set<std::string> seen;
        std::vector<Job> unique;
        for (Job &j : jobs) {
            const std::string key = Lower(j.load.name) + "|" + Lower(j.load.alphaName) + "|" + std::to_string(j.load.flags);
            if (seen.insert(key).second)
                unique.push_back(std::move(j));
        }
        jobs.swap(unique);
    }
    const TextureFinder find = [&](const std::string &n) { return data.FindTexture(n); };
    const FileLocator locate = [&](const std::string &p) { return data.Locate(p); };

    std::vector<CacheResult> results(jobs.size());
    BuildOptions build = o.build;
    build.contents = !o.dryRun;
    ParallelFor(jobs.size(), o.jobs, [&](size_t i) {
        CacheResult r = BuildCache(jobs[i].load, o.device, find, locate, {false, build.resetEncoder});
        const int root = r.imageFile.empty() ? -1 : data.RootOf(r.imageFile);
        if (r.outcome == Outcome::Cache && build.contents && (root == 0 || o.allRoots))
            r = BuildCache(jobs[i].load, o.device, find, locate, build);
        results[i] = std::move(r);
    });

    int written = 0, unchanged = 0, skippedRoot = 0, noCache = 0, errors = 0, planned = 0;
    std::set<std::string> written_paths, warned;
    for (size_t i = 0; i < jobs.size(); ++i) {
        const CacheResult &r = results[i];
        for (const std::string &w : r.warnings) {
            if (warned.insert(w).second)
                std::printf("warning: %s\n", w.c_str());
        }
        if (r.outcome == Outcome::Error) {
            ++errors;
            std::printf("error: %s (%s): %s\n", jobs[i].load.name.c_str(), jobs[i].source.c_str(), r.message.c_str());
            continue;
        }
        if (r.outcome == Outcome::NoCache) {
            ++noCache;
            if (o.verbose)
                std::printf("no cache: %s: %s\n", jobs[i].load.name.c_str(), r.message.c_str());
            continue;
        }
        const int root = data.RootOf(r.imageFile);
        // Textures of the mod root only, unless asked: the others come with their own caches.
        if (root != 0 && !o.allRoots) {
            ++skippedRoot;
            if (o.verbose)
                std::printf("skip (not in %s): %s\n", data.Roots()[0].c_str(), r.cachePath.c_str());
            continue;
        }
        const std::string dest = o.out.empty() ? JoinPath(data.Roots()[root], r.cachePath) : JoinPath(o.out, r.cachePath);
        if (!written_paths.insert(Lower(dest)).second)
            continue;
        if (o.dryRun) {
            ++planned;
            std::printf("%-48s %s %ux%u, %u level%s, %s  <- %s%s%s\n", r.cachePath.c_str(), FormatName(r.format),
                        r.width, r.height, r.levels, r.levels == 1 ? "" : "s", FlagNames(r.flags).c_str(),
                        r.imagePath.c_str(), r.alphaPath.empty() ? "" : " + ", r.alphaPath.c_str());
            continue;
        }
        std::vector<uint8_t> old;
        if (ReadWholeFile(dest, &old) && old == r.bytes) {
            ++unchanged;
            if (o.verbose)
                std::printf("unchanged: %s\n", dest.c_str());
            continue;
        }
        if (!WriteWholeFile(dest, r.bytes)) {
            ++errors;
            std::printf("error: cannot write %s\n", dest.c_str());
            continue;
        }
        ++written;
        std::printf("wrote %s (%s %ux%u, %u level%s)\n", dest.c_str(), FormatName(r.format), r.width, r.height,
                    r.levels, r.levels == 1 ? "" : "s");
    }
    // Images of the first root that the game will not load because the same name is found first
    // in another search directory (or another root).
    {
        std::set<std::string> names;
        for (const Job &j : jobs)
            names.insert(j.load.name);
        for (const std::string &n : names) {
            if (n.find('\\') != std::string::npos)
                continue;
            const std::string used = data.FindTexture(n);
            if (used.empty())
                continue;
            const std::string usedFile = data.Locate(used);
            for (const std::string &dir : data.searchDirs) {
                const std::string own = JoinPath(data.Roots()[0], dir + "\\" + n);
                if (GetFileAttributesA(own.c_str()) != INVALID_FILE_ATTRIBUTES && Lower(own) != Lower(usedFile)) {
                    std::printf("warning: %s is not used: the game finds %s first (%s)\n", own.c_str(), used.c_str(),
                                usedFile.c_str());
                    break;
                }
            }
        }
    }
    if (o.dryRun)
        std::printf("\n%d cache file%s", planned, planned == 1 ? "" : "s");
    else
        std::printf("\n%d written, %d unchanged", written, unchanged);
    std::printf(", %d without a cache file, %d skipped (images outside %s), %d error%s\n", noCache, skippedRoot,
                data.Roots()[0].c_str(), errors, errors == 1 ? "" : "s");
    return errors ? 1 : 0;
}

int CmdGenerate(const Options &o)
{
    if (o.args.empty())
        Usage("generate needs model files or folders");
    const std::vector<std::string> models = CollectFiles(o.args, {".4ds"});
    std::vector<Job> jobs;
    int bad = 0;
    for (const std::string &m : models) {
        for (bool half : o.halves) {
            DriverStates st = o.states;
            st.half = half;
            std::vector<MaterialLoad> loads;
            const std::string err = ReadModelTextures(m, st, &loads);
            if (!err.empty()) {
                if (half == o.halves.front()) {
                    std::printf("warning: %s: %s\n", m.c_str(), err.c_str());
                    ++bad;
                }
            }
            for (MaterialLoad &l : loads) {
                Job j;
                j.load = l.load;
                j.source = fs::path(m).filename().string() + " material " + std::to_string(l.material + 1) +
                           (l.environment ? " (environment)" : "");
                jobs.push_back(std::move(j));
            }
        }
    }
    std::printf("%zu model file%s, %zu texture load%s\n", models.size(), models.size() == 1 ? "" : "s", jobs.size(),
                jobs.size() == 1 ? "" : "s");
    return RunLoads(o, std::move(jobs)) | (bad ? 1 : 0);
}

int CmdTexture(const Options &o)
{
    if (o.args.size() != 1)
        Usage("texture takes one texture name");
    if (!o.flagsGiven)
        Usage("texture needs --flags");
    uint32_t flags;
    if (!ParseFlags(o.flags, &flags))
        Usage("bad --flags");
    std::vector<Job> jobs;
    Job j;
    j.load.name = o.args[0];
    j.load.alphaName = o.alpha;
    j.load.flags = flags;
    j.source = "command line";
    jobs.push_back(j);
    return RunLoads(o, std::move(jobs));
}

// ---------------------------------------------------------------- verify

struct Candidate {
    TextureLoad load;
};

// The texture loads that could have written a cache file of this game-relative name.
std::vector<Candidate> Candidates(const std::string &rel, DataFs &data)
{
    std::vector<Candidate> out;
    const size_t dot = rel.rfind('.');
    if (dot == std::string::npos)
        return out;
    const std::string ext = Lower(rel.substr(dot + 1));
    std::string stem = rel.substr(0, dot);
    const size_t us = stem.rfind('_');
    if (us == std::string::npos)
        return out;
    std::string tail = stem.substr(us + 1);
    const bool half = !tail.empty() && (tail.back() == 'L' || tail.back() == 'l');
    if (half)
        tail.pop_back();
    if (tail.empty() || tail.find_first_not_of("0123456789") != std::string::npos)
        return out;
    const int levels = std::atoi(tail.c_str());
    const std::string base = stem.substr(0, us);
    const uint32_t common = (levels > 1 ? ITC_MIPMAP : 0) | (half ? ITC_HALF : 0);

    // The images that could have this name (a texture may exist as both .bmp and .tga).
    auto images = [&](const std::string &b) {
        std::vector<std::string> found;
        for (const char *e : {".bmp", ".tga"}) {
            if (!data.Locate(b + e).empty())
                found.push_back(b + e);
        }
        return found;
    };
    auto add = [&](const std::string &img, const std::string &alpha, uint32_t flags) {
        Candidate c;
        c.load.name = img;
        c.load.alphaName = alpha;
        c.load.flags = flags;
        out.push_back(c);
    };
    const uint32_t dxt = ITC_COMPRESS | ITC_DITHER | common;
    if (ext == "dx1" || ext == "dx2") {
        const uint32_t f = dxt | (ext == "dx2" ? ITC_ALPHA_1BIT : 0);
        for (const std::string &img : images(base)) {
            add(img, "", f);
            add(img, "", f & ~ITC_DITHER);
        }
    } else if (ext == "dx3") {
        // "<image>_<opacity map>" or just "<image>".
        for (size_t p = base.find('_', base.rfind('\\') + 1); p != std::string::npos; p = base.find('_', p + 1)) {
            const std::string alphaBase = base.substr(p + 1);
            for (const std::string &img : images(base.substr(0, p))) {
                for (const char *e : {".bmp", ".tga"}) {
                    if (!data.FindTexture(alphaBase + e).empty())
                        add(img, alphaBase + e, dxt | ITC_USE_ALPHA);
                }
            }
        }
        for (const std::string &img : images(base)) {
            add(img, "", dxt | ITC_SOURCE_ALPHA);
            add(img, "", dxt | ITC_ALPHA_1BIT | ITC_MIPMAP);
            add(img, "", dxt | ITC_SOURCE_ALPHA | ITC_ALPHA_1BIT | ITC_MIPMAP);
        }
    } else if (ext == "565" || ext == "555") {
        const uint32_t f = common | (ext == "555" ? ITC_ALPHA_1BIT : 0);
        for (const std::string &img : images(base)) {
            for (uint32_t extra : {0u, static_cast<uint32_t>(ITC_SOURCE_ALPHA)}) {
                add(img, "", f | extra);
                add(img, "", f | extra | ITC_DITHER);
            }
        }
    }
    return out;
}

enum class Verdict { Exact, SameData, Differs, Unresolved, Error };

struct VerifyResult {
    Verdict verdict = Verdict::Unresolved;
    std::string detail;
    uint32_t flags = 0;
    size_t units = 0, unitsDiffering = 0;    // blocks (DXT) or pixels (16-bit)
};

int CmdVerify(const Options &o)
{
    if (o.args.empty())
        Usage("verify needs cache files or folders");
    DataFs data(o.data);
    const std::vector<std::string> files = CollectFiles(o.args, {".dx1", ".dx2", ".dx3", ".565", ".555"});
    const TextureFinder find = [&](const std::string &n) { return data.FindTexture(n); };
    const FileLocator locate = [&](const std::string &p) { return data.Locate(p); };
    std::vector<VerifyResult> results(files.size());
    std::atomic<size_t> done{0};
    std::mutex printMutex;

    ParallelFor(files.size(), o.jobs, [&](size_t i) {
        VerifyResult &v = results[i];
        const std::string &file = files[i];
        const int root = data.RootOf(file);
        std::vector<uint8_t> shipped;
        if (root < 0) {
            v.verdict = Verdict::Error;
            v.detail = "not inside a --data folder";
        } else if (!ReadWholeFile(file, &shipped)) {
            v.verdict = Verdict::Error;
            v.detail = "cannot read";
        } else {
            const std::string rel = file.substr(data.Roots()[root].size() + 1);
            const std::string ext = Lower(rel.substr(rel.rfind('.') + 1));
            CacheHeader h;
            if (!ParseCacheHeader(shipped, ext == "dx3", &h) ||
                std::memcmp(h.magic, ext[0] == 'd' ? "DXT" : "TEX", 4) != 0) {
                v.verdict = Verdict::Error;
                v.detail = "not a cache file";
            } else {
                const std::vector<Candidate> cands = Candidates(rel, data);
                bool any = false;
                VerifyResult best;
                best.verdict = Verdict::Unresolved;
                best.detail = cands.empty() ? "no image for this name" : "no texture load makes this name";
                for (const Candidate &c : cands) {
                    CacheResult r = BuildCache(c.load, o.device, find, locate, o.build);
                    if (r.outcome != Outcome::Cache || Lower(r.cachePath) != Lower(rel)) {
                        if (r.outcome == Outcome::Error && !any)
                            best.detail = r.message;
                        continue;
                    }
                    any = true;
                    VerifyResult cur;
                    cur.flags = r.flags;
                    CacheHeader g;
                    ParseCacheHeader(r.bytes, ext == "dx3", &g);
                    const bool sameSize = r.bytes.size() == shipped.size();
                    const bool sameData =
                        sameSize && std::equal(r.bytes.begin() + g.dataOffset, r.bytes.end(), shipped.begin() + h.dataOffset);
                    const bool sameDims = g.width == h.width && g.height == h.height && g.version == h.version;
                    if (sameData && sameDims) {
                        const bool sameTimes = g.time == h.time && g.alphaTime == h.alphaTime;
                        cur.verdict = sameTimes ? Verdict::Exact : Verdict::SameData;
                        cur.detail = sameTimes ? "" : "image time " + HexTime(h.time) + " in the file, " + HexTime(g.time) + " on disk";
                    } else if (!sameSize || !sameDims) {
                        cur.verdict = Verdict::Differs;
                        cur.detail = "size: " + std::to_string(h.width) + "x" + std::to_string(h.height) + ", " +
                                     std::to_string(shipped.size()) + " bytes in the file; made " +
                                     std::to_string(g.width) + "x" + std::to_string(g.height) + ", " +
                                     std::to_string(r.bytes.size()) + " bytes";
                        cur.units = 1;
                        cur.unitsDiffering = 1;
                    } else {
                        const size_t unit = ext == "dx1" || ext == "dx2" ? 8 : ext == "dx3" ? 16 : 2;
                        const size_t n = (shipped.size() - h.dataOffset) / unit;
                        size_t diff = 0;
                        for (size_t k = 0; k < n; ++k) {
                            if (std::memcmp(shipped.data() + h.dataOffset + k * unit, r.bytes.data() + g.dataOffset + k * unit,
                                            unit) != 0)
                                ++diff;
                        }
                        cur.verdict = Verdict::Differs;
                        cur.units = n;
                        cur.unitsDiffering = diff;
                        cur.detail = std::to_string(diff) + " of " + std::to_string(n) +
                                     (unit == 2 ? " pixels" : " blocks") + " differ";
                    }
                    if (best.verdict == Verdict::Unresolved || cur.verdict < best.verdict ||
                        (cur.verdict == Verdict::Differs && best.verdict == Verdict::Differs &&
                         cur.unitsDiffering * best.units < best.unitsDiffering * cur.units))
                        best = cur;
                    if (best.verdict == Verdict::Exact)
                        break;
                }
                v = best;
            }
        }
        const size_t d = ++done;
        if (o.verbose || v.verdict == Verdict::Differs || v.verdict == Verdict::Error) {
            static const char *kNames[] = {"exact", "same data", "DIFFERS", "unresolved", "ERROR"};
            std::lock_guard<std::mutex> lock(printMutex);
            std::printf("%-10s %s  %s\n", kNames[static_cast<int>(v.verdict)], file.c_str(), v.detail.c_str());
        } else if (d % 1000 == 0) {
            std::lock_guard<std::mutex> lock(printMutex);
            std::fprintf(stderr, "  %zu / %zu\n", d, files.size());
        }
    });

    size_t counts[5] = {};
    size_t units = 0, unitsDiff = 0;
    for (const VerifyResult &v : results) {
        ++counts[static_cast<int>(v.verdict)];
        if (v.verdict == Verdict::Differs) {
            units += v.units;
            unitsDiff += v.unitsDiffering;
        }
    }
    if (!o.report.empty()) {
        FILE *f = std::fopen(o.report.c_str(), "w");
        if (f) {
            static const char *kNames[] = {"exact", "same_data", "differs", "unresolved", "error"};
            std::fprintf(f, "file\tverdict\tflags\tdetail\n");
            for (size_t i = 0; i < files.size(); ++i)
                std::fprintf(f, "%s\t%s\t%s\t%s\n", files[i].c_str(), kNames[static_cast<int>(results[i].verdict)],
                             results[i].verdict <= Verdict::Differs ? FlagNames(results[i].flags).c_str() : "",
                             results[i].detail.c_str());
            std::fclose(f);
        } else {
            std::fprintf(stderr, "hd2tcg: cannot write %s\n", o.report.c_str());
        }
    }
    std::printf("\n%zu cache files: %zu identical, %zu identical but for the image time stamps, %zu differ "
                "(%zu of %zu blocks/pixels), %zu unresolved, %zu errors\n",
                files.size(), counts[0], counts[1], counts[2], unitsDiff, units, counts[3], counts[4]);
    return counts[2] || counts[4] ? 1 : 0;
}

int CmdInfo(const Options &o)
{
    if (o.args.empty())
        Usage("info needs cache files");
    int bad = 0;
    for (const std::string &file : o.args) {
        std::vector<uint8_t> bytes;
        const std::string ext = Lower(fs::path(file).extension().string());
        CacheHeader h;
        if (!ReadWholeFile(file, &bytes) || !ParseCacheHeader(bytes, ext == ".dx3", &h)) {
            std::printf("%s: not a cache file\n", file.c_str());
            ++bad;
            continue;
        }
        std::printf("%s\n  magic %.3s, version %u, %ux%u, image time %s", file.c_str(), h.magic, h.version, h.width,
                    h.height, HexTime(h.time).c_str());
        if (h.hasAlphaTime)
            std::printf(", opacity map time %s", HexTime(h.alphaTime).c_str());
        const Format f = ext == ".dx1" || ext == ".dx2" ? Format::DXT1 : ext == ".dx3" ? Format::DXT3 : Format::R5G6B5;
        size_t pos = h.dataOffset;
        uint32_t w = h.width, ht = h.height;
        int levels = 0;
        while (pos < bytes.size() && w && ht) {
            const size_t bw = (w + 3) / 4, bh = (ht + 3) / 4;
            const size_t size = f == Format::DXT1   ? bw * bh * 8
                                : f == Format::DXT3 ? bw * bh * 16
                                                    : size_t((w * 2 + 3) & ~3u) * ht;
            pos += size;
            ++levels;
            if (w == 1 && ht == 1)
                break;
            w = w > 1 ? w >> 1 : 1;
            ht = ht > 1 ? ht >> 1 : 1;
        }
        std::printf("\n  %d level%s%s\n", levels, levels == 1 ? "" : "s",
                    pos == bytes.size() ? "" : " (the size does not match a mip chain)");
    }
    return bad ? 1 : 0;
}

}  // namespace

int main(int argc, char **argv)
{
    if (argc < 2)
        Usage();
    const std::string cmd = argv[1];
    if (cmd == "-h" || cmd == "--help" || cmd == "help")
        Usage();
    const Options o = ParseArgs(argc, argv, 2);
    if (cmd == "generate")
        return CmdGenerate(o);
    if (cmd == "texture")
        return CmdTexture(o);
    if (cmd == "verify")
        return CmdVerify(o);
    if (cmd == "info")
        return CmdInfo(o);
    Usage(("unknown command " + cmd).c_str());
}
