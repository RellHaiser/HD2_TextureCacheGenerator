//! ITexture::Create's image path and cache files (LS3DF.dll 0x10073a80 - 0x100775f0), computed on
//! the CPU: which file a texture load writes (.DX1 / .DX2 / .DX3 / .565 / .555) and its exact
//! contents. Follows the LS3DF port (src/igraph/itexture.cpp, image_convert.cpp,
//! resize_image.cpp, texture_compress.cpp) without its enhancements.

use std::path::PathBuf;

use crate::image::{load_image, read_image_info, Image};
use crate::s3tc::{self, Encoder};

// ITexture::Create / IGraph::LoadTexture flags (ITC_*), as far as they matter here.
pub const ITC_FROM_MEMORY: u32 = 0x0000_0001;
pub const ITC_SOURCE_ALPHA: u32 = 0x0000_0002;
pub const ITC_USE_ALPHA: u32 = 0x0000_0004;
pub const ITC_ALPHA_1BIT: u32 = 0x0000_0008;
pub const ITC_STREAMED: u32 = 0x0000_0040;
pub const ITC_DITHER: u32 = 0x0000_0080;
pub const ITC_MIPMAP: u32 = 0x0000_0100;
pub const ITC_COMPRESS: u32 = 0x0000_0200;
pub const ITC_TRUECOLOR: u32 = 0x0000_0400;
pub const ITC_KEEP_PIXELS: u32 = 0x0000_1000;
pub const ITC_HALF: u32 = 0x0000_2000;
pub const ITC_DEFAULT: u32 = 0x0001_0000;
pub const ITC_NORESIZE: u32 = 0x0040_0000;
/// Options a .dds file does not take (LoadTexture clears them).
pub const ITC_DDS_IGNORED: u32 = ITC_DITHER | ITC_MIPMAP | ITC_COMPRESS | ITC_TRUECOLOR | ITC_HALF;

#[derive(Clone, Copy, PartialEq, Eq, Debug, Default)]
pub enum Format {
    #[default]
    Unknown,
    R5G6B5,
    X1R5G5B5,
    A1R5G5B5,
    A4R4G4B4,
    A8R8G8B8,
    Dxt1,
    Dxt3,
}

impl Format {
    pub fn name(self) -> &'static str {
        match self {
            Format::R5G6B5 => "R5G6B5",
            Format::X1R5G5B5 => "X1R5G5B5",
            Format::A1R5G5B5 => "A1R5G5B5",
            Format::A4R4G4B4 => "A4R4G4B4",
            Format::A8R8G8B8 => "A8R8G8B8",
            Format::Dxt1 => "DXT1",
            Format::Dxt3 => "DXT3",
            Format::Unknown => "unknown",
        }
    }
}

/// What IGraph::Init found on the display adapter. Modern adapters (and the usual Direct3D 8
/// wrappers) give the defaults.
#[derive(Clone, Copy)]
pub struct Device {
    /// g_fmt16Bit: the first of R5G6B5, X1R5G5B5, A1R5G5B5 the adapter has.
    pub fmt16: Format,
    /// DXT1 and DXT3 textures (and IGCAPS_DXT).
    pub dxt: bool,
    pub max_width: u32,
    pub max_height: u32,
}

impl Default for Device {
    fn default() -> Self {
        Device { fmt16: Format::R5G6B5, dxt: true, max_width: 16384, max_height: 16384 }
    }
}

/// One IGraph::LoadTexture call: the names as the caller gives them and the ITC_ flags.
#[derive(Clone, Default, Debug)]
pub struct TextureLoad {
    pub name: String,
    /// "" for none.
    pub alpha_name: String,
    pub flags: u32,
}

/// How texture files are found: IGraph's search (FindTextureFile 0x1006fbd0) turns a file name
/// into a game-relative path, and that path is found on disk.
pub trait Files {
    /// The game-relative path of a texture file name ("Maps\\X.BMP"), None if not found.
    fn find_texture(&self, file_name: &str) -> Option<String>;
    /// The file on disk of a game-relative path.
    fn locate(&self, relative: &str) -> Option<PathBuf>;
}

#[derive(Clone, Copy, PartialEq, Eq, Debug, Default)]
pub enum Outcome {
    /// The load writes this cache file.
    Cache,
    /// The load succeeds but writes no cache file.
    NoCache,
    /// The load fails in the game.
    #[default]
    Error,
}

#[derive(Default)]
pub struct CacheResult {
    pub outcome: Outcome,
    /// Why NoCache / Error.
    pub message: String,
    /// Game-relative, e.g. "Maps\\X_8L.DX1".
    pub cache_path: String,
    /// The game-relative paths the texture was loaded from ...
    pub image_path: String,
    pub alpha_path: String,
    /// ... and those files on disk.
    pub image_file: Option<PathBuf>,
    pub alpha_file: Option<PathBuf>,
    /// ITC_ flags in effect (ITexture::flags).
    pub flags: u32,
    pub format: Format,
    /// 0 uncompressed, 1 DXT1, 2 DXT1 with 1-bit alpha, 3 DXT3.
    pub kind: i32,
    pub width: u16,
    pub height: u16,
    pub levels: u16,
    /// The cache file (when asked for).
    pub bytes: Vec<u8>,
    pub warnings: Vec<String>,
}

