//! The S3TC (DXT) compressor linked into LS3DF.dll (S3 Inc.'s S3TC library, 0x10080ed0 -
//! 0x10086500), as far as ITexture uses it: 16-bit sources, opaque / 1-bit alpha DXT1 and explicit
//! alpha DXT3. Translated from the LS3DF port's `src/igraph/s3tc.cpp`.
//!
//! The library works in doubles, but the game runs the x87 FPU in Direct3D's single precision:
//! every operation's result is rounded to a 24-bit mantissa. `r()` reproduces that (the exact
//! result rounded to float, computed in double: exact for products of floats, correctly rounded
//! for sums, quotients and square roots of floats since 53 >= 2 * 24 + 2). Every expression keeps
//! the original's operation order. `lt` / `le` / `eq` are the x87 compare outcomes (unordered
//! compares count as less and equal).
//!
//! The original keeps its work arrays in globals; a few carry over from one block (and one
//! texture) to the next: the best ratios of the two cluster fits and the single-colour table.
//! Here they are the fields of an [`Encoder`].

/// Encode types: the low byte selects the colour blocks' transparency, the high byte an alpha
/// block.
pub const S3TC_OPAQUE: u32 = 0x000;
/// DXT1, pixels at or below the alpha threshold (0) transparent.
pub const S3TC_ALPHA_KEY: u32 = 0x002;
/// DXT3: 4 bits of alpha per pixel.
pub const S3TC_ALPHA_EXPLICIT: u32 = 0x100;

/// A 16-bit source surface: its size, pitch and the red, green, blue, alpha masks.
pub struct Source<'a> {
    pub width: u32,
    pub height: u32,
    pub pitch: usize,
    pub pixels: &'a [u8],
    pub masks: [u32; 4],
}

#[inline]
fn r(x: f64) -> f64 {
    // The mantissa rounded to 24 bits (nearest, ties to even), the double's exponent kept: x87
    // registers keep their wide exponent at 24-bit precision.
    let mut u = x.to_bits();
    if (u >> 52) & 0x7ff == 0x7ff {
        return x;
    }
    u = u.wrapping_add(((1u64 << 28) - 1) + ((u >> 29) & 1));
    u &= !((1u64 << 29) - 1);
    f64::from_bits(u)
}
#[inline]
fn lt(a: f64, b: f64) -> bool {
    !(a >= b)
}
#[inline]
fn le(a: f64, b: f64) -> bool {
    !(a > b)
}
#[inline]
fn eq(a: f64, b: f64) -> bool {
    !(a < b) && !(a > b)
}

/// MSVC 6 `__ftol`: truncation with a 64-bit FISTP, the low 32 bits kept; NaN and values outside
/// the 64-bit range give 0.
#[inline]
fn ftol(v: f64) -> i32 {
    if !(v > -9223372036854775808.0 && v < 9223372036854775808.0) {
        return 0;
    }
    (v as i64) as i32
}

const INV_255: f64 = 0.00392156862745098; // 0x100a1c98
const BITS: [i32; 3] = [5, 6, 5];
/// The three colours a block can produce from two endpoints a, b: (a * m0 + b * m1 + m2) / m3:
/// a itself, the 3-colour midpoint, the 4-colour third.
const MIX: [[i32; 4]; 3] = [[1, 0, 0, 1], [1, 1, 0, 2], [1, 2, 1, 3]];
/// Endpoint extensions tried by final_quantize: count per (variant, mode) and the coefficient
/// pairs (a, b) of new = a * e0 + b * e1, indexed ((variant * 2 + mode) * 6 + slot) * 2 + endpoint.
const VARIANT_COUNT: [usize; 6] = [2, 0, 0, 6, 2, 0];
const XFORM: [[f64; 2]; 48] = [
    [2.0, -1.0], [0.0, 1.0], [1.0, 0.0], [-1.0, 2.0], [0.0, 0.0], [0.0, 0.0], [0.0, 0.0], [0.0, 0.0],
    [0.0, 0.0], [0.0, 0.0], [0.0, 0.0], [0.0, 0.0], [0.0, 0.0], [0.0, 0.0], [0.0, 0.0], [0.0, 0.0],
    [0.0, 0.0], [0.0, 0.0], [0.0, 0.0], [0.0, 0.0], [0.0, 0.0], [0.0, 0.0], [0.0, 0.0], [0.0, 0.0],
    [1.0, 0.0], [0.0, 1.0], [1.0, 0.0], [-2.0, 3.0], [3.0, -2.0], [0.0, 1.0],
    [2.0, -1.0], [-1.0, 2.0], [1.5, -0.5], [0.0, 1.0], [1.0, 0.0], [-0.5, 1.5],
    [1.5, -0.5], [0.0, 1.0], [1.0, 0.0], [-0.5, 1.5], [0.0, 0.0], [0.0, 0.0], [0.0, 0.0], [0.0, 0.0],
    [0.0, 0.0], [0.0, 0.0], [0.0, 0.0], [0.0, 0.0],
];

type V3 = [f64; 3];

/// The colour block handed to encode_block.
struct Block {
    count: usize,
    colors: [V3; 16],
    weights: V3,
    mode: i32,
    result_mode: i32,
    endpoints: [[i32; 3]; 2],
    indices: [i32; 16],
}

/// The orderings of encode_block's axis passes (a ring of 16).
struct Orders {
    orderings: [[usize; 16]; 16],
    slot: usize,
    wrapped: bool,
    found: bool,
}

/// The library's state (its globals, as far as the colour blocks use them).
pub struct Encoder {
    unique: [V3; 16],
    inv_count: f64,
    lo: V3,
    hi: V3,
    kind_err: V3,
    quant_tmp: f64,
    max_dist2: f64,
    cov2: [f64; 9],
    ord_sum: [V3; 16],
    fit_c: V3,
    xform_ep: [V3; 2],
    best_quant_err: f64,
    proj: [f64; 16],
    scaled: [V3; 16],
    unique_sum: [V3; 16],
    bfit_s: [V3; 3],
    err_tab: [f64; 3 * 4 * 64],
    ord_w2: [f64; 16],
    cand_err: [f64; 2],
    ord_sum2: [V3; 16],
    inv_norm: f64,
    mean: V3,
    /// [value][channel][kind]: valid, endpoint 0, endpoint 1.
    single: [[[[u8; 3]; 3]; 3]; 256],
    pass_ep: [[V3; 2]; 2],
    centered: [V3; 16],
    palette: [[[f64; 4]; 4]; 3],
    unique_weight: [f64; 16],
    quant_err: [f64; 2],
    trace: f64,
    bfit_c: V3,
    fit_q: V3,
    axis: V3,
    bfit_q: V3,
    pass_err: [f64; 2],
    dist2: f64,
    ord_w: [f64; 16],
    weights: V3,
    cov: [f64; 9],
    fit_s: [V3; 3],
    fit_best_num: f64,
    bfit_best_num: f64,
    single_ready: bool,
    // Initialised data of the original (0x100a99a0, 0x100a99a8).
    fit_best_den: f64,
    bfit_best_den: f64,
}

