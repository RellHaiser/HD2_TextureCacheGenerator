//! The texture loads of a .4ds model (LS3D version 0x29): its material table read the way
//! LS3DF.dll's loader does (ReadModelFile 0x1005d970, I3D_material::Load,
//! I3D_anim_material::Load), with the ITC_ flags of I3D_material::TextureFlags for given driver
//! settings.

use std::path::Path;

use crate::texture::*;

// Material flags (I3D_material::matFlags, as stored in the file).
const MATF_ENV_TEXTURE: u32 = 0x0008_0000;
const MATF_ANIMATED: u32 = 0x0400_0000;
/// Animated: the opacity map has frames too.
const MATF_ALPHA_FRAMES: u32 = 0x0200_0000;
const MATF_COLORKEY: u32 = 0x2000_0000;
const MATF_ALPHA: u32 = 0x4000_0000;
/// Additive, colour key or own alpha: no opacity map name.
const MATF_NO_ALPHA_NAME: u32 = 0xa100_0000;

/// The I3D driver states that pick the texture options (DRVSTATE_TEX_*). HD2 sets mip maps on,
/// leaves dithering at the driver's default (on) and takes compression, 32-bit textures and half
/// size from its setup (C_App 0x006dbd70).
#[derive(Clone, Copy)]
pub struct DriverStates {
    pub mipmap: bool,
    pub dither: bool,
    pub true_color: bool,
    pub half: bool,
    pub compress: bool,
}

impl Default for DriverStates {
    fn default() -> Self {
        DriverStates { mipmap: true, dither: true, true_color: false, half: false, compress: true }
    }
}

/// I3D_material::TextureFlags: a material's (file) flags to ITC_ flags.
pub fn material_texture_flags(flags: u32, st: &DriverStates) -> u32 {
    let mut tf = flags >> 5 & ITC_STREAMED;
    if st.dither {
        tf |= ITC_DITHER;
    }
    if st.true_color || flags & 0x10_0000 != 0 {
        tf |= ITC_TRUECOLOR;
    } else if st.compress && flags & 0x20_0000 == 0 {
        tf |= ITC_COMPRESS;
    }
    if flags & 0x80_0000 != 0 && st.mipmap {
        tf |= ITC_MIPMAP;
    }
    if st.half && flags & 0x40_0000 == 0 {
        tf |= ITC_HALF;
    }
    if flags & 0x8000 != 0 {
        tf |= ITC_KEEP_PIXELS;
    }
    if flags & MATF_COLORKEY != 0 {
        tf |= if flags & 0x100_0000 != 0 { ITC_ALPHA_1BIT | ITC_SOURCE_ALPHA } else { ITC_ALPHA_1BIT };
    } else if flags & MATF_ALPHA != 0 {
        tf |= if flags & 0x100_0000 != 0 { ITC_SOURCE_ALPHA } else { ITC_USE_ALPHA };
    }
    tf
}

/// The flags of a material's environment (second) texture.
pub fn environment_texture_flags(flags: u32, st: &DriverStates) -> u32 {
    (if st.mipmap { ITC_MIPMAP } else { 0 }) | (material_texture_flags(flags, st) & 0xffff_ef31) | 0x8_0000
}

pub struct MaterialLoad {
    /// Index in the model's table.
    pub material: usize,
    /// The material's second texture.
    pub environment: bool,
    pub load: TextureLoad,
}

struct Reader<'a> {
    data: &'a [u8],
    pos: usize,
    ok: bool,
}

impl Reader<'_> {
    fn bytes(&mut self, n: usize) -> &[u8] {
        if self.pos + n > self.data.len() {
            self.ok = false;
            self.pos = self.data.len();
            return &[];
        }
        let s = &self.data[self.pos..self.pos + n];
        self.pos += n;
        s
    }
    fn u8(&mut self) -> u8 {
        self.bytes(1).first().copied().unwrap_or(0)
    }
    fn u16(&mut self) -> u16 {
        let b = self.bytes(2);
        if b.len() == 2 {
            u16::from_le_bytes([b[0], b[1]])
        } else {
            0
        }
    }
    fn u32(&mut self) -> u32 {
        let b = self.bytes(4);
        if b.len() == 4 {
            u32::from_le_bytes([b[0], b[1], b[2], b[3]])
        } else {
            0
        }
    }
    fn skip(&mut self, n: usize) {
        self.bytes(n);
    }
    /// ReadName: a length byte and that many characters (Windows-1252, kept byte for byte).
    fn name(&mut self) -> String {
        let n = self.u8() as usize;
        self.bytes(n).iter().map(|&b| b as char).collect()
    }
}