#[derive(Clone, Copy)]
pub struct BuildOptions {
    /// Compute the file (else only its name).
    pub contents: bool,
}

// Versions of the compressed-file caches (0x1009dc30 / 0x1009dc32).
const DXT_CACHE_VERSION: u16 = 9;
const TEX_CACHE_VERSION: u16 = 1;

/// The part of a path after its last '\' ('/' is not a separator for the game).
fn base_name(path: &str) -> &str {
    path.rfind('\\').map_or(path, |i| &path[i + 1..])
}

/// strncpy(buf, name, strlen(name) - 4): the name without its 3-letter extension.
fn without_extension(name: &str) -> &str {
    if name.len() >= 4 {
        &name[..name.len() - 4]
    } else {
        ""
    }
}

/// The texture size for an image: halved with ITC_HALF, scaled down to the device maximum (both
/// sides by the same factor), then rounded up to powers of two (at least 4 with DXT support).
fn fit_texture_size(width: u16, height: u16, flags: u32, device: &Device) -> (u16, u16) {
    let (mut w, mut h) = (width, height);
    if flags & ITC_HALF != 0 {
        w >>= 1;
        h >>= 1;
    }
    let mut scale_x = 1.0f32;
    if u32::from(w) > device.max_width {
        scale_x = device.max_width as f32 / f32::from(w);
    }
    let mut scale_y = 1.0f32;
    if u32::from(h) > device.max_height {
        scale_y = device.max_height as f32 / f32::from(h);
    }
    let scale = if scale_x < scale_y { scale_x } else { scale_y };
    let fit_w = (f32::from(w) * scale) as i32 as u16;
    let fit_h = (f32::from(h) * scale) as i32 as u16;
    let min = if device.dxt { 4u32 } else { 1 };
    let (mut tw, mut th) = (min, min);
    while fit_w > tw as u16 && tw < 0x8000 {
        tw <<= 1;
    }
    while fit_h > th as u16 && th < 0x8000 {
        th <<= 1;
    }
    (tw as u16, th as u16)
}

/// A full mip chain's level count (CreateTexture with 0 levels).
fn full_chain(width: u16, height: u16) -> u16 {
    let mut m = u32::from(width.max(height));
    let mut n = 1;
    while m > 1 {
        m >>= 1;
        n += 1;
    }
    n
}

/// Palette entry / pixel in the bits of a texture format, for the colour-key compare.
fn key_color(format: Format, r: u8, g: u8, b: u8) -> Option<u32> {
    let (r, g, b) = (u32::from(r), u32::from(g), u32::from(b));
    match format {
        Format::A8R8G8B8 => Some(r << 16 | g << 8 | b),
        Format::R5G6B5 => Some((((r & 0xf8) << 5 | (g & 0xfc)) << 3) | (b >> 3)),
        Format::X1R5G5B5 | Format::A1R5G5B5 | Format::Dxt1 => Some((((r & 0xf8) << 5 | (g & 0xf8)) << 2) | (b >> 3)),
        Format::A4R4G4B4 | Format::Dxt3 => Some(((r & 0xf0) << 4 | (g & 0xf0)) | (b >> 4)),
        Format::Unknown => None,
    }
}

/// The mip filter of Create: a 2x2 block to one pixel with weights 3/8 for the pixels right of and
/// below the block's first one, 1/8 for the other two; every term truncated, summed in 8 bits.
/// Rows of the destination are half the source row bytes apart.
fn downsample(src: &[u8], width: i32, height: i32, row_bytes: i32, dst: &mut [u8]) {
    if height - 1 <= 0 {
        return;
    }
    let rows = ((height - 2) >> 1) + 1;
    let cols = if width - 1 > 0 { ((width - 2) >> 1) + 1 } else { 0 };
    let dst_row = (row_bytes >> 1) as usize;
    let rb = row_bytes as usize;
    for y in 0..rows as usize {
        let r0 = y * 2 * rb;
        let r1 = r0 + rb;
        let d = y * dst_row;
        for x in 0..cols as usize {
            let (p00, p01) = (r0 + x * 8, r1 + x * 8);
            for c in 0..4 {
                let v = (3 * u32::from(src[p00 + 4 + c]) >> 3)
                    + (3 * u32::from(src[p01 + c]) >> 3)
                    + (u32::from(src[p01 + 4 + c]) >> 3)
                    + (u32::from(src[p00 + c]) >> 3);
                dst[d + x * 4 + c] = v as u8;
            }
        }
    }
}

/// lrintf (round half to even) and the conversion to the byte the original stores.
#[inline]
fn round_byte(v: f32) -> u8 {
    v.round_ties_even() as i64 as u8
}