impl Default for Encoder {
    fn default() -> Self {
        Self::new()
    }
}

impl Encoder {
    /// The state of a freshly loaded LS3DF.dll.
    pub fn new() -> Self {
        Encoder {
            unique: [[0.0; 3]; 16],
            inv_count: 0.0,
            lo: [0.0; 3],
            hi: [0.0; 3],
            kind_err: [0.0; 3],
            quant_tmp: 0.0,
            max_dist2: 0.0,
            cov2: [0.0; 9],
            ord_sum: [[0.0; 3]; 16],
            fit_c: [0.0; 3],
            xform_ep: [[0.0; 3]; 2],
            best_quant_err: 0.0,
            proj: [0.0; 16],
            scaled: [[0.0; 3]; 16],
            unique_sum: [[0.0; 3]; 16],
            bfit_s: [[0.0; 3]; 3],
            err_tab: [0.0; 3 * 4 * 64],
            ord_w2: [0.0; 16],
            cand_err: [0.0; 2],
            ord_sum2: [[0.0; 3]; 16],
            inv_norm: 0.0,
            mean: [0.0; 3],
            single: [[[[0; 3]; 3]; 3]; 256],
            pass_ep: [[[0.0; 3]; 2]; 2],
            centered: [[0.0; 3]; 16],
            palette: [[[0.0; 4]; 4]; 3],
            unique_weight: [0.0; 16],
            quant_err: [0.0; 2],
            trace: 0.0,
            bfit_c: [0.0; 3],
            fit_q: [0.0; 3],
            axis: [0.0; 3],
            bfit_q: [0.0; 3],
            pass_err: [0.0; 2],
            dist2: 0.0,
            ord_w: [0.0; 16],
            weights: [0.0; 3],
            cov: [0.0; 9],
            fit_s: [[0.0; 3]; 3],
            fit_best_num: 0.0,
            bfit_best_num: 0.0,
            single_ready: false,
            fit_best_den: 1.0,
            bfit_best_den: 1.0,
        }
    }

    // ---------------------------------------------------------------- single colours

    /// For every 8-bit value of each channel and colour kind, the endpoint pair closest together
    /// that produces it (built on first use).
    fn build_single_color_table(&mut self) {
        for ch in 0..3 {
            let bits = BITS[ch];
            let max = (1 << bits) - 1;
            for a in (0..=max).rev() {
                let ea = (a >> (2 * bits - 8)) | (a << (8 - bits));
                for b in (0..=max).rev() {
                    let eb = (b >> (2 * bits - 8)) | (b << (8 - bits));
                    for k in 0..3 {
                        let v = ((MIX[k][1] * eb + ea * MIX[k][0] + MIX[k][2]) / MIX[k][3]) as usize;
                        let e = &mut self.single[v][ch][k];
                        if e[0] == 0 || (e[2] as i32 - e[1] as i32).abs() > (eb - ea).abs() {
                            e[0] = 1;
                            e[1] = (a << (8 - bits)) as u8;
                            e[2] = (b << (8 - bits)) as u8;
                        }
                    }
                }
            }
        }
    }

    /// A block of one colour (0x10083d40): the best endpoints per kind from the table.
    fn encode_single_color(
        &mut self,
        mode: &mut i32,
        n: usize,
        points: &[V3; 16],
        weights: &V3,
        endpoints: &mut [[i32; 3]; 2],
        indices: &mut [i32; 16],
    ) -> f64 {
        if !self.single_ready {
            self.single_ready = true;
            self.build_single_color_table();
        }
        let mut choice = [[0i32; 3]; 3];
        let mut kind = 0usize;
        while (kind as i32) < *mode - 1 {
            self.kind_err[kind] = 0.0;
            for ch in 0..3 {
                let mut v = ftol(r(r(r(points[0][ch] * 255.0) / weights[ch]) + 0.5).floor());
                if v < 0 {
                    v = 0;
                } else if v >= 256 {
                    v = 255;
                }
                let valid = self.single[v as usize][ch][kind][0] != 0;
                let mut cand = [0i32; 2];
                let mut dir: i32 = 1;
                for side in 0..2 {
                    let mut c = v;
                    if !valid
                        || !le(r(r(r(r(points[0][ch] * 255.0) / weights[ch]) - v as f64) * dir as f64), 0.0)
                    {
                        // The nearest valid value on this side.
                        c = v + dir;
                        if c < 0 {
                            c = 0;
                        } else if c >= 256 {
                            c = 255;
                        }
                        while self.single[c as usize][ch][kind][0] == 0 && c + dir >= 0 && c + dir < 256 {
                            c += dir;
                        }
                    }
                    self.cand_err[side] = 0.0;
                    for p in points.iter().take(n) {
                        let d = r(r(c as f64 * weights[ch]) - r(p[ch] * 255.0));
                        self.cand_err[side] = r(r(d * d) + self.cand_err[side]);
                    }
                    cand[side] = c;
                    dir = -dir;
                }
                let e0 = self.cand_err[0];
                let e1 = self.cand_err[1];
                if lt(e0, e1) {
                    self.kind_err[kind] = r(e0 + self.kind_err[kind]);
                    choice[ch][kind] = cand[0];
                } else {
                    self.kind_err[kind] = r(e1 + self.kind_err[kind]);
                    choice[ch][kind] = if le(e0, e1) {
                        if v & 1 != 0 {
                            cand[0]
                        } else {
                            cand[1]
                        }
                    } else {
                        cand[1]
                    };
                }
            }
            kind += 1;
        }

        let mut best = kind;
        if *mode == 4 {
            best = if !le(self.kind_err[0], self.kind_err[1]) {
                if le(self.kind_err[1], self.kind_err[2]) {
                    1
                } else {
                    2
                }
            } else if le(self.kind_err[0], self.kind_err[2]) {
                0
            } else {
                2
            };
        } else if *mode == 3 {
            best = if le(self.kind_err[0], self.kind_err[1]) { 0 } else { 1 };
        }
        for ch in 0..3 {
            let e = self.single[choice[ch][best] as usize][ch][best];
            for k in 0..2 {
                endpoints[k][ch] = (e[1 + k] >> (8 - BITS[ch])) as i32;
            }
        }
        for idx in indices.iter_mut().take(n) {
            *idx = best as i32 + 1;
        }
        if best != 0 {
            *mode = best as i32 + 2;
        }
        self.kind_err[best]
    }

