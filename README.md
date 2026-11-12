# HD2 Texture Cache Generator, Rust (`hd2tcg`)

A Rust translation of [HD2_TextureCacheGenerator](../HD2_TextureCacheGenerator) (C++). It makes the
texture cache files (`.DX1`, `.DX2`, `.DX3`, `.565`, `.555`) that *Hidden & Dangerous 2: Sabre
Squadron*'s **unmodified `LS3DF.dll`** writes next to a `.bmp` / `.tga` texture the first time it
loads it. They are made ahead of time, without the game, so you can ship them with a mod.

The commands, options and output are the same as the C++ tool's. The output files are
byte-identical to it (see *What was checked*).

## Why a mod needs them

When `ITexture::Create` (LS3DF.dll 0x10073a80) loads an image, it first looks for a cache file of
the right name. It opens that file with `dtaOpen`, which checks loose files first and then the
`.dta` archives. The cache is **used if its width and height match**. The timestamps stored in it
are never compared with the image. So:

* If you replace a texture at the same resolution, the archive's cache for the old picture keeps
  being shown, unless your mod ships its own (loose) cache.
* A new texture is compressed on the player's machine the first time it loads, and the cache is
  written into their game folder.

## Cache files

Name: `<image path without extension>[_<opacity map name>]_<mip levels>[L].<ext>`

| ext    | contents | written when |
|--------|----------|--------------|
| `.DX1` | DXT1 | compressed, opaque |
| `.DX2` | DXT1 with 1-bit alpha | compressed, colour key, no mip maps |
| `.DX3` | DXT3 | compressed with alpha (opacity map, 32-bit image alpha, or colour key with mip maps) |
| `.565` | R5G6B5 | not compressed, opaque (on an adapter whose 16-bit format is R5G6B5) |
| `.555` | A1R5G5B5 / X1R5G5B5 | not compressed, colour key |

* The opacity map name is only part of `.DX3` names.
* `L` means the half-size texture setting.
* Header (little-endian): `"DXT\0"` (version 9) or `"TEX\0"` (version 1), `u16` version, `u32`
  width, `u32` height, the image file's `FILETIME`, for DXT3 also the opacity map's `FILETIME`.
  Then every mip level's surface, largest first.
* 16-bit levels have DWORD-aligned rows: a 1-pixel-wide level has 2 zero bytes of padding per row.
* 32-bit textures (`A8R8G8B8`), `A4R4G4B4` textures, `.dds` files and streamed textures get no
  cache file.

## Build

```bash
cargo build --release
```

This produces `target\release\hd2tcg.exe`. There are no dependencies beyond the standard library.
The results do not depend on the target: the encoder's arithmetic is done in software rounding
(see `src/s3tc.rs`), and the other float code is plain IEEE single precision. The checks below
were run with the x86-64 build.

`cargo test` runs the unit tests (cache names, texture sizes, frame names, rounding).

## Use

### A mod's models

```bash
target\release\hd2tcg.exe generate --data "G:\Hidden & Dangerous 2\JeepM2HB" --data H:\HD2_Unpacked "G:\Hidden & Dangerous 2\JeepM2HB\MODELS"
```

* `generate` reads the material table of every `.4ds` given (or found in the folders given),
  exactly as the game's loader does.
* It works out each texture load's `ITC_` flags as `I3D_material::TextureFlags` would, for both
  texture-size settings (full and half).
* It finds each image the way IGraph does: the search folders `Maps_U`, `Maps_C`, `Maps` in that
  order, each one looked up in every `--data` folder, the first `--data` folder first.
* It writes the cache next to the image.

`--data` folders stand for the game folder. Give the mod first and the game's unpacked data after
it. By default, caches are written only for images in the **first** `--data` folder (the mod): the
game's own textures already have caches in the archives. `--all-roots` writes them for everything.

Other options:
* `--dry-run` lists what would be written.
* `--out DIR` writes into another folder (same relative paths).
* An existing cache with the same bytes is left alone.
* A warning is printed when an image in the mod is not the one the game will load, because a file
  of the same name is found first (for example in `Maps_U` of the archives).

The defaults are the game's own settings:
* compression on;
* dithering on (HD2 never turns it off);
* mip maps where the material asks for them;
* both full and half texture size.

`--no-compress`, `--half on|off`, `--fmt16 555` and `--max-texture N` stand for other setups.

### Textures the game loads itself

The exe loads HUD images, decals, particles and so on with its own flags. For one of those:

```bash
target\release\hd2tcg.exe texture --data MyMod --data H:\HD2_Unpacked --flags dither,compress,mipmap MYTEX.BMP
target\release\hd2tcg.exe texture --data MyMod --data H:\HD2_Unpacked --flags 0x0 MYICON.BMP
```