/// IGraph::ResizeImage (0x10070160), 4 bytes per pixel, filtered.
fn resize_image(sw: i32, sh: i32, sp: i32, src: &[u8], dw: i32, dh: i32, dst: &mut [u8]) {
    const BPP: i32 = 4;
    let mut o = 0usize;
    if sw < dw || sh < dh {
        // Nearest source pixel.
        let step_x = sw as f32 / dw as f32;
        let step_y = sh as f32 / dh as f32;
        for y in 0..dh {
            let row = (y as f32 * step_y) as i32 * sp;
            for x in 0..dw {
                let col = (x as f32 * step_x) as i32;
                let at = (col * BPP + row) as usize;
                dst[o..o + 4].copy_from_slice(&src[at..at + 4]);
                o += 4;
            }
        }
        return;
    }

    // Box filter over the source area of each destination pixel, partial pixels at the edges
    // weighted by their covered fraction. The scale maps onto sw - 1 (sh - 1) pixels. Float
    // operations in the original's order.
    let src_size = sp as usize * sh as usize;
    let scale_x = (sw - 1) as f32 / dw as f32;
    let scale_y = (sh - 1) as f32 / dh as f32;
    const EPS: f32 = 1e-08;
    for y in 0..dh {
        for x in 0..dw {
            let top = y as f32 * scale_y;
            let bottom = (y + 1) as f32 * scale_y;
            let y0 = top as i32 + 1;
            let y1 = bottom as i32;
            let (w_top, w_bottom) = if y0 > y1 { (bottom - top, 0.0) } else { (y0 as f32 - top, bottom - y1 as f32) };
            let rows = (y1 - y0).max(0);
            let left = x as f32 * scale_x;
            let right = (x + 1) as f32 * scale_x;
            let x0 = left as i32 + 1;
            let x1 = right as i32;
            let (w_left, w_right) = if x0 > x1 { (right - left, 0.0) } else { (x0 as f32 - left, right - x1 as f32) };
            let cols = (x1 - x0).max(0);

            let (mut b, mut g, mut r, mut a, mut weight) = (0.0f32, 0.0f32, 0.0f32, 0.0f32, 0.0f32);
            let at = |px: i32, py: i32| (py * sp + px * BPP) as usize;
            let add = |px: i32, py: i32, w: f32, r: &mut f32, g: &mut f32, b: &mut f32, a: &mut f32| {
                let p = at(px, py);
                *r += f32::from(src[p + 2]) * w;
                *g += f32::from(src[p + 1]) * w;
                *b += f32::from(src[p]) * w;
                *a += f32::from(src[p + 3]) * w;
            };
            let corner = |px: i32,
                          py: i32,
                          wx: f32,
                          wy: f32,
                          r: &mut f32,
                          g: &mut f32,
                          b: &mut f32,
                          a: &mut f32,
                          weight: &mut f32| {
                let p = at(px, py);
                *r += f32::from(src[p + 2]) * wx * wy;
                *g += f32::from(src[p + 1]) * wx * wy;
                *b += f32::from(src[p]) * wx * wy;
                *a += f32::from(src[p + 3]) * wx * wy;
                *weight = wx * wy + *weight;
            };
            if w_top > EPS {
                if w_left > EPS {
                    corner(x0 - 1, y0 - 1, w_left, w_top, &mut r, &mut g, &mut b, &mut a, &mut weight);
                }
                if cols > 0 {
                    for c in 0..cols {
                        add(x0 + c, y0 - 1, w_top, &mut r, &mut g, &mut b, &mut a);
                    }
                    weight = cols as f32 * w_top + weight;
                }
                if w_right > EPS {
                    corner(x0 + cols, y0 - 1, w_right, w_top, &mut r, &mut g, &mut b, &mut a, &mut weight);
                }
            }
            if rows > 0 {
                if w_left > EPS {
                    for k in 0..rows {
                        add(x0 - 1, y0 + k, w_left, &mut r, &mut g, &mut b, &mut a);
                    }
                    weight = rows as f32 * w_left + weight;
                }
                if cols > 0 {
                    // Original bug kept: the inner block advances its pixel pointer twice per
                    // column (pixels x0, x0 + 2 ...). Beyond the source buffer the original read
                    // whatever memory followed; this reads 0, as the port does.
                    for k in 0..rows {
                        for c in 0..cols {
                            let p = ((y0 + k) * sp + (x0 + 2 * c) * BPP) as usize;
                            let byte = |i: usize| if p + i < src_size { src[p + i] } else { 0 };
                            r += f32::from(byte(2));
                            g += f32::from(byte(1));
                            b += f32::from(byte(0));
                            a += f32::from(byte(3));
                        }
                    }
                    weight = (cols * rows) as f32 + weight;
                }
                if w_right > EPS {
                    for k in 0..rows {
                        add(x0 + cols, y0 + k, w_right, &mut r, &mut g, &mut b, &mut a);
                    }
                    weight = rows as f32 * w_right + weight;
                }
            }
            if w_bottom > EPS {
                let py = y0 + rows;
                if w_left > EPS {
                    corner(x0 - 1, py, w_left, w_bottom, &mut r, &mut g, &mut b, &mut a, &mut weight);
                }
                if cols > 0 {
                    for c in 0..cols {
                        add(x0 + c, py, w_bottom, &mut r, &mut g, &mut b, &mut a);
                    }
                    weight = cols as f32 * w_bottom + weight;
                }
                if w_right > EPS {
                    corner(x0 + cols, py, w_right, w_bottom, &mut r, &mut g, &mut b, &mut a, &mut weight);
                }
            }
            if weight > EPS || weight.is_nan() {
                let inv = 1.0 / weight;
                r *= inv;
                g *= inv;
                b *= inv;
                a *= inv;
            } else {
                r = 255.0;
                g = 0.0;
                b = 255.0;
                a = 255.0;
            }
            dst[o + 2] = round_byte(r);
            dst[o + 1] = round_byte(g);
            dst[o] = round_byte(b);
            dst[o + 3] = round_byte(a);
            o += 4;
        }
    }
}