    // ---------------------------------------------------------------- axes and orderings

    /// The principal axis of the centred colours by power iteration on the covariance
    /// (0x10082510).
    fn principal_axis(&mut self, n: usize, p: &[V3; 16]) -> V3 {
        let m = &mut self.cov;
        m[8] = 0.0;
        m[5] = 0.0;
        m[4] = 0.0;
        m[2] = 0.0;
        m[1] = 0.0;
        m[0] = 0.0;
        for pi in p.iter().take(n) {
            let (x, y, z) = (pi[0], pi[1], pi[2]);
            m[0] = r(r(x * x) + m[0]);
            m[1] = r(r(y * x) + m[1]);
            m[2] = r(r(x * z) + m[2]);
            m[4] = r(r(y * y) + m[4]);
            m[5] = r(r(y * z) + m[5]);
            m[8] = r(r(z * z) + m[8]);
        }
        for _ in 0..3 {
            self.trace = r(r(self.cov[0] + self.cov[4]) + self.cov[8]);
            let scale = r(3.5 / self.trace);
            for row in 0..3 {
                for k in row..3 {
                    self.cov[row * 3 + k] = r(scale * self.cov[row * 3 + k]);
                }
            }
            for _ in 0..4 {
                let m = &mut self.cov;
                let s = &mut self.cov2;
                let (a, b, c, d, e, f) = (m[0], m[1], m[2], m[4], m[5], m[8]);
                let (bb, cc, ee) = (r(b * b), r(c * c), r(e * e));
                s[0] = r(r(r(a * a) + cc) + bb);
                s[1] = r(r(r(a + d) * b) + r(c * e));
                s[2] = r(r(r(a + f) * c) + r(b * e));
                s[4] = r(r(r(d * d) + ee) + bb);
                s[5] = r(r(r(d + f) * e) + r(b * c));
                s[8] = r(r(r(f * f) + ee) + cc);
                let (a2, b2, c2, d2, e2, f2) = (s[0], s[1], s[2], s[4], s[5], s[8]);
                let (bb2, cc2, ee2) = (r(b2 * b2), r(c2 * c2), r(e2 * e2));
                m[0] = r(r(r(a2 * a2) + bb2) + cc2);
                m[1] = r(r(r(d2 + a2) * b2) + r(e2 * c2));
                m[2] = r(r(r(f2 + a2) * c2) + r(e2 * b2));
                m[4] = r(r(r(d2 * d2) + ee2) + bb2);
                m[5] = r(r(r(f2 + d2) * e2) + r(c2 * b2));
                m[8] = r(r(r(f2 * f2) + ee2) + cc2);
            }
        }
        let m = &self.cov;
        let u = if le(m[0], m[4]) {
            if le(m[4], m[8]) {
                2
            } else {
                1
            }
        } else if le(m[0], m[8]) {
            2
        } else {
            0
        };
        self.inv_norm = r(1.0 / r(m[u * 4].sqrt()));
        let mut axis = [0.0; 3];
        for (k, a) in axis.iter_mut().enumerate().take(u) {
            *a = r(self.inv_norm * self.cov2[k * 3 + u]);
        }
        for (k, a) in axis.iter_mut().enumerate().skip(u) {
            *a = r(self.inv_norm * self.cov2[u * 3 + k]);
        }
        axis
    }

    /// The direction between the two colours farthest apart (0x10082920).
    fn farthest_pair_axis(&mut self, n: usize, p: &[V3; 16]) -> V3 {
        let (mut bi, mut bj) = (0, 0);
        self.max_dist2 = 0.0;
        for i in 0..n {
            for j in i..n {
                let dx = r(p[i][0] - p[j][0]);
                let dy = r(p[i][1] - p[j][1]);
                let dz = r(p[i][2] - p[j][2]);
                self.dist2 = r(r(r(dz * dz) + r(dy * dy)) + r(dx * dx));
                if !le(self.dist2, self.max_dist2) {
                    self.max_dist2 = self.dist2;
                    bi = i;
                    bj = j;
                }
            }
        }
        self.max_dist2 = r(1.0 / r(self.max_dist2.sqrt()));
        let mut axis = [0.0; 3];
        for c in 0..3 {
            axis[c] = r(r(p[bi][c] - p[bj][c]) * self.max_dist2);
        }
        axis
    }

    /// The colours in ascending order of their projection on the axis (0x10082a30); ties: the
    /// first (tie_mode 0) or the last (1) of them first.
    fn sort_by_projection(&mut self, n: usize, p: &[V3; 16], axis: &V3, order: &mut [usize; 16], tie_mode: i32) {
        let mut avail = [true; 16];
        for i in 0..n {
            avail[i] = true;
            self.proj[i] = r(r(r(p[i][2] * axis[2]) + r(p[i][0] * axis[0])) + r(axis[1] * p[i][1]));
        }
        for slot in order.iter_mut().take(n) {
            let mut best = 0;
            while best < n && !avail[best] {
                best += 1;
            }
            for j in best + 1..n {
                if avail[j]
                    && (if tie_mode != 0 {
                        le(self.proj[j], self.proj[best])
                    } else {
                        lt(self.proj[j], self.proj[best])
                    })
                {
                    best = j;
                }
            }
            avail[best] = false;
            *slot = best;
        }
    }

    // ---------------------------------------------------------------- cluster fits

