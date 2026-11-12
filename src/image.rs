//! .bmp / .tga files as LS3DF.dll reads them for ITexture::Create (IGraph::GetImageInfo
//! 0x1006fe00, LoadImage 0x100775f0), from files on disk instead of the game's .dta archives.
//!
//! The reader trusts the headers the way the original does: it does not seek to the BMP's pixel
//! offset, reads TGA files as uncompressed and takes 256 colour map entries for any TGA of 8 bits
//! or less. Such files load wrong in the game too; the warnings say so.

use std::path::Path;
use std::time::UNIX_EPOCH;

/// LoadImage's flags argument: only this ITC_ bit changes how a file is read.
pub const BMP_UNPADDED: u32 = 0x0004_0000;

#[derive(Default, Clone, Copy)]
pub struct ImageInfo {
    pub width: u16,
    pub height: u16,
    pub bit_depth: u8,
}

/// What LoadImage leaves in its BITMAPINFOHEADER and buffers.
#[derive(Default)]
pub struct Image {
    pub width: i32,
    pub height: i32,
    pub bit_count: u16,
    /// The row pitch in bytes (biXPelsPerMeter).
    pub pitch: i32,
    /// Rows bottom-up, `pitch` apart (unpadded after a resize).
    pub bits: Vec<u8>,
    /// 256 BGRA entries (0x400 bytes) when the image has a palette.
    pub palette: Vec<u8>,
    /// The file's last-write time as a FILETIME (dtaGetTime).
    pub file_time: u64,
}

/// The last-write time of a file as a FILETIME (100 ns since 1601); 0 if unknown.
pub fn file_time_of(path: &Path) -> u64 {
    const EPOCH_DIFF: u64 = 116_444_736_000_000_000;
    let Ok(m) = std::fs::metadata(path).and_then(|m| m.modified()) else {
        return 0;
    };
    match m.duration_since(UNIX_EPOCH) {
        Ok(d) => EPOCH_DIFF + d.as_secs() * 10_000_000 + u64::from(d.subsec_nanos()) / 100,
        Err(e) => {
            let d = e.duration();
            EPOCH_DIFF.saturating_sub(d.as_secs() * 10_000_000 + u64::from(d.subsec_nanos()) / 100)
        }
    }
}

/// A whole file read in order like a dta file handle.
struct Reader {
    data: Vec<u8>,
    pos: usize,
}

impl Reader {
    /// dtaRead: as many bytes as there are, up to `out.len()`; returns the count.
    fn read(&mut self, out: &mut [u8]) -> usize {
        let avail = self.data.len().saturating_sub(self.pos);
        let n = out.len().min(avail);
        out[..n].copy_from_slice(&self.data[self.pos..self.pos + n]);
        self.pos += out.len();
        n
    }
    fn skip(&mut self, n: usize) {
        self.pos += n;
    }
}

/// .tga by the first letter of the extension, .bmp by 'b'; None for anything else.
fn is_tga(path: &Path) -> Option<bool> {
    let s = path.to_string_lossy();
    let dot = s.rfind('.')?;
    match s[dot + 1..].chars().next()? {
        't' | 'T' => Some(true),
        'b' | 'B' => Some(false),
        _ => None,
    }
}

fn u16_at(b: &[u8], o: usize) -> u16 {
    u16::from_le_bytes([b[o], b[o + 1]])
}
fn u32_at(b: &[u8], o: usize) -> u32 {
    u32::from_le_bytes([b[o], b[o + 1], b[o + 2], b[o + 3]])
}

/// The BITMAPINFOHEADER fields the reader uses.
#[derive(Default)]
struct Info {
    size: u32,
    width: i32,
    height: i32,
    bit_count: u16,
    compression: u32,
    pitch: i32,
    clr_used: u32,
}

fn parse_info(b: &[u8; 40]) -> Info {
    Info {
        size: u32_at(b, 0),
        width: u32_at(b, 4) as i32,
        height: u32_at(b, 8) as i32,
        bit_count: u16_at(b, 14),
        compression: u32_at(b, 16),
        pitch: u32_at(b, 24) as i32,
        clr_used: u32_at(b, 32),
    }
}

/// IGraph::GetImageInfo: the size and bit depth of a .bmp / .tga file.
pub fn read_image_info(path: &Path) -> Result<ImageInfo, String> {
    let tga = is_tga(path).ok_or("not a .bmp or .tga file")?;
    let data = std::fs::read(path).map_err(|_| "cannot read the file".to_string())?;
    let mut r = Reader { data, pos: 0 };
    if tga {
        let mut h = [0u8; 0x12];
        if r.read(&mut h) == 0 {
            return Err("empty file".into());
        }
        Ok(ImageInfo { width: u16_at(&h, 0xc), height: u16_at(&h, 0xe), bit_depth: h[0x10] })
    } else {
        let mut fh = [0u8; 14];
        let mut ih = [0u8; 40];
        if r.read(&mut fh) == 0 || r.read(&mut ih) == 0 {
            return Err("empty file".into());
        }
        let info = parse_info(&ih);
        Ok(ImageInfo { width: info.width as u16, height: info.height as u16, bit_depth: info.bit_count as u8 })
    }
}