/// Error-diffusion buffer of the dithered conversions: (height + 1) rows of (width + 2) entries
/// of red, green, blue, alpha. Pixel (x, y) reads entry (x + 1, y) and passes its error to
/// (x + 2, y), (x, y + 1), (x + 1, y + 1), (x + 2, y + 1) with weights 7, 3, 5, 1 (/16).
struct Errors {
    stride: usize,
    data: Vec<f32>,
}

impl Errors {
    fn new(width: u32, height: u32) -> Self {
        let stride = width as usize + 2;
        Errors { stride, data: vec![0.0; (height as usize + 1) * stride * 4] }
    }
    fn at(&self, x: u32, y: u32) -> usize {
        (y as usize * self.stride + x as usize) * 4
    }
    fn channel(&mut self, x: u32, y: u32, ch: usize, value: u8, step: f32) -> i32 {
        let v = f32::from(value) + self.data[self.at(x + 1, y) + ch];
        let e = (v % step) * 0.0625;
        let right = self.at(x + 2, y) + ch;
        let below = self.at(x, y + 1) + ch;
        self.data[right] = e * 7.0 + self.data[right];
        self.data[below] = e * 3.0 + self.data[below];
        self.data[below + 4] = e * 5.0 + self.data[below + 4];
        self.data[below + 8] = e + self.data[below + 8];
        let clamped = if v > 255.0 { 255.0 } else { v };
        clamped.round_ties_even() as i32
    }
}

/// WriteSurface (0x10077d00): a 32-bit BGRA image into a 16-bit surface of `format` (rows
/// width * 2 bytes apart), Floyd-Steinberg dithered with `dither` and kind 0.
fn write_surface16(bgra: &[u8], format: Format, width: u32, height: u32, dither: bool, kind: i32) -> Vec<u16> {
    let mut out = vec![0u16; width as usize * height as usize];
    let diffuse = dither && kind == 0;
    let mut err = Errors::new(if diffuse { width } else { 0 }, if diffuse { height } else { 0 });
    let mut i = 0usize;
    for y in 0..height {
        for x in 0..width {
            let p = &bgra[i * 4..i * 4 + 4];
            let (pb, pg, pr, pa) = (i32::from(p[0]), i32::from(p[1]), i32::from(p[2]), i32::from(p[3]));
            out[i] = match format {
                Format::R5G6B5 => {
                    if diffuse {
                        let r = err.channel(x, y, 0, p[2], 8.0);
                        let g = err.channel(x, y, 1, p[1], 4.0);
                        let b = err.channel(x, y, 2, p[0], 8.0);
                        ((((r & 0xfff8) << 5 | (g & 0xfc)) << 3) | ((b >> 3) & 0x1f)) as u16
                    } else {
                        ((((pr & 0xf8) << 5 | (pg & 0xfc)) << 3) | (pb >> 3)) as u16
                    }
                }
                Format::X1R5G5B5 | Format::A1R5G5B5 => {
                    if diffuse {
                        let r = err.channel(x, y, 0, p[2], 8.0);
                        let g = err.channel(x, y, 1, p[1], 8.0);
                        let b = err.channel(x, y, 2, p[0], 8.0);
                        let a = err.channel(x, y, 3, p[3], 128.0);
                        (((((a & 0xff80) << 1 | (r & 0xf8)) << 5 | (g & 0xf8)) << 2) | ((b >> 3) & 0x1f)) as u16
                    } else {
                        // (Blue in the red bits and red in the blue bits, as in the original.)
                        (((((pa & 0x80) << 1 | (pb & 0xf8)) << 5 | (pg & 0xf8)) << 2) | (pr >> 3)) as u16
                    }
                }
                Format::A4R4G4B4 => {
                    if diffuse {
                        // (The original takes a step of 8 for every 4-bit channel, not 16.)
                        let r = err.channel(x, y, 0, p[2], 8.0);
                        let g = err.channel(x, y, 1, p[1], 8.0);
                        let b = err.channel(x, y, 2, p[0], 8.0);
                        let a = err.channel(x, y, 3, p[3], 8.0);
                        (((a & 0xfff0) << 8 | (r & 0xf0) << 4) | (g & 0xf0) | ((b >> 4) & 0xf)) as u16
                    } else {
                        ((((pa & 0xf0) << 4 | (pr & 0xf0)) << 4) | (pg & 0xf0) | (pb >> 4)) as u16
                    }
                }
                _ => 0,
            };
            i += 1;
        }
    }
    out
}