    /// The best split of the ordered colours into up to four clusters by least squares
    /// (0x10082c20); writes the two endpoints (centred), the new axis and the number of clusters,
    /// returns the error.
    fn fit_clusters(
        &mut self,
        mode: &mut i32,
        n: usize,
        sums: &[V3; 16],
        weights: &[f64; 16],
        order: &[usize; 16],
        endpoints: &mut [V3; 2],
        axis: &mut V3,
    ) -> f64 {
        for p in 0..n {
            self.ord_sum[p] = sums[order[p]];
            self.ord_w[p] = weights[order[p]];
        }
        self.fit_s = [[0.0; 3]; 3];
        self.fit_c = [0.0; 3];
        self.fit_q = [0.0; 3];
        let ni = n as i32;
        let mut best = [ni; 4];
        let mut i = ni;
        loop {
            let mut j = ni;
            loop {
                let mut k = ni;
                loop {
                    let s = &self.fit_s;
                    let q = &self.fit_q;
                    let num = r(r(-r(s[2][0] * s[2][0]) - r(s[2][1] * s[2][1])) - r(s[2][2] * s[2][2]));
                    let t = r(self.fit_best_num * q[2]);
                    let u = r(self.fit_best_den * num);
                    if le(u, t) {
                        self.fit_best_num = num;
                        self.fit_best_den = q[2];
                        best[0] = i;
                        best[1] = j;
                        best[2] = k;
                    }
                    k -= 1;
                    if k < j || *mode < 3 {
                        break;
                    }
                    let ku = k as usize;
                    for c in 0..3 {
                        self.fit_s[2][c] = r(self.fit_s[2][c] + self.ord_sum[ku][c]);
                    }
                    let (c2, w) = (self.fit_c[2], self.ord_w[ku]);
                    self.fit_q[2] = r(r(r(r(5.0 - r(c2 + c2)) - w) * w) + self.fit_q[2]);
                    self.fit_c[2] = r(c2 + w);
                }
                j -= 1;
                if j < i {
                    break;
                }
                let ju = j as usize;
                for c in 0..3 {
                    self.fit_s[1][c] = r(self.fit_s[1][c] + self.ord_sum[ju][c]);
                    self.fit_s[2][c] = self.fit_s[1][c];
                }
                let (c1, w) = (self.fit_c[1], self.ord_w[ju]);
                self.fit_q[1] = r(r(r(r(3.0 - r(c1 + c1)) - w) * w) + self.fit_q[1]);
                self.fit_q[2] = self.fit_q[1];
                self.fit_c[1] = r(c1 + w);
                self.fit_c[2] = self.fit_c[1];
            }
            i -= 1;
            if i < 1 {
                break;
            }
            let iu = i as usize;
            for c in 0..3 {
                self.fit_s[0][c] = r(self.fit_s[0][c] + self.ord_sum[iu][c]);
                self.fit_s[1][c] = self.fit_s[0][c];
                self.fit_s[2][c] = self.fit_s[0][c];
            }
            let (c0, w) = (self.fit_c[0], self.ord_w[iu]);
            self.fit_q[0] = r(r(r(r(1.0 - r(c0 + c0)) - w) * w) + self.fit_q[0]);
            self.fit_q[1] = self.fit_q[0];
            self.fit_q[2] = self.fit_q[0];
            self.fit_c[0] = r(c0 + w);
            self.fit_c[1] = self.fit_c[0];
            self.fit_c[2] = self.fit_c[0];
        }

        best[3] = ni;
        let distinct = (best[1] != best[2]) as i32 + (best[0] != best[1]) as i32 + (best[2] != ni) as i32;
        if distinct == 1 {
            best[1] = best[0];
        }
        let (ord_w, ord_sum) = (self.ord_w, self.ord_sum);
        let (s2, c2, q, cnt) = cluster_sums(n, &best, &ord_w, &ord_sum);
        self.fit_s[2] = s2;
        self.fit_c[2] = c2;
        self.fit_q[2] = r(q - r(c2 * c2));
        let q2 = self.fit_q[2];
        let neg_c = -c2;
        let cnt_c = r(cnt as f64 - c2);
        let num = r(r(-r(s2[0] * s2[0]) - r(s2[1] * s2[1])) - r(s2[2] * s2[2]));
        for c in 0..3 {
            endpoints[0][c] = r(r(neg_c * s2[c]) / q2);
            endpoints[1][c] = r(r(cnt_c * s2[c]) / q2);
            axis[c] = s2[c];
        }
        *mode = if distinct == 1 { 1 } else { cnt };
        r(num / q2)
    }

