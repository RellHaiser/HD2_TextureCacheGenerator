// The game's file lookup over folders of loose files: a list of data roots (a mod folder, the game
// folder, an unpacked copy of the .dta archives ...) searched in order, the way dtaOpen looks at
// loose files before the archives, and IGraph's texture search directories.
#pragma once

#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace tcg {

class DataFs {
public:
    // `roots`: folders that stand for the game folder, first found wins.
    explicit DataFs(std::vector<std::string> roots);

    const std::vector<std::string> &Roots() const { return roots_; }

    // The file on disk of a game-relative path ("Maps\\X.BMP"), "" if none.
    std::string Locate(const std::string &relativePath);
    // The index of the root a file on disk lies in (-1 for none).
    int RootOf(const std::string &file) const;

    // IGraph's FindTextureFile (0x1006fbd0): the name as it is when it has a directory and exists,
    // else the first of the search directories that holds it (the last added first). The
    // original first tries the directory of the previous find; that only matters for names in
    // more than one directory and is left out. "" when not found.
    std::string FindTexture(const std::string &fileName);

    // The search directories in the order they are tried; HD2 adds Maps, Maps_C, Maps_U
    // (RegisterDataDirectories 0x006dc230), so Maps_U is tried first.
    std::vector<std::string> searchDirs = {"Maps_U", "Maps_C", "Maps"};

private:
    std::vector<std::string> roots_;
    std::unordered_map<std::string, std::string> cache_;    // lower-case relative path -> file
    std::mutex mutex_;
};

// Lower-case copy (ASCII).
std::string Lower(std::string s);
// Joins with a single backslash.
std::string JoinPath(const std::string &a, const std::string &b);

}  // namespace tcg