/// CompressSurface (0x100788b0): a 16-bit staging level through the S3TC encoder. `kind` 1:
/// X1R5G5B5 to DXT1, 2: A1R5G5B5 to DXT1 with transparency, 3: A4R4G4B4 to DXT3.
fn compress(encoder: &mut Encoder, kind: i32, source: &[u16], width: u32, height: u32, out: &mut Vec<u8>) {
    const WEIGHTS: [f32; 3] = [0.3086, 0.6094, 0.082];
    let bytes: Vec<u8> = source.iter().flat_map(|v| v.to_le_bytes()).collect();
    let (masks, ty) = match kind {
        1 => ([0x7c00, 0x3e0, 0x1f, 0x8000], s3tc::S3TC_OPAQUE),
        2 => ([0x7c00, 0x3e0, 0x1f, 0x8000], s3tc::S3TC_ALPHA_KEY),
        _ => ([0xf00, 0xf0, 0xf, 0xf000], s3tc::S3TC_ALPHA_EXPLICIT),
    };
    let src = s3tc::Source { width, height, pitch: width as usize * 2, pixels: &bytes, masks };
    encoder.encode(&src, out, ty, &WEIGHTS);
}

/// The row pitch of a 16-bit level: whole DWORDs, so a 1-pixel-wide level has 2 bytes of padding
/// per row (seen in the shipped G_AF5_143_8.565, whose 1x2 and 1x1 levels are 8 and 4 bytes).
pub fn pitch16(width: u32) -> u32 {
    (width * 2 + 3) & !3
}

/// ITexture's cache file name (0x10073f00 inlined): the image path without its extension, for
/// DXT3 "_" and the opacity map's file name without extension, "_" levels, "L" for ITC_HALF, then
/// ".DX<kind>" or ".565" / ".555".
pub fn cache_file_name(image_path: &str, alpha_file_name: &str, kind: i32, levels: u16, half: bool, format: Format) -> String {
    let mut s = without_extension(image_path).to_string();
    let dxt = kind >= 1;
    if dxt && kind == 3 && !alpha_file_name.is_empty() {
        s.push('_');
        s.push_str(without_extension(alpha_file_name));
    }
    s.push_str(&format!("_{levels}"));
    if half {
        s.push('L');
    }
    s.push('.');
    if dxt {
        s.push_str(&format!("DX{kind}"));
    } else {
        s.push_str(if format == Format::R5G6B5 { "565" } else { "555" });
    }
    s
}

/// The header of a cache file.
#[derive(Default, Clone, Copy)]
pub struct CacheHeader {
    pub magic: [u8; 4],
    pub version: u16,
    pub width: u32,
    pub height: u32,
    pub time: u64,
    pub alpha_time: u64,
    pub has_alpha_time: bool,
    pub data_offset: usize,
}

pub fn parse_cache_header(file: &[u8], dxt3: bool) -> Option<CacheHeader> {
    let need = if dxt3 { 30 } else { 22 };
    if file.len() < need {
        return None;
    }
    let u32_at = |o: usize| u32::from_le_bytes(file[o..o + 4].try_into().unwrap());
    let u64_at = |o: usize| u64::from_le_bytes(file[o..o + 8].try_into().unwrap());
    Some(CacheHeader {
        magic: file[0..4].try_into().unwrap(),
        version: u16::from_le_bytes([file[4], file[5]]),
        width: u32_at(6),
        height: u32_at(10),
        time: u64_at(14),
        alpha_time: if dxt3 { u64_at(22) } else { 0 },
        has_alpha_time: dxt3,
        data_offset: need,
    })
}