    /// fit_clusters with the endpoints clamped to the channel bounds (0x10083310).
    #[allow(clippy::too_many_arguments)]
    fn fit_clusters_bounded(
        &mut self,
        mode: &mut i32,
        n: usize,
        sums: &[V3; 16],
        weights: &[f64; 16],
        order: &[usize; 16],
        lo: &V3,
        hi: &V3,
        endpoints: &mut [V3; 2],
        axis: &mut V3,
    ) -> f64 {
        for p in 0..n {
            self.ord_sum2[p] = sums[order[p]];
            self.ord_w2[p] = weights[order[p]];
        }
        self.bfit_s = [[0.0; 3]; 3];
        self.bfit_c = [0.0; 3];
        self.bfit_q = [0.0; 3];
        let ni = n as i32;
        let mut level = 0i32;
        let mut best = [ni; 4];
        let mut i = ni;
        loop {
            let mut j = ni;
            loop {
                let mut k = ni;
                loop {
                    let neg_c = -self.bfit_c[2];
                    let t_lvl = level as f64;
                    let tmc = r(t_lvl - self.bfit_c[2]);
                    let q = self.bfit_q[2];
                    let a = r(r(neg_c * neg_c) + q);
                    let b = r(r(tmc * neg_c) + q);
                    let c = r(r(tmc * tmc) + q);
                    let d = r(c * a);
                    let e = r(r(r(r(t_lvl * t_lvl) * q) * d) * d);
                    let mut acc = 0.0;
                    for ch in 0..3 {
                        let s = self.bfit_s[2][ch];
                        let qlo = r(q * lo[ch]);
                        let qhi = r(q * hi[ch]);
                        let ns = r(neg_c * s);
                        let ts = r(tmc * s);
                        let u = r(qlo - ns);
                        let v = r(qhi - ns);
                        let w1 = r(r(qlo - ts) * a);
                        let w2 = r(r(qhi - ts) * a);
                        let p = r(r(r(r(v - v.abs()) + r(u.abs() + u)) * 0.5) * b);
                        let ud = r(u * d);
                        let vd = r(v * d);
                        let rr = r(r(r(w2 - r(w2 - p).abs()) + r(r(w1 - p).abs() + w1)) * 0.5);
                        let x1 = r(rr * b);
                        let rc = r(rr * c);
                        let z = r(r(r(vd - r(vd - x1).abs()) + r(r(ud - x1).abs() + ud)) * 0.5);
                        let diff = r(z - rc);
                        let gg = r(r(rc * neg_c) - r(z * tmc));
                        let h = r(r(q * diff) * diff);
                        acc = r(acc + r(r(h + r(gg * gg)) - r(r(s * s) * e)));
                    }
                    let eqv = r(q * e);
                    let t = r(self.bfit_best_num * eqv);
                    let u = r(self.bfit_best_den * acc);
                    if le(u, t) {
                        self.bfit_best_num = acc;
                        self.bfit_best_den = eqv;
                        best[0] = i;
                        best[1] = j;
                        best[2] = k;
                    }
                    k -= 1;
                    if k < j || *mode < 3 {
                        break;
                    }
                    level = 3;
                    let ku = k as usize;
                    for ch in 0..3 {
                        self.bfit_s[2][ch] = r(self.bfit_s[2][ch] + self.ord_sum2[ku][ch]);
                    }
                    let (c2, w) = (self.bfit_c[2], self.ord_w2[ku]);
                    self.bfit_q[2] = r(r(r(r(5.0 - r(c2 + c2)) - w) * w) + self.bfit_q[2]);
                    self.bfit_c[2] = r(c2 + w);
                }
                j -= 1;
                if j < i {
                    break;
                }
                level = 2;
                let ju = j as usize;
                for ch in 0..3 {
                    self.bfit_s[1][ch] = r(self.bfit_s[1][ch] + self.ord_sum2[ju][ch]);
                    self.bfit_s[2][ch] = self.bfit_s[1][ch];
                }
                let (c1, w) = (self.bfit_c[1], self.ord_w2[ju]);
                self.bfit_q[1] = r(r(r(r(3.0 - r(c1 + c1)) - w) * w) + self.bfit_q[1]);
                self.bfit_q[2] = self.bfit_q[1];
                self.bfit_c[1] = r(c1 + w);
                self.bfit_c[2] = self.bfit_c[1];
            }
            i -= 1;
            if i < 1 {
                break;
            }
            level = 1;
            let iu = i as usize;
            for ch in 0..3 {
                self.bfit_s[0][ch] = r(self.bfit_s[0][ch] + self.ord_sum2[iu][ch]);
                self.bfit_s[1][ch] = self.bfit_s[0][ch];
                self.bfit_s[2][ch] = self.bfit_s[0][ch];
            }
            let (c0, w) = (self.bfit_c[0], self.ord_w2[iu]);
            self.bfit_q[0] = r(r(r(r(1.0 - r(c0 + c0)) - w) * w) + self.bfit_q[0]);
            self.bfit_q[1] = self.bfit_q[0];
            self.bfit_q[2] = self.bfit_q[0];
            self.bfit_c[0] = r(c0 + w);
            self.bfit_c[1] = self.bfit_c[0];
            self.bfit_c[2] = self.bfit_c[0];
        }

        best[3] = ni;
        let distinct = (best[1] != best[2]) as i32 + (best[2] != ni) as i32 + (best[0] != best[1]) as i32;
        if distinct == 1 {
            best[1] = best[0];
        }
        let (ord_w2, ord_sum2) = (self.ord_w2, self.ord_sum2);
        let (s2, c2, qs, cnt) = cluster_sums(n, &best, &ord_w2, &ord_sum2);
        self.bfit_s[2] = s2;
        self.bfit_c[2] = c2;
        let q = r(qs - r(c2 * c2));
        self.bfit_q[2] = q;
        let neg_c = -c2;
        let t_cnt = cnt as f64;
        let tmc = r(t_cnt - c2);
        let a = r(r(neg_c * neg_c) + q);
        let b = r(r(tmc * neg_c) + q);
        let c = r(r(tmc * tmc) + q);
        let d = r(c * a);
        let e = r(r(r(r(t_cnt * t_cnt) * d) * d) * q);
        let mut acc = 0.0;
        for ch in 0..3 {
            let s = s2[ch];
            let qlo = r(q * lo[ch]);
            let qhi = r(q * hi[ch]);
            let ns = r(neg_c * s);
            let ts = r(tmc * s);
            let u = r(qlo - ns);
            let v = r(qhi - ns);
            let w1 = r(r(qlo - ts) * a);
            let w2 = r(r(qhi - ts) * a);
            let p = r(r(r(r(v - v.abs()) + r(u.abs() + u)) * 0.5) * b);
            let ud = r(u * d);
            let vd = r(v * d);
            let rr = r(r(r(w2 - r(w2 - p).abs()) + r(r(w1 - p).abs() + w1)) * 0.5);
            let x1 = r(rr * b);
            let rc = r(rr * c);
            let z = r(r(r(vd - r(vd - x1).abs()) + r(r(ud - x1).abs() + ud)) * 0.5);
            let diff = r(z - rc);
            let gg = r(r(rc * neg_c) - r(z * tmc));
            let h = r(r(diff * diff) * q);
            acc = r(acc + r(r(h + r(gg * gg)) - r(r(s * s) * e)));
            endpoints[0][ch] = r(r(r(z / d) + ns) / q);
            endpoints[1][ch] = r(r(r(tmc * s) + r(rc / d)) / q);
            axis[ch] = s;
        }
        *mode = if distinct == 1 { 1 } else { cnt };
        r(acc / r(e * q))
    }

    // ---------------------------------------------------------------- quantisation

