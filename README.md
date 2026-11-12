# HD2 Texture Cache Generator (`hd2tcg`)

Makes the texture cache files (`.DX1`, `.DX2`, `.DX3`, `.565`, `.555`) that *Hidden & Dangerous 2:
Sabre Squadron*'s **unmodified `LS3DF.dll`** writes next to a `.bmp` / `.tga` texture the first time
it loads it. They are made ahead of time, without the game, so you can ship them with a mod.

## Why a mod needs them

When `ITexture::Create` (LS3DF.dll 0x10073a80) loads an image, it first looks for a cache file of the
right name. It opens that file with `dtaOpen`, which checks loose files first and then the `.dta`
archives. The cache is **used if its width and height match**. The timestamps stored in it are never
compared with the image. So:

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
* Header (little-endian): `"DXT\0"` (version 9) or `"TEX\0"` (version 1), `u16` version, `u32` width,
  `u32` height, the image file's `FILETIME`, for DXT3 also the opacity map's `FILETIME`. Then every
  mip level's surface, largest first.
* 16-bit levels have DWORD-aligned rows: a 1-pixel-wide level has 2 zero bytes of padding per row.
* 32-bit textures (`A8R8G8B8`), `A4R4G4B4` textures, `.dds` files and streamed textures get no cache file.

## Build

```bash
python build.py
```

You need Visual Studio with the C++ x86 tools. The build is x86 with the same compiler settings as
the LS3DF port the S3TC encoder comes from, and produces `build\hd2tcg.exe`.

## Use

### A mod's models

```bash
build\hd2tcg.exe generate --data "G:\Hidden & Dangerous 2\JeepM2HB" --data H:\HD2_Unpacked "G:\Hidden & Dangerous 2\JeepM2HB\MODELS"
```

* `generate` reads the material table of every `.4ds` given (or found in the folders given), exactly
  as the game's loader does.
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
build\hd2tcg.exe texture --data MyMod --data H:\HD2_Unpacked --flags dither,compress,mipmap MYTEX.BMP
build\hd2tcg.exe texture --data MyMod --data H:\HD2_Unpacked --flags 0x0 MYICON.BMP
```

`--flags` takes a hex number or names: `source_alpha use_alpha alpha_1bit dither mipmap compress
truecolor half noresize`. `--alpha NAME` gives an opacity map. Most exe loads ask for no
compression, so on a normal adapter they make a `.565` (opaque), a `.555` (colour key) or no cache
at all (alpha).

### Checking

```bash
build\hd2tcg.exe verify --data H:\HD2_Unpacked --report verify.tsv H:\HD2_Unpacked\MAPS
build\hd2tcg.exe info H:\HD2_Unpacked\MAPS\D_SHOVEL_1.DX1
```

`verify` works out, from each existing cache file's name, which image and flags could have made it,
remakes it, and compares the bytes (DXT blocks / pixels that differ, and the timestamps
separately). Arguments can be files, folders or `@list.txt`.

## What was checked (offline)

Against the 27,474 cache files in the unpacked game archives (`H:\HD2_Unpacked\MAPS*`):

* **Caches written in 2004 (the Sabre Squadron builds), 1,464 files: all reproduced.** 1,457 are
  byte-identical, including the timestamps. Six differ only in a timestamp, where the image or opacity
  map in the archives is a different copy from the one that made the cache.
  `G_AF5_143_8.565` was made from the image that the archives keep as `G_AF5_143.ver`: from that
  file it comes out identical, timestamp included.
  * This covers DXT1, DXT1 with colour key, DXT3 with an opacity map, DXT3 from 32-bit alpha, R5G6B5,
    full mip chains, half size and resizing to powers of two.
* **Caches written in October 2003, 26,010 files: an older LS3DF build made them.** That build did not
  dither, and it converted to A1R5G5B5 with red and blue in the right places. The shipped
  `LS3DF.dll`, without dithering, puts blue in the red bits (checked in Ghidra, `WriteSurface`
  0x10077d00), and with dithering (the default) it dithers. So the current DLL cannot reproduce these
  bytes, and neither does this tool. 553 match anyway (mostly gray images).

Also checked:
* Naming: the material tables of all 6,190 shipped `.4ds` models predict 65,322 cache names that
  exist in the archives. The other 170 predictions are variants the developers never shipped.
* `generate` on the JeepM2HB mod writes its 6 caches, and a second run leaves them unchanged.

**Not tried in the game.** The shipped DLL's output is matched byte for byte, so the game should
accept these files as its own. Nobody has yet loaded a mod with generated caches and looked at it.

## Things to know

* **The S3TC encoder carries state from one block to the next, and from one texture to the next**
  (the best cluster-fit ratio is never reset). In the game, a texture's blocks can therefore depend
  on what was compressed earlier in the same session. This tool starts every texture from a freshly
  loaded DLL's state (`--carry-encoder` runs them in order with one state instead). All the 2004
  caches above match with the fresh state.
* The game also tries the folder of the previous texture find first. That only matters for a name
  that exists in more than one search folder, and is not reproduced.
* Only loose files are read. To include the base game, use an unpacked copy of the archives.
* Only material textures come from models (normal, environment and animated-frame textures). 4DS
  frame types that carry their own texture (emitters) are not read.
* The adapter is assumed to be a modern one: R5G6B5, A1R5G5B5, A4R4G4B4, DXT1 and DXT3 textures, and
  no maximum texture size below the image's. That only affects `.565` / `.555` names and huge images.
* Images are read the way LS3DF reads them, quirks included, with warnings for files it reads wrong:
  * BMP headers longer than 40 bytes;
  * a pixel offset that is not right after the palette;
  * RLE TGA files;
  * 8-bit TGA files without a colour map.
* A 24- or 32-bit image with a colour key treats every pixel whose **blue byte is 0** as transparent.
  That is what the DLL does, not a bug here.

## Source

* `src/texture.cpp`: `ITexture::Create`'s image path. It covers format choice, colour key, opacity
  map, box-filter resize, mip filter, the dithered 16-bit conversion, S3TC, and the cache file name
  and layout.
* `src/image.cpp`: `GetImageInfo` / `LoadImage`.
* `src/model4ds.cpp`: the 4DS material table and `TextureFlags`.
* `src/datafs.cpp`: the data folders and the texture search.
* `src/ls3df/`: the S3TC encoder (`s3tc.cpp`, S3's library as linked into LS3DF, 0x10080ed0 -
  0x10086500) and `core/fpu.h`. They are copied from the LS3DF port
  (github.com/RellHaiser/LS3DF, commit 387d53e) with two changes: the encoder's globals are
  `thread_local`, and there is a `ResetState()`.
* The rest follows the port's `itexture.cpp`, `image_convert.cpp`, `resize_image.cpp` and
  `texture_compress.cpp`, without its enhancements.