/// The 32-bit BGRA pixels of the image (top row first; the sources are bottom-up), with the
/// colour key, the opacity map or the image's alpha.
fn build_pixels(image: &Image, gray: &[u8], gray_w: i32, gray_h: i32, flags: u32, fmt: Format) -> Vec<u8> {
    let (w, h) = (image.width, image.height);
    let mut pixels = vec![0u8; w as usize * h as usize * 4];
    let mut color_key: u32 = 0;
    let pitch = image.pitch as isize;
    let row_back = ((image.bit_count >> 3) as i32 * w) as isize + pitch;
    let gray_back = (gray_w * 2) as isize;
    let bits = &image.bits;
    let mut out = 0usize;
    // The opacity map is read from its last row up, like the image.
    let mut alpha: Option<isize> = if gray.is_empty() { None } else { Some((gray_h - 1) as isize * gray_w as isize) };
    let mut src = (h - 1) as isize * pitch;
    let one_bit = flags & ITC_ALPHA_1BIT != 0;
    let source_alpha = flags & ITC_SOURCE_ALPHA != 0;
    let next_alpha = |alpha: &mut Option<isize>| -> u8 {
        let a = alpha.as_mut().unwrap();
        let v = gray[*a as usize];
        *a += 1;
        v
    };
    match image.bit_count {
        8 => {
            let pal = &image.palette;
            if let Some(k) = key_color(fmt, pal[2], pal[1], pal[0]) {
                color_key = k;
            }
            for _ in 0..h {
                for _ in 0..w {
                    let e = bits[src as usize] as usize * 4;
                    pixels[out] = pal[e];
                    pixels[out + 1] = pal[e + 1];
                    pixels[out + 2] = pal[e + 2];
                    pixels[out + 3] = if alpha.is_some() {
                        next_alpha(&mut alpha)
                    } else if one_bit {
                        let c = key_color(fmt, pal[e + 2], pal[e + 1], pal[e]).unwrap_or(0);
                        if c == color_key {
                            0
                        } else {
                            0xff
                        }
                    } else {
                        0xff
                    };
                    src += 1;
                    out += 4;
                }
                src -= row_back;
                if let Some(a) = alpha.as_mut() {
                    *a -= gray_back;
                }
            }
        }
        16 => {
            for _ in 0..h {
                for x in 0..w as isize {
                    let at = (src + x * 2) as usize;
                    let c = u16::from_le_bytes([bits[at], bits[at + 1]]);
                    pixels[out] = (c << 3) as u8;
                    pixels[out + 1] = ((c >> 2) as u8) & 0xf8;
                    pixels[out + 2] = ((c >> 7) as u8) & 0xf8;
                    pixels[out + 3] = if source_alpha {
                        if c & 0x8000 != 0 {
                            0xff
                        } else {
                            0
                        }
                    } else if alpha.is_some() {
                        next_alpha(&mut alpha)
                    } else if one_bit {
                        if u32::from(c) == color_key {
                            0
                        } else {
                            0xff
                        }
                    } else {
                        0xff
                    };
                    out += 4;
                }
                src += (w * 2) as isize - row_back;
                if let Some(a) = alpha.as_mut() {
                    *a -= gray_back;
                }
            }
        }
        24 | 32 => {
            let px = if image.bit_count == 24 { 3 } else { 4 };
            for _ in 0..h {
                for _ in 0..w {
                    let s = src as usize;
                    pixels[out] = bits[s];
                    pixels[out + 1] = bits[s + 1];
                    pixels[out + 2] = bits[s + 2];
                    pixels[out + 3] = if px == 4 && source_alpha {
                        bits[s + 3]
                    } else if alpha.is_some() {
                        next_alpha(&mut alpha)
                    } else if one_bit {
                        // (The blue byte against the key.)
                        if u32::from(bits[s]) == color_key {
                            0
                        } else {
                            0xff
                        }
                    } else {
                        0xff
                    };
                    src += px;
                    out += 4;
                }
                src -= row_back;
                if let Some(a) = alpha.as_mut() {
                    *a -= gray_back;
                }
            }
        }
        _ => {}
    }
    pixels
}