    /// The endpoints rounded to 5:6:5 both ways per channel; the best of the 64 combinations for
    /// the colours; the palette indices (0x10084470). `mode` 3 or 4 colours.
    #[allow(clippy::too_many_arguments)]
    fn quantize(
        &mut self,
        mode: i32,
        n: usize,
        points: &[V3; 16],
        point_weights: &[f64; 16],
        chan_w: &V3,
        ep: &[V3; 2],
        ep_out: &mut [[i32; 3]; 2],
        indices: &mut [i32; 16],
    ) -> f64 {
        // Per channel four endpoint choices of four entries.
        let mut a_tab = [[[0i32; 4]; 4]; 3];
        for c in 0..3 {
            let bits = BITS[c];
            let step = 1 << (8 - bits);
            let mask = 0x100 - step;
            for e in 0..2 {
                self.quant_tmp = if eq(chan_w[c], 0.0) { 0.0 } else { r(r(ep[e][c] / chan_w[c]) * 255.0) };
                let mut q = ftol(self.quant_tmp.floor());
                if q < 0 {
                    q = 0;
                } else if q >= 256 {
                    q = mask;
                }
                q &= mask;
                if !le(((q >> bits) + q) as f64, self.quant_tmp) && q - step >= 0 {
                    q -= step;
                }
                let lo = (q >> bits) + q;
                if q + step < 256 {
                    q += step;
                }
                let hi = q + (q >> bits);
                if e == 0 {
                    a_tab[c][0][0] = lo;
                    a_tab[c][1][0] = lo;
                    a_tab[c][2][0] = hi;
                    a_tab[c][3][0] = hi;
                } else {
                    a_tab[c][0][1] = lo;
                    a_tab[c][2][1] = lo;
                    a_tab[c][1][1] = hi;
                    a_tab[c][3][1] = hi;
                }
            }
        }

        let mut best: i32 = -1;
        let stride = if mode == 3 { 3 } else { 4 };
        let modeu = mode as usize;
        if mode == 3 || mode == 4 {
            for c in 0..3 {
                for gi in 0..4 {
                    let g = &mut a_tab[c][gi];
                    if mode == 3 {
                        g[2] = (g[1] + g[0]) / 2;
                    } else {
                        g[2] = (g[1] + g[0] * 2 + 1) / 3;
                        g[3] = (g[0] + g[1] * 2 + 1) / 3;
                    }
                    let g = *g;
                    for k in 0..modeu {
                        self.palette[c][gi][k] = r(r(g[k] as f64 * chan_w[c]) * INV_255);
                    }
                    let base = (c * 4 + gi) * 64;
                    for p in 0..n {
                        let x = points[p][c];
                        let w = point_weights[p];
                        for k in 0..modeu {
                            let d = r(x - self.palette[c][gi][k]);
                            self.err_tab[base + p * stride + k] =
                                if mode == 3 && k < 2 { r(r(d * w) * d) } else { r(r(d * d) * w) };
                        }
                    }
                }
            }
            self.best_quant_err =
                r(r(r(r(chan_w[0] * chan_w[0]) + r(chan_w[1] * chan_w[1])) + r(chan_w[2] * chan_w[2])) * 32.0);
            for combo in 0..64usize {
                let t0 = (combo & 3) * 64;
                let t1 = (4 + ((combo >> 2) & 3)) * 64;
                let t2 = (2 * 4 + (combo >> 4)) * 64;
                let mut sum = 0.0;
                if (1..=16).contains(&n) {
                    for p in (0..n).rev() {
                        let o = p * stride;
                        let mut e = [0.0f64; 4];
                        for (k, ek) in e.iter_mut().enumerate().take(modeu) {
                            *ek = r(r(self.err_tab[t2 + o + k] + self.err_tab[t1 + o + k]) + self.err_tab[t0 + o + k]);
                        }
                        let m01 = r(r(e[1] + e[0]) - r(e[0] - e[1]).abs());
                        let m = if mode == 3 {
                            let e2 = r(e[2] + e[2]);
                            r(r(m01 + e2) - r(e2 - m01).abs())
                        } else {
                            let m23 = r(r(e[3] + e[2]) - r(e[2] - e[3]).abs());
                            r(r(m01 + m23) - r(m23 - m01).abs())
                        };
                        sum = if p == n - 1 { m } else { r(sum + m) };
                    }
                }
                if lt(sum, self.best_quant_err) {
                    self.best_quant_err = sum;
                    best = combo as i32;
                }
            }
        }

        // (If no combination beat the bound, best stays -1 and selects choice 3 of every channel,
        // as the original's arithmetic shift does.)
        let sel = |shift: i32| ((best >> shift) & 3) as usize;
        for c in 0..3 {
            let g = a_tab[c][sel(2 * c as i32)];
            for e in 0..2 {
                ep_out[e][c] = g[e] >> (8 - BITS[c]);
            }
        }
        let p0 = self.palette[0][sel(0)];
        let p1 = self.palette[1][sel(2)];
        let p2 = self.palette[2][sel(4)];
        let mut total = 0.0;
        for p in 0..n {
            let mut best_k = -1;
            let mut min_err =
                r(r(r(r(chan_w[0] * chan_w[0]) + r(chan_w[1] * chan_w[1])) + r(chan_w[2] * chan_w[2])) * 2.0);
            for k in 0..modeu {
                let dz = r(points[p][2] - p2[k]);
                let dy = r(points[p][1] - p1[k]);
                let dx = r(points[p][0] - p0[k]);
                let err = r(r(r(r(dx * dx) + r(dz * dz)) + r(dy * dy)) * point_weights[p]);
                if lt(err, min_err) {
                    min_err = err;
                    best_k = k as i32;
                }
            }
            indices[p] = best_k;
            total = r(total + min_err);
        }
        total
    }

    /// quantize, and with extended endpoints for the other colour counts; keeps the better
    /// (0x10084290). `result_mode`: in the fit's mode, out 3 or 4.
    #[allow(clippy::too_many_arguments)]
    fn final_quantize(
        &mut self,
        result_mode: &mut i32,
        mode: i32,
        n: usize,
        points: &[V3; 16],
        point_weights: &[f64; 16],
        chan_w: &V3,
        ep: &[V3; 2],
        ep_out: &mut [[i32; 3]; 2],
        indices: &mut [i32; 16],
    ) -> f64 {
        let mut kinds = [0i32; 2];
        let mut cand_ep = [[[0i32; 3]; 2]; 2];
        let mut cand_idx = [[0i32; 16]; 2];
        kinds[0] = if *result_mode == 4 { 1 } else { 0 };
        self.quant_err[0] =
            self.quantize(kinds[0] + 3, n, points, point_weights, chan_w, ep, &mut cand_ep[0], &mut cand_idx[0]);
        let mut cur = 1usize;
        let variants = mode - 2;
        *result_mode -= 2;
        for v in 0..variants.max(0) as usize {
            let count = VARIANT_COUNT[v * 3 + *result_mode as usize];
            for s in 0..count {
                for e in 0..2 {
                    let k = XFORM[((v * 2 + *result_mode as usize) * 6 + s) * 2 + e];
                    for c in 0..3 {
                        self.xform_ep[e][c] = r(r(k[1] * ep[1][c]) + r(k[0] * ep[0][c]));
                    }
                }
                kinds[cur] = v as i32;
                let xform = self.xform_ep;
                let err = self.quantize(
                    v as i32 + 3,
                    n,
                    points,
                    point_weights,
                    chan_w,
                    &xform,
                    &mut cand_ep[cur],
                    &mut cand_idx[cur],
                );
                self.quant_err[cur] = err;
                if lt(err, self.quant_err[1 - cur]) {
                    cur = 1 - cur;
                }
            }
        }
        let best = 1 - cur;
        indices[..n].copy_from_slice(&cand_idx[best][..n]);
        *ep_out = cand_ep[best];
        *result_mode = kinds[best] + 3;
        self.quant_err[best]
    }

    // ---------------------------------------------------------------- the colour block

    /// The ordering pass's sort and repeat check (sortAndCheck in the port).
    fn sort_and_check(&mut self, o: &mut Orders, unique: usize, check_budget: bool, budget: i32) {
        let points = self.unique;
        let axis = self.axis;
        let mut tie = 0;
        loop {
            let mut slot_order = o.orderings[o.slot];
            self.sort_by_projection(unique, &points, &axis, &mut slot_order, tie);
            o.orderings[o.slot] = slot_order;
            let mut hit;
            if o.slot >= 1 {
                o.found = false;
                let mut s = o.slot as i32 - 1;
                while s >= 0 && !o.found {
                    o.found = same_order(unique, &o.orderings[s as usize], &o.orderings[o.slot]);
                    s -= 1;
                }
                hit = o.found;
            } else {
                hit = o.found;
            }
            if !hit && o.wrapped {
                let mut s = o.slot + 1;
                while s < 16 && !o.found {
                    o.found = same_order(unique, &o.orderings[s], &o.orderings[o.slot]);
                    s += 1;
                }
                hit = o.found;
            }
            if !hit {
                return;
            }
            if check_budget && budget == 0 {
                return;
            }
            tie += 1;
            if tie >= 2 {
                return;
            }
        }
    }