`--flags` takes a hex number or names: `source_alpha use_alpha alpha_1bit dither mipmap compress
truecolor half noresize`. `--alpha NAME` gives an opacity map.

### Checking

```bash
target\release\hd2tcg.exe verify --data H:\HD2_Unpacked --report verify.tsv H:\HD2_Unpacked\MAPS
target\release\hd2tcg.exe info H:\HD2_Unpacked\MAPS\D_SHOVEL_1.DX1
```

`verify` works out, from each existing cache file's name, which image and flags could have made it,
remakes it, and compares the bytes. Arguments can be files, folders or `@list.txt`.

## What was checked (offline)

* **The game's 2004 caches:** all 1,464 cache files in the unpacked archives dated 2004 (the Sabre
  Squadron builds), the set the C++ tool reproduces in full. Results:
  * 1,457 are byte-identical, timestamps included;
  * 6 differ only in a timestamp;
  * 1 (`G_AF5_143_8.565`) was made from the image the archives keep as `G_AF5_143.ver`. That is the
    same as the C++ tool's result.
* **Against the C++ tool:** both tools ran `generate --all-roots` over the 244 mission models in
  `H:\HD2_Unpacked\MISSIONS`. Every file was byte-identical in all three runs:

  | Run | Files | C++ (x86) | Rust (x86-64) |
  |---|---|---|---|
  | default (dithered DXT1/DXT3, mip maps, both sizes) | 6,694 | 882 s | 97 s |
  | `--no-dither` (the DLL's undithered conversion, red/blue swap included) | 6,694 | 359 s | 80 s |
  | `--no-compress` (`.565` / `.555`) | 6,198 | 31 s | 23 s |

  The two builds were also on different targets (C++ x86, Rust x86-64).
* The caches written in October 2003 came from an older LS3DF build, which neither tool (nor the
  shipped DLL) reproduces. See the C++ project's README.

**Not tried in the game.**

## Differences from the C++ version

* The S3TC encoder's globals are the fields of an `Encoder` value: one per texture by default, or
  one per thread that carries over from texture to texture with `--carry-encoder` (which also
  means one thread). The C++ version used `thread_local` globals for the same thing.
* Only the parts of the S3TC library that ITexture uses are translated: 16-bit sources, opaque and
  1-bit-alpha DXT1, explicit-alpha DXT3. The DXT5 alpha fit, palette sources and the colour-key
  range mode are left out because LS3DF never asks for them.
* Material names are read as Latin-1 (byte for byte to characters). The C++ version passed the
  bytes to Windows' ANSI functions. The two differ only for bytes 0x80 - 0x9F in a texture name.

## Things to know

These apply to both versions:

* **The S3TC encoder carries state from one block to the next, and from one texture to the next**
  (the best cluster-fit ratio is never reset), so in the game a texture's blocks can depend on what
  was compressed earlier in the same session. The tool starts every texture from a freshly loaded
  DLL's state. All the 2004 caches match that way.
* The game also tries the folder of the previous texture find first. That only matters for a name
  in more than one search folder, and is not reproduced.
* Only loose files are read. To include the base game, use an unpacked copy of the archives.
* Only material textures come from models (normal, environment and animated-frame textures). 4DS
  frame types that carry their own texture (emitters) are not read.
* The adapter is assumed to be a modern one: R5G6B5, A1R5G5B5, A4R4G4B4, DXT1 and DXT3 textures,
  and no maximum texture size below the image's.
* Images are read the way LS3DF reads them, quirks included, with warnings for files it reads wrong:
  * BMP headers longer than 40 bytes;
  * a pixel offset that is not right after the palette;
  * RLE TGA files;
  * 8-bit TGA files without a colour map.
* A 24- or 32-bit image with a colour key treats every pixel whose **blue byte is 0** as
  transparent. That is what the DLL does.

## Source

* `src/s3tc.rs`: the S3TC encoder (S3's library as linked into LS3DF, 0x10080ed0 - 0x10086500).
  It is translated from the LS3DF port's `s3tc.cpp` operation for operation, with `r()` rounding
  each result to 24 bits as the game's x87 FPU does.
* `src/texture.rs`: `ITexture::Create`'s image path: format choice, colour key, opacity map,
  box-filter resize, mip filter, the dithered 16-bit conversion, cache file name and layout.
* `src/image.rs`: `GetImageInfo` / `LoadImage`.
* `src/model4ds.rs`: the 4DS material table and `TextureFlags`.
* `src/datafs.rs`: the data folders and the texture search.
* `src/main.rs`: the commands.