/// "name00.ext" ... for an animated material's frames.
fn frame_name(name: &str, frame: usize) -> String {
    let mut s: Vec<char> = name.chars().collect();
    let at = s.len() - 6;
    let digits: Vec<char> = format!("{frame:02}").chars().collect();
    s[at] = digits[0];
    s[at + 1] = digits[1];
    s[at + 2] = '.';
    if digits.len() > 2 {
        s.truncate(at + 3); // sprintf's terminator lands on the extension (frames >= 100)
    }
    s.into_iter().collect()
}

/// The texture loads of a model file's materials.
pub fn read_model_textures(file: &Path, st: &DriverStates) -> Result<Vec<MaterialLoad>, String> {
    let data = std::fs::read(file).map_err(|_| "cannot read".to_string())?;
    let mut r = Reader { data: &data, pos: 0, ok: true };
    let magic = r.bytes(4).to_vec();
    let version = r.u16();
    if !r.ok || magic != b"4DS\0" {
        return Err("not a 4DS file".into());
    }
    if version != 0x29 {
        return Err(format!("4DS version {version} (the game loads 0x29)"));
    }
    r.skip(8);
    let count = r.u16();
    if count > 6000 {
        return Err("more than 6000 materials".into());
    }
    let mut out = Vec::new();
    for i in 0..count as usize {
        if !r.ok {
            break;
        }
        let flags = r.u32();
        let tf = material_texture_flags(flags, st);
        let mut add = |name: String, alpha: String, tex_flags: u32, env: bool| {
            out.push(MaterialLoad {
                material: i,
                environment: env,
                load: TextureLoad { name, alpha_name: alpha, flags: tex_flags },
            });
        };
        let has_env = flags & MATF_ENV_TEXTURE != 0;
        let reads_alpha = flags & MATF_ALPHA != 0 && flags & MATF_NO_ALPHA_NAME == 0;
        r.skip(if has_env { 0x4c } else { 0x48 });
        if flags & MATF_ANIMATED == 0 {
            // I3D_material::Load
            if has_env {
                let env = r.name();
                if !env.is_empty() {
                    add(env, String::new(), environment_texture_flags(flags, st), true);
                }
            }
            let name = r.name();
            if name.is_empty() {
                continue;
            }
            let mut alpha = String::new();
            let mut tex_flags = tf;
            if reads_alpha {
                alpha = r.name();
                if alpha.is_empty() {
                    tex_flags &= !ITC_USE_ALPHA;
                }
            }
            add(name, alpha, tex_flags, false);
        } else {
            // I3D_anim_material::Load
            let env = if has_env { r.name() } else { String::new() };
            let name = r.name();
            let alpha = if !name.is_empty() && reads_alpha { r.name() } else { String::new() };
            let frames = r.u8() as usize;
            let env_frames = r.u8() as usize;
            r.skip(0x10);
            if has_env {
                let env_flags = environment_texture_flags(flags, st);
                if env_frames < 2 {
                    if !env.is_empty() {
                        add(env, String::new(), env_flags, true);
                    }
                } else if env.chars().count() >= 6 {
                    for k in 0..env_frames {
                        add(frame_name(&env, k), String::new(), env_flags, true);
                    }
                }
            }
            if !name.is_empty() || !alpha.is_empty() {
                if frames < 2 {
                    add(name, alpha, tf, false);
                } else if name.chars().count() >= 6 {
                    let alpha_frames =
                        flags & MATF_ALPHA_FRAMES != 0 && flags & MATF_NO_ALPHA_NAME == 0 && alpha.chars().count() >= 6;
                    for k in 0..frames {
                        let a = if alpha_frames { frame_name(&alpha, k) } else { alpha.clone() };
                        add(frame_name(&name, k), a, tf, false);
                    }
                }
            }
        }
    }
    if !r.ok {
        return Err("file ends inside the material table".into());
    }
    Ok(out)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn frames() {
        assert_eq!(frame_name("WATER00.BMP", 7), "WATER07.BMP");
        assert_eq!(frame_name("WATER00.BMP", 123), "WATER12.");
    }
}