    fn encode_block(&mut self, b: &mut Block) {
        if b.count == 0 {
            return;
        }
        let n = b.count;
        let nd = n as f64;
        self.inv_count = r(1.0 / nd);
        for c in 0..3 {
            self.weights[c] = if lt(b.weights[c], 0.0) { 0.0 } else { b.weights[c] };
        }
        let mut uniform = true;
        for c in 0..3 {
            self.mean[c] = 0.0;
            for i in 0..n {
                uniform &= lt(r(b.colors[i][c] - b.colors[0][c]).abs(), 0.00048828125);
                self.scaled[i][c] = r(self.weights[c] * b.colors[i][c]);
                self.mean[c] = r(self.scaled[i][c] + self.mean[c]);
            }
        }
        if uniform {
            let mut mode = b.mode;
            let (scaled, weights) = (self.scaled, self.weights);
            self.encode_single_color(&mut mode, n, &scaled, &weights, &mut b.endpoints, &mut b.indices);
            b.result_mode = mode;
            return;
        }
        for c in 0..3 {
            let m = r(self.mean[c] / nd);
            self.mean[c] = m;
            for i in 0..n {
                self.centered[i][c] = r(self.scaled[i][c] - m);
            }
            self.lo[c] = -m;
            self.hi[c] = r(self.weights[c] - m);
        }

        // The distinct colours, with weights and centred sums.
        let mut unique = 0usize;
        let mut map = [0usize; 16];
        for i in 0..n {
            let mut j = 0;
            while j < unique {
                if eq(self.unique[j][0], self.scaled[i][0])
                    && eq(self.unique[j][1], self.scaled[i][1])
                    && eq(self.unique[j][2], self.scaled[i][2])
                {
                    break;
                }
                j += 1;
            }
            if j == unique {
                self.unique_weight[j] = 1.0;
                unique += 1;
                self.unique[j] = self.scaled[i];
                self.unique_sum[j] = self.centered[i];
            } else {
                for c in 0..3 {
                    self.unique_sum[j][c] = r(self.centered[i][c] + self.unique_sum[j][c]);
                }
                self.unique_weight[j] = r(self.unique_weight[j] + 1.0);
            }
            map[i] = j;
        }
        for j in 0..unique {
            self.unique_weight[j] = r(self.inv_count * self.unique_weight[j]);
            for c in 0..3 {
                self.unique_sum[j][c] = r(self.inv_count * self.unique_sum[j][c]);
            }
        }

        let mut local_idx = [0i32; 16];
        if unique == 2 {
            b.result_mode = unique as i32;
            let (pts, w, cw) = (self.unique, self.unique_weight, self.weights);
            let ep = [pts[0], pts[1]];
            self.final_quantize(&mut b.result_mode, b.mode, unique, &pts, &w, &cw, &ep, &mut b.endpoints, &mut local_idx);
            for i in 0..n {
                b.indices[i] = local_idx[map[i]];
            }
            return;
        }

        // Two passes of "fit the clusters along the axis, sort along the fit's axis" until an
        // ordering repeats: from the principal axis, then from the farthest pair.
        let mut o = Orders { orderings: [[0; 16]; 16], slot: 0, wrapped: false, found: true };
        let mut best_order = [[0usize; 16]; 2];
        let mut modes = [0i32; 2];
        let centered = self.centered;
        self.axis = self.principal_axis(n, &centered);
        {
            let (pts, axis) = (self.unique, self.axis);
            let mut first = o.orderings[0];
            self.sort_by_projection(unique, &pts, &axis, &mut first, 0);
            o.orderings[0] = first;
        }
        let mut last_eval = 0usize;
        for pass in 0..2usize {
            let mut budget: i32 = 16;
            loop {
                modes[pass] = b.mode - 1;
                if pass as i32 + budget - 1 == 16 {
                    let pts = self.unique;
                    self.axis = self.farthest_pair_axis(unique, &pts);
                } else {
                    let (sums, w, order) = (self.unique_sum, self.unique_weight, o.orderings[o.slot]);
                    let mut ep = [[0.0; 3]; 2];
                    let mut axis = [0.0; 3];
                    let mut m = modes[pass];
                    self.pass_err[pass] = self.fit_clusters(&mut m, unique, &sums, &w, &order, &mut ep, &mut axis);
                    modes[pass] = m;
                    self.pass_ep[pass] = ep;
                    self.axis = axis;
                    last_eval = pass;
                }
                o.slot = (o.slot + 1) & 15;
                o.wrapped |= o.slot == 0;
                self.sort_and_check(&mut o, unique, true, budget);
                if budget == 0 || o.found {
                    break;
                }
                budget -= 1;
            }
            best_order[pass][..unique].copy_from_slice(&o.orderings[o.slot][..unique]);
        }

        let best = if last_eval == 1 && !lt(self.pass_err[0], self.pass_err[1]) { 1 } else { 0 };
        let mut out = false;
        for k in 0..2 {
            for c in 0..3 {
                if !out {
                    let e = self.pass_ep[best][k][c];
                    out = lt(e, self.lo[c]) || !le(e, self.hi[c]);
                }
            }
        }
        if out {
            // An endpoint left the colour range: fit again with clamping, from the best ordering.
            o.orderings[0][..unique].copy_from_slice(&best_order[best][..unique]);
            o.slot = 0;
            o.wrapped = false;
            let mut budget: i32 = 8;
            loop {
                modes[best] = b.mode - 1;
                let (sums, w, order, lo, hi) =
                    (self.unique_sum, self.unique_weight, o.orderings[o.slot], self.lo, self.hi);
                let mut ep = [[0.0; 3]; 2];
                let mut axis = [0.0; 3];
                let mut m = modes[best];
                self.pass_err[best] =
                    self.fit_clusters_bounded(&mut m, unique, &sums, &w, &order, &lo, &hi, &mut ep, &mut axis);
                modes[best] = m;
                self.pass_ep[best] = ep;
                self.axis = axis;
                o.slot = (o.slot + 1) & 15;
                o.wrapped |= o.slot == 0;
                self.sort_and_check(&mut o, unique, false, budget);
                if budget == 0 || o.found {
                    break;
                }
                budget -= 1;
            }
        }

        // Un-centred endpoints into the other pass's slot, then the 5:6:5 quantisation.
        for k in 0..2 {
            for c in 0..3 {
                self.pass_ep[1 - best][k][c] = r(self.pass_ep[best][k][c] + self.mean[c]);
            }
        }
        b.result_mode = modes[best] + 1;
        let (pts, w, cw, ep) = (self.unique, self.unique_weight, self.weights, self.pass_ep[1 - best]);
        self.final_quantize(&mut b.result_mode, b.mode, unique, &pts, &w, &cw, &ep, &mut b.endpoints, &mut local_idx);
        for i in 0..n {
            b.indices[i] = local_idx[map[i]];
        }
    }