/// LoadImage: the image's rows, palette and header; resized to width x height (nearest pixel)
/// when both are given and differ. `warnings` gets notes about files the game reads differently
/// from an image viewer.
pub fn load_image(
    path: &Path,
    width: i32,
    height: i32,
    flags: u32,
    warnings: &mut Vec<String>,
) -> Result<Image, String> {
    let tga = is_tga(path).ok_or("not a .bmp or .tga file")?;
    let data = std::fs::read(path).map_err(|_| "file not found".to_string())?;
    let mut image = Image { file_time: file_time_of(path), ..Default::default() };
    let mut r = Reader { data, pos: 0 };
    let shown = path.display().to_string();
    let mut warn = |text: String| warnings.push(format!("{shown}: {text}"));

    let mut info = Info::default();
    let mut top_down = false;
    if tga {
        let mut h = [0u8; 0x12];
        if r.read(&mut h) == 0 {
            return Err("empty file".into());
        }
        if (h[0] as i8) > 0 {
            r.skip(h[0] as usize); // the image id
        }
        if h[0x11] & 0x10 != 0 {
            warn("right-to-left TGA: the game does not mirror it".into());
        }
        if !matches!(h[2], 1..=3) {
            warn(format!("TGA image type {} (RLE?): the game reads the data as uncompressed", h[2]));
        }
        top_down = h[0x11] & 0x20 != 0;
        let w = u16_at(&h, 0xc) as i16 as i32;
        let ht = u16_at(&h, 0xe) as i16 as i32;
        let bpp = h[0x10] as i8 as i32;
        if bpp <= 8 && (h[1] != 1 || h[7] != 24) {
            warn("8-bit TGA without a 24-bit colour map: the game reads 768 bytes as the palette anyway".into());
        }
        info.size = 0x28;
        info.width = w;
        info.height = ht;
        info.bit_count = bpp as u16;
        info.pitch = (bpp / 8) * w;
    } else {
        let mut fh = [0u8; 14];
        let mut ih = [0u8; 40];
        if r.read(&mut fh) == 0 || r.read(&mut ih) == 0 {
            return Err("empty file".into());
        }
        info = parse_info(&ih);
        let off_bits = u32_at(&fh, 10);
        if info.size != 40 {
            warn(format!(
                "BMP info header of {} bytes: the game reads the palette and pixels right after the first 40",
                info.size
            ));
        }
        if info.height < 0 {
            return Err("top-down BMP (negative height): the game cannot load it".into());
        }
        let clr = if info.clr_used != 0 {
            info.clr_used
        } else if info.bit_count <= 8 {
            1u32 << info.bit_count
        } else {
            0
        };
        if info.size == 40 && u64::from(off_bits) != 14 + 40 + u64::from(clr) * 4 {
            warn(format!("BMP pixel offset {off_bits} is not right after the palette: the game ignores the offset"));
        }
        if info.compression != 0 {
            return Err(format!("compressed BMP ({}): unsupported by the game", info.compression));
        }
        // BMP rows are padded to 4 bytes.
        let row_bytes = (info.bit_count as i32).wrapping_mul(info.width) / 8;
        let padded = row_bytes / 4 * 4;
        info.pitch = if flags & BMP_UNPADDED == 0 && padded != row_bytes { padded + 4 } else { row_bytes };
    }
    let pitch = info.pitch;

    if info.clr_used == 0 && info.bit_count <= 8 {
        info.clr_used = 1 << info.bit_count;
    }
    if info.clr_used != 0 {
        if info.clr_used > 256 {
            return Err(format!("palette of {} entries (more than 256)", info.clr_used));
        }
        image.palette = vec![0; 0x400];
        let n = info.clr_used as usize;
        if !tga {
            if r.read(&mut image.palette[..n * 4]) != n * 4 {
                return Err("error reading the palette".into());
            }
        } else {
            for i in 0..n {
                if r.read(&mut image.palette[i * 4..i * 4 + 3]) != 3 {
                    return Err("error reading the palette".into());
                }
            }
        }
    }

    if pitch <= 0 || info.height <= 0 {
        return Err("empty image".into());
    }
    let size = pitch as usize * info.height as usize;
    let mut bits = vec![0u8; size];
    if r.read(&mut bits) != size {
        return Err("error reading the bitmap (file too short)".into());
    }
    if top_down {
        let p = pitch as usize;
        let h = info.height as usize;
        for i in 0..h / 2 {
            let (a, b) = bits.split_at_mut((h - 1 - i) * p);
            a[i * p..i * p + p].swap_with_slice(&mut b[..p]);
        }
    }
    image.width = info.width;
    image.height = info.height;
    image.bit_count = info.bit_count;
    if width == 0 || height == 0 || (width == info.width && height == info.height) {
        image.pitch = pitch;
        image.bits = bits;
        return Ok(image);
    }
    // Resized to width x height (nearest pixel), rows without padding.
    let bpp = info.bit_count as i32;
    let px = (bpp >> 3) as usize;
    let mut resized = Vec::with_capacity((bpp * width * height >> 3) as usize);
    let step_x = info.width as f32 / width as f32;
    let step_y = info.height as f32 / height as f32;
    for y in 0..height {
        let row = (y as f32 * step_y) as i32 * pitch;
        for x in 0..width {
            let col = (x as f32 * step_x) as i32;
            let at = (row + (col * bpp >> 3)) as usize;
            resized.extend_from_slice(&bits[at..at + px]);
        }
    }
    image.width = width;
    image.height = height;
    image.pitch = bpp * width >> 3;
    image.bits = resized;
    Ok(image)
}
