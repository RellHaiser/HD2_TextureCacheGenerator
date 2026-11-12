//! The game's file lookup over folders of loose files: a list of data roots (a mod folder, the
//! game folder, an unpacked copy of the .dta archives ...) searched in order, the way dtaOpen
//! looks at loose files before the archives, and IGraph's texture search directories.

use std::collections::HashMap;
use std::path::{Path, PathBuf};
use std::sync::Mutex;

use crate::texture::Files;

pub struct DataFs {
    roots: Vec<PathBuf>,
    /// The search directories in the order they are tried. HD2 adds Maps, Maps_C, Maps_U
    /// (RegisterDataDirectories 0x006dc230) and the last added is tried first.
    pub search_dirs: Vec<String>,
    cache: Mutex<HashMap<String, Option<PathBuf>>>,
}

/// ASCII lower case.
pub fn lower(s: &str) -> String {
    s.to_ascii_lowercase()
}

impl DataFs {
    /// `roots`: folders that stand for the game folder, the first that has a file wins.
    pub fn new(roots: &[PathBuf]) -> Self {
        let roots = roots.iter().map(|r| std::path::absolute(r).unwrap_or_else(|_| r.clone())).collect();
        DataFs {
            roots,
            search_dirs: vec!["Maps_U".into(), "Maps_C".into(), "Maps".into()],
            cache: Mutex::new(HashMap::new()),
        }
    }

    pub fn roots(&self) -> &[PathBuf] {
        &self.roots
    }

    /// The index of the root a file on disk lies in.
    pub fn root_of(&self, file: &Path) -> Option<usize> {
        let f = lower(&file.to_string_lossy()).replace('/', "\\");
        self.roots.iter().position(|r| {
            let mut rs = lower(&r.to_string_lossy()).replace('/', "\\");
            if !rs.ends_with('\\') {
                rs.push('\\');
            }
            f.starts_with(&rs)
        })
    }

    /// A file's path relative to its root ("Maps\\X.DX1").
    pub fn relative(&self, file: &Path) -> Option<String> {
        let root = &self.roots[self.root_of(file)?];
        let f = file.to_string_lossy().replace('/', "\\");
        let n = root.to_string_lossy().trim_end_matches(['\\', '/']).len();
        Some(f[n..].trim_start_matches('\\').to_string())
    }
}

impl Files for DataFs {
    fn locate(&self, relative: &str) -> Option<PathBuf> {
        let rel = relative.replace('/', "\\");
        let key = lower(&rel);
        if let Some(hit) = self.cache.lock().unwrap().get(&key) {
            return hit.clone();
        }
        let found = self.roots.iter().map(|r| r.join(&rel)).find(|p| p.is_file());
        self.cache.lock().unwrap().insert(key, found.clone());
        found
    }

    /// IGraph's FindTextureFile (0x1006fbd0): the name as it is when it has a directory and
    /// exists, else the first of the search directories that holds it. The original first tries
    /// the directory of the previous find; that only matters for names in more than one directory
    /// and is left out.
    fn find_texture(&self, file_name: &str) -> Option<String> {
        if file_name.contains('\\') && self.locate(file_name).is_some() {
            return Some(file_name.to_string());
        }
        self.search_dirs.iter().map(|d| format!("{d}\\{file_name}")).find(|rel| self.locate(rel).is_some())
    }
}