    // ---------------------------------------------------------------- encode

    /// Compresses a 16-bit surface (0x10080f00) into DXT blocks appended to `out`: `kind` is
    /// S3TC_OPAQUE, S3TC_ALPHA_KEY (DXT1) or S3TC_ALPHA_EXPLICIT (DXT3); `weights` the error
    /// weights of red, green, blue.
    pub fn encode(&mut self, src: &Source, out: &mut Vec<u8>, kind: u32, weights: &[f32; 3]) {
        let bpp = 2usize;
        let shift = [low_bit(src.masks[0]), low_bit(src.masks[1]), low_bit(src.masks[2])];
        let alpha_shift = low_bit(src.masks[3]);
        let key_mode = kind & 0xff;
        // g_alphaThreshold is 0: nothing in LS3DF sets it.
        let alpha_ref: u32 = 0;
        let pixel = |off: usize| -> u32 { u16::from_le_bytes([src.pixels[off], src.pixels[off + 1]]) as u32 };

        let mut y = 0u32;
        while y < src.height {
            let rows = (src.height - y).min(4);
            let row_off = y as usize * src.pitch;
            let mut x = 0u32;
            while x < src.width {
                let cols = (src.width - x).min(4);
                let block_off = row_off + x as usize * bpp;
                let mut block = Block {
                    count: 0,
                    colors: [[0.0; 3]; 16],
                    weights: [weights[0] as f64, weights[1] as f64, weights[2] as f64],
                    mode: 0,
                    result_mode: 0,
                    endpoints: [[0; 3]; 2],
                    indices: [0; 16],
                };
                let mut map = [-1i32; 16];

                if kind & 0xff00 == S3TC_ALPHA_EXPLICIT {
                    let mut acc: u32 = 0;
                    for rr in 0..4u32 {
                        if rr < rows {
                            for c in 0..4u32 {
                                if c < cols {
                                    let p = block_off + rr as usize * src.pitch + c as usize * bpp;
                                    let a = (src.masks[3] & pixel(p)) >> alpha_shift;
                                    let amax = src.masks[3] >> alpha_shift;
                                    let v = r(r(r(a as f64 / amax as f64) * 15.0) + 0.5).floor();
                                    acc = ((ftol(v) as u32) << 12) | ((acc & 0xffff) >> 4);
                                } else {
                                    acc = (acc & 0xffff) >> 4;
                                }
                            }
                            out.extend_from_slice(&(acc as u16).to_le_bytes());
                        } else {
                            out.extend_from_slice(&[0, 0]);
                        }
                    }
                }

                // The colours (opaque pixels only).
                for rr in 0..4u32 {
                    for c in 0..4u32 {
                        let i = (rr * 4 + c) as usize;
                        if c < cols && rr < rows {
                            let pv = pixel(block_off + rr as usize * src.pitch + c as usize * bpp);
                            let opaque = match key_mode {
                                S3TC_OPAQUE => true,
                                S3TC_ALPHA_KEY => alpha_ref < (src.masks[3] & pv),
                                _ => false,
                            };
                            if opaque {
                                map[i] = block.count as i32;
                                for ch in 0..3 {
                                    let v = (src.masks[ch] & pv) >> shift[ch];
                                    let vmax = src.masks[ch] >> shift[ch];
                                    block.colors[block.count][ch] = r(v as f64 / vmax as f64);
                                }
                                block.count += 1;
                            }
                        }
                    }
                }

                block.mode = if block.count >= (cols * rows) as usize { 4 } else { 3 };
                if block.count == 0 {
                    block.endpoints = [[0; 3]; 2];
                } else {
                    self.encode_block(&mut block);
                }
                let ep = &block.endpoints;
                let mut c0 = ((((ep[0][0] << 6) | ep[0][1]) << 5) | ep[0][2]) as u16;
                let mut c1 = ((((ep[1][0] << 6) | ep[1][1]) << 5) | ep[1][2]) as u16;
                if c1 == c0 {
                    c1 = c1.wrapping_add(1);
                    for idx in block.indices.iter_mut().take(block.count) {
                        *idx = 0;
                    }
                    block.result_mode = 3;
                }
                let swap = (c1 < c0) == (block.result_mode == 3);
                if swap {
                    std::mem::swap(&mut c0, &mut c1);
                }
                let mut codes: u32 = 0;
                for i in (0..16).rev() {
                    if map[i] < 0 {
                        codes = (codes << 2) | 3;
                    } else {
                        let mut code = block.indices[map[i] as usize] as u32;
                        if swap {
                            code ^= if block.result_mode == 4 { 1 } else { (!code >> 1) & 1 };
                        }
                        codes = (codes << 2) | code;
                    }
                }
                out.extend_from_slice(&c0.to_le_bytes());
                out.extend_from_slice(&c1.to_le_bytes());
                out.extend_from_slice(&codes.to_le_bytes());
                x += 4;
            }
            y += 4;
        }
    }
}

/// The cluster sums of the best split, shared by both fits: (sums, weight, weighted square sum,
/// cluster count).
fn cluster_sums(n: usize, best: &[i32; 4], weights: &[f64; 16], sums: &[V3; 16]) -> (V3, f64, f64, i32) {
    let mut s = [0.0; 3];
    let mut cw = 0.0;
    let mut q = 0.0;
    let mut cnt = 0;
    let mut pos = 0i32;
    for (t, &end) in best.iter().enumerate() {
        if pos < end {
            let tt = t as f64;
            while pos < end {
                let p = pos as usize;
                let tw = r(tt * weights[p]);
                cw = r(cw + tw);
                q = r(q + r(tt * tw));
                for c in 0..3 {
                    s[c] = r(s[c] + r(tt * sums[p][c]));
                }
                pos += 1;
            }
        }
        cnt += (end != n as i32) as i32;
    }
    (s, cw, q, cnt)
}

/// Two orderings are the same, or one is the other reversed (0x10082bb0).
fn same_order(n: usize, a: &[usize; 16], b: &[usize; 16]) -> bool {
    let mut i = 0;
    if n > 0 {
        while a[i] == b[i] {
            i += 1;
            if i >= n {
                return i == n;
            }
        }
        i = 0;
        while i < n && a[i] == b[n - 1 - i] {
            i += 1;
        }
    }
    i == n
}

fn low_bit(mut mask: u32) -> u32 {
    let mut n = 0;
    while mask & 1 == 0 && n < 32 {
        mask >>= 1;
        n += 1;
    }
    n
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn r_rounds_to_24_bits() {
        assert_eq!(r(1.0), 1.0);
        assert_eq!(r(1.0 / 3.0), (1.0f32 / 3.0f32) as f64);
        assert_eq!(r(0.1), 0.1f32 as f64);
    }
}