/// IGraph::LoadTexture + ITexture::Create for one texture: the cache file it writes.
pub fn build_cache(
    load: &TextureLoad,
    device: &Device,
    files: &dyn Files,
    encoder: &mut Encoder,
    options: BuildOptions,
) -> CacheResult {
    let mut res = CacheResult::default();
    macro_rules! fail {
        ($outcome:expr, $msg:expr) => {{
            res.outcome = $outcome;
            res.message = $msg;
            return res;
        }};
    }

    // ---- IGraph::LoadTexture (0x10069f70)
    let mut flags = load.flags;
    let name = &load.name;
    if name.ends_with(['s', 'S']) {
        flags &= !ITC_DDS_IGNORED;
    }
    if flags & ITC_DEFAULT != 0 {
        fail!(Outcome::NoCache, "IGraph's default texture".into());
    }
    if flags & ITC_FROM_MEMORY != 0 {
        fail!(Outcome::NoCache, "a texture from memory".into());
    }
    if !load.alpha_name.is_empty() {
        flags |= ITC_USE_ALPHA;
    }
    if name.is_empty() {
        fail!(Outcome::Error, "no file name".into());
    }
    let path = files.find_texture(name).unwrap_or_else(|| format!("Maps\\{name}"));
    let alpha_path = if load.alpha_name.is_empty() {
        String::new()
    } else {
        files.find_texture(&load.alpha_name).unwrap_or_else(|| format!("Maps\\{}", load.alpha_name))
    };
    // Create gets the opacity map only with ITC_USE_ALPHA.
    let create_alpha = if flags & ITC_USE_ALPHA != 0 { alpha_path } else { String::new() };
    res.image_path = path.clone();
    res.alpha_path = create_alpha.clone();

    // ---- ITexture::Create
    if flags & ITC_STREAMED != 0 {
        fail!(Outcome::NoCache, "streamed texture (loaded by the texture thread)".into());
    }
    if let Some(dot) = path.rfind('.') {
        if matches!(path[dot + 1..].chars().next(), Some('d' | 'D')) {
            fail!(Outcome::NoCache, ".dds file (loaded in its own format)".into());
        }
    }
    let Some(image_file) = files.locate(&path) else {
        fail!(Outcome::Error, format!("file not found: {path}"));
    };
    res.image_file = Some(image_file.clone());
    let info = match read_image_info(&image_file) {
        Ok(i) => i,
        Err(e) => fail!(Outcome::Error, format!("{path}: {e}")),
    };
    if flags & ITC_SOURCE_ALPHA != 0 {
        if info.bit_depth == 8 || info.bit_depth == 24 {
            flags &= !ITC_SOURCE_ALPHA;
        }
        if info.bit_depth == 16 {
            flags |= ITC_ALPHA_1BIT;
        }
    }
    let (mut width, mut height) = (info.width, info.height);
    if flags & ITC_NORESIZE == 0 {
        (width, height) = fit_texture_size(width, height, flags, device);
    }
    let image = match load_image(&image_file, 0, 0, flags, &mut res.warnings) {
        Ok(i) => i,
        Err(e) => fail!(Outcome::Error, format!("{path}: {e}")),
    };
    if !matches!(image.bit_count, 8 | 16 | 24 | 32) {
        fail!(Outcome::Error, format!("{path}: {}-bit image (only 8, 16, 24, 32)", image.bit_count));
    }

    // ---- the opacity map, as 8-bit gray (luminance 77 R + 154 G + 25 B, / 256)
    let mut gray = Vec::new();
    let (mut gray_w, mut gray_h) = (0, 0);
    let mut alpha_file_name = String::new();
    let mut alpha_time = 0u64;
    if flags & ITC_USE_ALPHA != 0 {
        if !create_alpha.is_empty() {
            alpha_file_name = base_name(&create_alpha).to_string();
            let Some(alpha_file) = files.locate(&create_alpha) else {
                fail!(Outcome::Error, format!("opacity map not found: {create_alpha}"));
            };
            res.alpha_file = Some(alpha_file.clone());
            let alpha = match load_image(&alpha_file, image.width, image.height, flags, &mut res.warnings) {
                Ok(a) => a,
                Err(e) => fail!(Outcome::Error, format!("{create_alpha}: {e}")),
            };
            alpha_time = alpha.file_time;
            let luma = |r: u8, g: u8, b: u8| (((u32::from(r) + u32::from(g) * 2) * 0x4d + u32::from(b) * 0x19) >> 8) as u8;
            gray_w = alpha.width;
            gray_h = alpha.height;
            let count = alpha.width as usize * alpha.height as usize;
            // (Read without the rows' padding, as in the original.)
            gray = match alpha.bit_count >> 3 {
                1 => (0..count)
                    .map(|i| {
                        let e = alpha.bits[i] as usize * 4;
                        luma(alpha.palette[e + 2], alpha.palette[e + 1], alpha.palette[e])
                    })
                    .collect(),
                3 => (0..count).map(|i| luma(alpha.bits[i * 3 + 2], alpha.bits[i * 3 + 1], alpha.bits[i * 3])).collect(),
                _ => fail!(
                    Outcome::Error,
                    format!("{create_alpha}: {}-bit opacity map (only 8 and 24 bits)", alpha.bit_count)
                ),
            };
        } else {
            flags &= !ITC_USE_ALPHA;
        }
    }

    // ---- the texture format; kind 0 uncompressed, 1 DXT1, 2 DXT1 with 1-bit alpha, 3 DXT3
    let fmt_dxt1 = if device.dxt { Format::Dxt1 } else { Format::Unknown };
    let fmt_dxt3 = if device.dxt { Format::Dxt3 } else { Format::Unknown };
    let (fmt_1bit, fmt_alpha) = (Format::A1R5G5B5, Format::A4R4G4B4);
    let mut staging = Format::Unknown;
    let mut kind = 0;
    let alpha_format = |staging: &mut Format, kind: &mut i32| {
        if flags & ITC_COMPRESS != 0 && fmt_dxt3 != Format::Unknown {
            *staging = fmt_alpha;
            *kind = 3;
            fmt_dxt3
        } else {
            fmt_alpha
        }
    };
    let fmt = if flags & ITC_TRUECOLOR != 0 {
        Format::A8R8G8B8
    } else {
        let one_bit = flags & ITC_ALPHA_1BIT != 0;
        if flags & (ITC_SOURCE_ALPHA | ITC_USE_ALPHA) != 0 && !one_bit {
            alpha_format(&mut staging, &mut kind)
        } else if one_bit {
            if flags & ITC_MIPMAP != 0 {
                alpha_format(&mut staging, &mut kind)
            } else if flags & ITC_COMPRESS != 0 && fmt_dxt1 != Format::Unknown {
                staging = fmt_1bit;
                kind = 2;
                fmt_dxt1
            } else {
                fmt_1bit
            }
        } else if flags & ITC_COMPRESS != 0 && fmt_dxt1 != Format::Unknown {
            staging = fmt_1bit;
            kind = 1;
            fmt_dxt1
        } else {
            device.fmt16
        }
    };
    let levels = if flags & ITC_MIPMAP != 0 { full_chain(width, height) } else { 1 };
    res.flags = flags;
    res.format = fmt;
    res.kind = kind;
    res.width = width;
    res.height = height;
    res.levels = levels;
    let sixteen = matches!(fmt, Format::R5G6B5 | Format::X1R5G5B5 | Format::A1R5G5B5);
    if kind == 0 && !sixteen {
        fail!(Outcome::NoCache, format!("{} texture (only DXT and 16-bit RGB textures are cached)", fmt.name()));
    }
    if kind >= 1 && (width & 3 != 0 || height & 3 != 0) {
        fail!(Outcome::Error, format!("DXT texture of {width}x{height} (ITC_NORESIZE): Direct3D refuses it"));
    }
    res.cache_path = cache_file_name(&path, &alpha_file_name, kind, levels, flags & ITC_HALF != 0, fmt);
    res.outcome = Outcome::Cache;
    if !options.contents {
        return res;
    }

    // ---- the image as 32-bit BGRA
    let mut pixels = build_pixels(&image, &gray, gray_w, gray_h, flags, fmt);
    // Scaled to the texture size (box filter).
    let (w, h) = (image.width, image.height);
    if i32::from(width) != w || i32::from(height) != h {
        let mut resized = vec![0u8; width as usize * height as usize * 4];
        resize_image(w, h, w * 4, &pixels, i32::from(width), i32::from(height), &mut resized);
        pixels = resized;
    }

    // ---- the levels: converted (and compressed through a staging texture), mip maps filtered
    let mut data = Vec::new();
    let mut row_bytes = i32::from(width) * 4;
    let (mut level_w, mut level_h) = (i32::from(width), i32::from(height));
    let (mut cur, mut next) = (Vec::new(), Vec::new());
    if levels > 1 {
        cur = pixels.clone();
        // (Uninitialised in the original until written; the levels it does not compute.)
        next = vec![0u8; width as usize * height as usize * 2];
    }
    let dither = flags & ITC_DITHER != 0;
    for level in 0..levels {
        if level > 0 {
            downsample(&cur, level_w, level_h, row_bytes, &mut next);
            level_w = (level_w >> 1).max(1);
            level_h = (level_h >> 1).max(1);
            row_bytes >>= 1;
        }
        let img: &[u8] = if level > 0 { &next } else { &pixels };
        let (lw, lh) = (level_w as u32, level_h as u32);
        if staging != Format::Unknown {
            let stage = write_surface16(img, staging, lw, lh, dither, 0);
            compress(encoder, kind, &stage, lw, lh, &mut data);
        } else {
            let surface = write_surface16(img, fmt, lw, lh, dither, kind);
            // Rows at the surface's pitch, the padding left 0.
            let pitch = pitch16(lw) as usize;
            let at = data.len();
            data.resize(at + pitch * lh as usize, 0);
            for y in 0..lh as usize {
                for x in 0..lw as usize {
                    let v = surface[y * lw as usize + x].to_le_bytes();
                    data[at + y * pitch + x * 2] = v[0];
                    data[at + y * pitch + x * 2 + 1] = v[1];
                }
            }
        }
        if level > 0 {
            std::mem::swap(&mut cur, &mut next);
        }
    }

    // ---- the cache file (the end of Create)
    let file = &mut res.bytes;
    file.extend_from_slice(if kind >= 1 { b"DXT\0" } else { b"TEX\0" });
    file.extend_from_slice(&(if kind >= 1 { DXT_CACHE_VERSION } else { TEX_CACHE_VERSION }).to_le_bytes());
    file.extend_from_slice(&u32::from(width).to_le_bytes());
    file.extend_from_slice(&u32::from(height).to_le_bytes());
    file.extend_from_slice(&image.file_time.to_le_bytes());
    if fmt == Format::Dxt3 {
        file.extend_from_slice(&alpha_time.to_le_bytes());
    }
    file.extend_from_slice(&data);
    res
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn names() {
        assert_eq!(cache_file_name("Maps\\A.BMP", "", 1, 8, true, Format::Dxt1), "Maps\\A_8L.DX1");
        assert_eq!(cache_file_name("Maps\\A.BMP", "B.BMP", 3, 1, false, Format::Dxt3), "Maps\\A_B_1.DX3");
        assert_eq!(cache_file_name("Maps\\A.BMP", "", 0, 1, false, Format::R5G6B5), "Maps\\A_1.565");
        assert_eq!(cache_file_name("Maps\\A.BMP", "", 0, 1, false, Format::A1R5G5B5), "Maps\\A_1.555");
    }

    #[test]
    fn fit() {
        let d = Device::default();
        assert_eq!(fit_texture_size(100, 60, 0, &d), (128, 64));
        assert_eq!(fit_texture_size(256, 256, ITC_HALF, &d), (128, 128));
        assert_eq!(fit_texture_size(2, 2, 0, &d), (4, 4));
        assert_eq!(full_chain(128, 64), 8);
    }
}
