#include "datafs.h"

#include <windows.h>

#include <algorithm>

namespace tcg {

std::string Lower(std::string s)
{
    for (char &c : s) {
        if (c >= 'A' && c <= 'Z')
            c = static_cast<char>(c - 'A' + 'a');
    }
    return s;
}

std::string JoinPath(const std::string &a, const std::string &b)
{
    if (a.empty())
        return b;
    if (a.back() == '\\' || a.back() == '/')
        return a + b;
    return a + "\\" + b;
}

namespace {

bool IsFile(const std::string &path)
{
    const DWORD a = GetFileAttributesA(path.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

std::string FullPath(const std::string &path)
{
    char buf[MAX_PATH * 2];
    const DWORD n = GetFullPathNameA(path.c_str(), sizeof buf, buf, nullptr);
    return n && n < sizeof buf ? std::string(buf, n) : path;
}

}  // namespace

DataFs::DataFs(std::vector<std::string> roots)
{
    for (std::string &r : roots) {
        std::string full = FullPath(r);
        while (full.size() > 3 && (full.back() == '\\' || full.back() == '/'))
            full.pop_back();
        roots_.push_back(full);
    }
}

std::string DataFs::Locate(const std::string &relativePath)
{
    std::string rel = relativePath;
    std::replace(rel.begin(), rel.end(), '/', '\\');
    const std::string key = Lower(rel);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto it = cache_.find(key);
        if (it != cache_.end())
            return it->second;
    }
    std::string found;
    for (const std::string &root : roots_) {
        const std::string p = JoinPath(root, rel);
        if (IsFile(p)) {
            found = p;
            break;
        }
    }
    std::lock_guard<std::mutex> lock(mutex_);
    cache_[key] = found;
    return found;
}

int DataFs::RootOf(const std::string &file) const
{
    const std::string f = Lower(file);
    for (size_t i = 0; i < roots_.size(); ++i) {
        const std::string r = Lower(roots_[i]) + "\\";
        if (f.compare(0, r.size(), r) == 0)
            return static_cast<int>(i);
    }
    return -1;
}

std::string DataFs::FindTexture(const std::string &fileName)
{
    if (fileName.find('\\') != std::string::npos && !Locate(fileName).empty())
        return fileName;
    for (const std::string &dir : searchDirs) {
        const std::string rel = dir + "\\" + fileName;
        if (!Locate(rel).empty())
            return rel;
    }
    return "";
}

}  // namespace tcg
