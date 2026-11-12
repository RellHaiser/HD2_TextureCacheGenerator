// The S3TC (DXT) compressor (S3 Inc.'s S3TC library, 0x10080ed0 - 0x10086500).
//
// The library works in doubles, but the game runs the x87 FPU in Direct3D's single precision:
// every operation's result is rounded to a 24-bit mantissa. R() reproduces that (the exact result
// rounded to float, computed in double: exact for products of floats, correctly rounded for sums,
// quotients and square roots of floats since 53 >= 2 * 24 + 2). Every expression below keeps the
// original's operation order. Lt / Le / Eq are the x87 compare outcomes (unordered compares count
// as less and equal, as FCOM sets all of C0, C2, C3 for them).
//
// Like the original, the encoder keeps its work arrays in globals (g); a few of them carry over
// from one block to the next (the best ratios of the two cluster fits, the single-colour table).
#include "igraph/s3tc.h"

#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <cstring>

#include "core/fpu.h"

namespace ls3d::s3tc {

uint32_t g_alphaThreshold = 0;

namespace {

inline double R(double x)
{
    // The mantissa rounded to 24 bits (nearest, ties to even), the double's exponent kept: x87
    // registers keep their wide exponent at 24-bit precision, and the power iteration below
    // reaches values far beyond float's range (up to 3.5^256).
    uint64_t u;
    std::memcpy(&u, &x, sizeof u);
    if (((u >> 52) & 0x7ff) == 0x7ff)
        return x;
    u += ((uint64_t(1) << 28) - 1) + ((u >> 29) & 1);
    u &= ~((uint64_t(1) << 29) - 1);
    std::memcpy(&x, &u, sizeof u);
    return x;
}
inline bool Lt(double a, double b) { return !(a >= b); }
inline bool Le(double a, double b) { return !(a > b); }
inline bool Eq(double a, double b) { return !(a < b) && !(a > b); }

constexpr double kInv255 = 0.00392156862745098;     // 0x100a1c98

constexpr int kBits[3] = {5, 6, 5};                 // 0x100a99b0 / 0x100a1cb0
// The three colours a block can produce from two endpoints a, b: (a * m0 + b * m1 + m2) / m3
// (0x100a99bc): a itself, the 3-colour midpoint, the 4-colour third.
constexpr int kMix[3][4] = {{1, 0, 0, 1}, {1, 1, 0, 2}, {1, 2, 1, 3}};
// Endpoint extensions tried by FinalQuantize: count per (variant, mode) (0x100a99ec) and the
// coefficient pairs (a, b) of new = a * e0 + b * e1 (0x100a9a08), indexed
// ((variant * 2 + mode) * 6 + slot) * 2 + endpoint.
constexpr int kVariantCount[6] = {2, 0, 0, 6, 2, 0};
constexpr double kXform[48][2] = {
    {2.0, -1.0}, {0.0, 1.0}, {1.0, 0.0}, {-1.0, 2.0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0},
    {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0},
    {1.0, 0.0}, {0.0, 1.0}, {1.0, 0.0}, {-2.0, 3.0}, {3.0, -2.0}, {0.0, 1.0},
    {2.0, -1.0}, {-1.0, 2.0}, {1.5, -0.5}, {0.0, 1.0}, {1.0, 0.0}, {-0.5, 1.5},
    {1.5, -0.5}, {0.0, 1.0}, {1.0, 0.0}, {-0.5, 1.5}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0},
};

// The colour block handed to EncodeBlock (on the stack of Encode, 0x200 bytes).
struct S_block {
    int count;                      // +0x000 opaque pixels
    int pad004;
    double colors[16][3];           // +0x008 0 - 1
    double weights[3];              // +0x188
    int mode;                       // +0x1A0 4: every pixel of the block opaque, 3: some transparent
    int resultMode;                 // +0x1A4 the block's mode (3 or 4)
    int endpoints[2][3];            // +0x1A8 5:6:5
    int indices[16];                // +0x1C0 per colour
};

// The alpha block of S3TC_ALPHA_INTERPOLATED.
struct S_alphaBlock {
    int count;                      // +0x00
    float alpha[16];                // +0x04 0 - 1
    int needZero;                   // +0x44 an endpoint must be 0 (always 0 here)
    int needOne;                    // +0x48
    int mode;                       // +0x4C 8 or 6 levels
    int alpha0;                     // +0x50
    int alpha1;                     // +0x54
    int indices[16];                // +0x58
};

// The library's globals: a mirror of the original's 0x100d2cd8 - 0x100d7340 (the differential
// test compares the two byte for byte).
struct S_work {
    double alphaSk;                 // 0x100d2cd8
    double unique[16][3];           // 0x100d2ce0 the block's distinct weighted colours
    double invCount;                // 0x100d2e60
    double alphaErrDen;             // 0x100d2e68
    double lo[3];                   // 0x100d2e70 bounds of the centred colours: -mean
    double hi[3];                   // 0x100d2e88 and weight - mean
    double kindErr[3];              // 0x100d2ea0
    double alphaMean;               // 0x100d2eb8
    double quantTmp;                // 0x100d2ec0
    double maxDist2;                // 0x100d2ec8
    double cov2[9];                 // 0x100d2ed0 the covariance squared (upper triangle, stride 3)
    double alphaN;                  // 0x100d2f18
    double alphaErr;                // 0x100d2f20
    double alphaSetErr[5];          // 0x100d2f28
    double ordSum[16][3];           // 0x100d2f50 FitClusters' copies in sort order
    double fitC[3];                 // 0x100d30d0 weight sums of the three levels
    double xformEp[2][3];           // 0x100d30e8
    double bestQuantErr;            // 0x100d3118
    double proj[16];                // 0x100d3120
    double scaled[16][3];           // 0x100d31a0
    double alphaErrNum;             // 0x100d3320
    double uniqueSum[16][3];        // 0x100d3328 centred colour sums of the distinct colours
    double bfitS[3][3];             // 0x100d34a8
    double errTab[3 * 4 * 64];      // 0x100d34f0 [channel][endpoint choice][point][palette entry]
    double ordW2[16];               // 0x100d4cf0
    double alphaCentered[16];       // 0x100d4d70
    double candErr[2];              // 0x100d4df0
    double alphaSets[5][16];        // 0x100d4e00
    double ordSum2[16][3];          // 0x100d5080
    double invNorm;                 // 0x100d5200
    double mean[3];                 // 0x100d5208
    uint8_t single[256][3][3][3];   // 0x100d5220 [value][channel][kind]: valid, endpoint 0, endpoint 1
    double alphaSkc;                // 0x100d6d20
    double passEp[2][2][3];         // 0x100d6d28
    double centered[16][3];         // 0x100d6d88
    double alphaInvN;               // 0x100d6f08
    double palette[3][4][4];        // 0x100d6f10
    double uniqueWeight[16];        // 0x100d7090
    double quantErr[2];             // 0x100d7110
    double alphaSq;                 // 0x100d7120
    double trace;                   // 0x100d7128
    double alphaLo;                 // 0x100d7130
    double alphaHi;                 // 0x100d7138
    double bfitC[3];                // 0x100d7140
    double fitQ[3];                 // 0x100d7158
    double axis[3];                 // 0x100d7170
    double bfitQ[3];                // 0x100d7188
    double alphaThresh[7];          // 0x100d71a0
    double passErr[2];              // 0x100d71d8
    double dist2;                   // 0x100d71e8
    double ordW[16];                // 0x100d71f0
    double weights[3];              // 0x100d7270
    double cov[9];                  // 0x100d7288
    double fitS[3][3];              // 0x100d72d0
    double alphaPrevLo;             // 0x100d7318
    double alphaPrevHi;             // 0x100d7320
    double fitBestNum;              // 0x100d7328
    double bfitBestNum;             // 0x100d7330
    int singleReady;                // 0x100d7338
    int pad733c;
};
static_assert(sizeof(S_work) == 0x100d7340 - 0x100d2cd8, "S3TC globals layout");
static_assert(offsetof(S_work, single) == 0x100d5220 - 0x100d2cd8, "S3TC globals layout");
static_assert(offsetof(S_work, fitBestNum) == 0x100d7328 - 0x100d2cd8, "S3TC globals layout");
// (HD2_TextureCacheGenerator: thread_local so that several textures can be compressed at once;
// each thread's state starts and carries over as the original's single one does.)
thread_local S_work g;
// The best ratios' denominators are initialised data (0x100a99a0, 0x100a99a8).
thread_local double g_fitBestDen = 1.0;
thread_local double g_bfitBestDen = 1.0;

// Single colours -----------------------------------------------------------------------------

// For every 8-bit value of each channel and colour kind, the endpoint pair closest together that
// produces it (0x10083d40, built on first use).
void BuildSingleColorTable()
{
    for (int ch = 0; ch < 3; ++ch) {
        const int bits = kBits[ch];
        const int max = (1 << bits) - 1;
        for (int a = max; a >= 0; --a) {
            const int ea = (a >> (2 * bits - 8)) | (a << (8 - bits));
            for (int b = max; b >= 0; --b) {
                const int eb = (b >> (2 * bits - 8)) | (b << (8 - bits));
                for (int k = 0; k < 3; ++k) {
                    const int v = (kMix[k][1] * eb + ea * kMix[k][0] + kMix[k][2]) / kMix[k][3];
                    uint8_t *e = g.single[v][ch][k];
                    if (!e[0] || std::abs(e[2] - e[1]) > std::abs(eb - ea)) {
                        e[0] = 1;
                        e[1] = static_cast<uint8_t>(a << (8 - bits));
                        e[2] = static_cast<uint8_t>(b << (8 - bits));
                    }
                }
            }
        }
    }
}

// A block of one colour (0x10083d40): the best endpoints per kind from the table.
double EncodeSingleColor(int *mode, int n, const double (*points)[3], const double *weights, int (*endpoints)[3],
                         int *indices)
{
    if (!g.singleReady) {
        g.singleReady = 1;
        BuildSingleColorTable();
    }
    int choice[3][3] = {};
    int kind = 0;
    for (; kind < *mode - 1; ++kind) {
        g.kindErr[kind] = 0.0;
        for (int ch = 0; ch < 3; ++ch) {
            int v = Ftol(std::floor(R(R(R(points[0][ch] * 255.0) / weights[ch]) + 0.5)));
            if (v < 0)
                v = 0;
            else if (v >= 256)
                v = 255;
            const bool valid = g.single[v][ch][kind][0] != 0;
            int cand[2];
            int dir = 1;
            for (int side = 0; side < 2; ++side) {
                int c = v;
                if (!valid || !Le(R(R(R(R(points[0][ch] * 255.0) / weights[ch]) - v) * dir), 0.0)) {
                    // The nearest valid value on this side.
                    c = v + dir;
                    if (c < 0)
                        c = 0;
                    else if (c >= 256)
                        c = 255;
                    while (!g.single[c][ch][kind][0] && c + dir >= 0 && c + dir < 256)
                        c += dir;
                }
                g.candErr[side] = 0.0;
                for (int i = 0; i < n; ++i) {
                    const double d = R(R(static_cast<double>(c) * weights[ch]) - R(points[i][ch] * 255.0));
                    g.candErr[side] = R(R(d * d) + g.candErr[side]);
                }
                cand[side] = c;
                dir = -dir;
            }
            const double e0 = g.candErr[0];
            const double e1 = g.candErr[1];
            if (Lt(e0, e1)) {
                g.kindErr[kind] = R(e0 + g.kindErr[kind]);
                choice[ch][kind] = cand[0];
            } else {
                g.kindErr[kind] = R(e1 + g.kindErr[kind]);
                if (Le(e0, e1))
                    choice[ch][kind] = (v & 1) ? cand[0] : cand[1];
                else
                    choice[ch][kind] = cand[1];
            }
        }
    }

    int best = kind;
    if (*mode == 4) {
        if (!Le(g.kindErr[0], g.kindErr[1]))
            best = Le(g.kindErr[1], g.kindErr[2]) ? 1 : 2;
        else
            best = Le(g.kindErr[0], g.kindErr[2]) ? 0 : 2;
    } else if (*mode == 3) {
        best = Le(g.kindErr[0], g.kindErr[1]) ? 0 : 1;
    }
    for (int ch = 0; ch < 3; ++ch) {
        const uint8_t *e = g.single[choice[ch][best]][ch][best];
        for (int k = 0; k < 2; ++k)
            endpoints[k][ch] = e[1 + k] >> (8 - kBits[ch]);
    }
    for (int i = 0; i < n; ++i)
        indices[i] = best + 1;
    if (best)
        *mode = best + 2;
    return g.kindErr[best];
}

// Axes and orderings -------------------------------------------------------------------------

// The principal axis of the centred colours by power iteration on the covariance (0x10082510).
void PrincipalAxis(int n, const double (*p)[3], double *axis)
{
    double *m = g.cov;
    m[8] = m[5] = m[4] = m[2] = m[1] = m[0] = 0.0;
    for (int i = 0; i < n; ++i) {
        const double x = p[i][0], y = p[i][1], z = p[i][2];
        m[0] = R(R(x * x) + m[0]);
        m[1] = R(R(y * x) + m[1]);
        m[2] = R(R(x * z) + m[2]);
        m[4] = R(R(y * y) + m[4]);
        m[5] = R(R(y * z) + m[5]);
        m[8] = R(R(z * z) + m[8]);
    }
    for (int it = 0; it < 3; ++it) {
        g.trace = R(R(m[0] + m[4]) + m[8]);
        const double scale = R(3.5 / g.trace);
        for (int r = 0; r < 3; ++r) {
            for (int k = r; k < 3; ++k)
                m[r * 3 + k] = R(scale * m[r * 3 + k]);
        }
        for (int sq = 0; sq < 4; ++sq) {
            double *s = g.cov2;
            const double a = m[0], b = m[1], c = m[2], d = m[4], e = m[5], f = m[8];
            const double bb = R(b * b), cc = R(c * c), ee = R(e * e);
            s[0] = R(R(R(a * a) + cc) + bb);
            s[1] = R(R(R(a + d) * b) + R(c * e));
            s[2] = R(R(R(a + f) * c) + R(b * e));
            s[4] = R(R(R(d * d) + ee) + bb);
            s[5] = R(R(R(d + f) * e) + R(b * c));
            s[8] = R(R(R(f * f) + ee) + cc);
            const double A = s[0], B = s[1], C = s[2], D = s[4], E = s[5], F = s[8];
            const double BB = R(B * B), CC = R(C * C), EE = R(E * E);
            m[0] = R(R(R(A * A) + BB) + CC);
            m[1] = R(R(R(D + A) * B) + R(E * C));
            m[2] = R(R(R(F + A) * C) + R(E * B));
            m[4] = R(R(R(D * D) + EE) + BB);
            m[5] = R(R(R(F + D) * E) + R(C * B));
            m[8] = R(R(R(F * F) + EE) + CC);
        }
    }
    int u;
    if (Le(m[0], m[4]))
        u = Le(m[4], m[8]) ? 2 : 1;
    else
        u = Le(m[0], m[8]) ? 2 : 0;
    g.invNorm = R(1.0 / R(std::sqrt(m[u * 4])));
    for (int k = 0; k < u; ++k)
        axis[k] = R(g.invNorm * g.cov2[k * 3 + u]);
    for (int k = u; k < 3; ++k)
        axis[k] = R(g.invNorm * g.cov2[u * 3 + k]);
}

// The direction between the two colours farthest apart (0x10082920).
void FarthestPairAxis(int n, const double (*p)[3], double *axis)
{
    int bi = 0, bj = 0;
    g.maxDist2 = 0.0;
    for (int i = 0; i < n; ++i) {
        for (int j = i; j < n; ++j) {
            const double dx = R(p[i][0] - p[j][0]);
            const double dy = R(p[i][1] - p[j][1]);
            const double dz = R(p[i][2] - p[j][2]);
            g.dist2 = R(R(R(dz * dz) + R(dy * dy)) + R(dx * dx));
            if (!Le(g.dist2, g.maxDist2)) {
                g.maxDist2 = g.dist2;
                bi = i;
                bj = j;
            }
        }
    }
    g.maxDist2 = R(1.0 / R(std::sqrt(g.maxDist2)));
    for (int c = 0; c < 3; ++c)
        axis[c] = R(R(p[bi][c] - p[bj][c]) * g.maxDist2);
}

// The colours in ascending order of their projection on the axis (0x10082a30); ties: the first
// (tieMode 0) or the last (1) of them first.
void SortByProjection(int n, const double (*p)[3], const double *axis, int *order, int tieMode)
{
    int avail[16];
    for (int i = 0; i < n; ++i) {
        avail[i] = 1;
        g.proj[i] = R(R(R(p[i][2] * axis[2]) + R(p[i][0] * axis[0])) + R(axis[1] * p[i][1]));
    }
    for (int k = 0; k < n; ++k) {
        int best = 0;
        while (best < n && !avail[best])
            ++best;
        for (int j = best + 1; j < n; ++j) {
            if (avail[j] && (tieMode ? Le(g.proj[j], g.proj[best]) : Lt(g.proj[j], g.proj[best])))
                best = j;
        }
        avail[best] = 0;
        order[k] = best;
    }
}

// Two orderings are the same, or one is the other reversed (0x10082bb0).
bool SameOrder(int n, const int *a, const int *b)
{
    int i = 0;
    if (n > 0) {
        while (a[i] == b[i]) {
            if (++i >= n)
                return i == n;
        }
        i = 0;
        while (i < n && a[i] == b[n - 1 - i])
            ++i;
    }
    return i == n;
}

// Cluster fits ---------------------------------------------------------------------------------

// The cluster sums of the best split (Fit*), shared by both fits.
void ClusterSums(int n, const int *best, const double *weights, const double (*sums)[3], double *s, double *cw,
                 double *q, int *cnt)
{
    s[0] = s[1] = s[2] = 0.0;
    *cw = 0.0;
    *q = 0.0;
    *cnt = 0;
    int pos = 0;
    for (int t = 0; t < 4; ++t) {
        const int end = best[t];
        if (pos < end) {
            const double T = t;
            for (; pos < end; ++pos) {
                const double tw = R(T * weights[pos]);
                *cw = R(*cw + tw);
                *q = R(*q + R(T * tw));
                for (int c = 0; c < 3; ++c)
                    s[c] = R(s[c] + R(T * sums[pos][c]));
            }
        }
        *cnt += end != n;
    }
}

// The best split of the ordered colours into up to four clusters by least squares (0x10082c20);
// writes the two endpoints (centred), the new axis and the number of clusters, returns the error.
double FitClusters(int *mode, int n, const double (*sums)[3], const double *weights, const int *order,
                   double (*endpoints)[3], double *axis)
{
    for (int p = 0; p < n; ++p) {
        for (int c = 0; c < 3; ++c)
            g.ordSum[p][c] = sums[order[p]][c];
        g.ordW[p] = weights[order[p]];
    }
    double(&S)[3][3] = g.fitS;          // [level 1, level 2, current][channel]
    double(&C)[3] = g.fitC;
    double(&Q)[3] = g.fitQ;
    std::memset(S, 0, sizeof S);
    C[0] = C[1] = C[2] = 0.0;
    Q[0] = Q[1] = Q[2] = 0.0;

    int best[4] = {n, n, n, n};
    for (int i = n;;) {
        for (int j = n;;) {
            for (int k = n;;) {
                const double num = R(R(-R(S[2][0] * S[2][0]) - R(S[2][1] * S[2][1])) - R(S[2][2] * S[2][2]));
                const double t = R(g.fitBestNum * Q[2]);
                const double u = R(g_fitBestDen * num);
                if (Le(u, t)) {
                    g.fitBestNum = num;
                    g_fitBestDen = Q[2];
                    best[0] = i;
                    best[1] = j;
                    best[2] = k;
                }
                --k;
                if (k < j || *mode < 3)
                    break;
                for (int c = 0; c < 3; ++c)
                    S[2][c] = R(S[2][c] + g.ordSum[k][c]);
                Q[2] = R(R(R(R(5.0 - R(C[2] + C[2])) - g.ordW[k]) * g.ordW[k]) + Q[2]);
                C[2] = R(C[2] + g.ordW[k]);
            }
            --j;
            if (j < i)
                break;
            for (int c = 0; c < 3; ++c) {
                S[1][c] = R(S[1][c] + g.ordSum[j][c]);
                S[2][c] = S[1][c];
            }
            Q[1] = R(R(R(R(3.0 - R(C[1] + C[1])) - g.ordW[j]) * g.ordW[j]) + Q[1]);
            Q[2] = Q[1];
            C[1] = R(C[1] + g.ordW[j]);
            C[2] = C[1];
        }
        --i;
        if (i < 1)
            break;
        for (int c = 0; c < 3; ++c) {
            S[0][c] = R(S[0][c] + g.ordSum[i][c]);
            S[1][c] = S[2][c] = S[0][c];
        }
        Q[0] = R(R(R(R(1.0 - R(C[0] + C[0])) - g.ordW[i]) * g.ordW[i]) + Q[0]);
        Q[1] = Q[2] = Q[0];
        C[0] = R(C[0] + g.ordW[i]);
        C[1] = C[2] = C[0];
    }

    best[3] = n;
    const int distinct = (best[1] != best[2]) + (best[0] != best[1]) + (best[2] != n);
    if (distinct == 1)
        best[1] = best[0];
    double q;
    int cnt;
    ClusterSums(n, best, g.ordW, g.ordSum, S[2], &C[2], &q, &cnt);
    Q[2] = R(q - R(C[2] * C[2]));
    const double negC = -C[2];
    const double cntC = R(static_cast<double>(cnt) - C[2]);
    const double num = R(R(-R(S[2][0] * S[2][0]) - R(S[2][1] * S[2][1])) - R(S[2][2] * S[2][2]));
    for (int c = 0; c < 3; ++c) {
        endpoints[0][c] = R(R(negC * S[2][c]) / Q[2]);
        endpoints[1][c] = R(R(cntC * S[2][c]) / Q[2]);
        axis[c] = S[2][c];
    }
    *mode = distinct == 1 ? 1 : cnt;
    return R(num / Q[2]);
}

// The part of the bounded fit's error that depends on one channel, and its endpoints.
struct S_boundedTerms {
    double negC, TmC, Q, a, b, c, d, e;
};

// FitClusters with the endpoints clamped to the channel bounds (bounds[0..2] low, [3..5] high;
// 0x10083310).
double FitClustersBounded(int *mode, int n, const double (*sums)[3], const double *weights, const int *order,
                          const double *bounds, double (*endpoints)[3], double *axis)
{
    for (int p = 0; p < n; ++p) {
        for (int c = 0; c < 3; ++c)
            g.ordSum2[p][c] = sums[order[p]][c];
        g.ordW2[p] = weights[order[p]];
    }
    double(&S)[3][3] = g.bfitS;
    double(&C)[3] = g.bfitC;
    double(&Q)[3] = g.bfitQ;
    std::memset(S, 0, sizeof S);
    C[0] = C[1] = C[2] = 0.0;
    Q[0] = Q[1] = Q[2] = 0.0;

    int level = 0;
    int best[4] = {n, n, n, n};
    for (int i = n;;) {
        for (int j = n;;) {
            for (int k = n;;) {
                const double negC = -C[2];
                const double T = level;
                const double TmC = R(T - C[2]);
                const double q = Q[2];
                const double a = R(R(negC * negC) + q);
                const double b = R(R(TmC * negC) + q);
                const double c = R(R(TmC * TmC) + q);
                const double d = R(c * a);
                const double e = R(R(R(R(T * T) * q) * d) * d);
                double acc = 0.0;
                for (int ch = 0; ch < 3; ++ch) {
                    const double s = S[2][ch];
                    const double Qlo = R(q * bounds[ch]);
                    const double Qhi = R(q * bounds[ch + 3]);
                    const double nS = R(negC * s);
                    const double tS = R(TmC * s);
                    const double u = R(Qlo - nS);
                    const double v = R(Qhi - nS);
                    const double w1 = R(R(Qlo - tS) * a);
                    const double w2 = R(R(Qhi - tS) * a);
                    const double p = R(R(R(R(v - std::fabs(v)) + R(std::fabs(u) + u)) * 0.5) * b);
                    const double ud = R(u * d);
                    const double vd = R(v * d);
                    const double r = R(R(R(w2 - std::fabs(R(w2 - p))) + R(std::fabs(R(w1 - p)) + w1)) * 0.5);
                    const double x1 = R(r * b);
                    const double rc = R(r * c);
                    const double z = R(R(R(vd - std::fabs(R(vd - x1))) + R(std::fabs(R(ud - x1)) + ud)) * 0.5);
                    const double diff = R(z - rc);
                    const double gg = R(R(rc * negC) - R(z * TmC));
                    const double h = R(R(q * diff) * diff);
                    acc = R(acc + R(R(h + R(gg * gg)) - R(R(s * s) * e)));
                }
                const double eq = R(q * e);
                const double t = R(g.bfitBestNum * eq);
                const double u = R(g_bfitBestDen * acc);
                if (Le(u, t)) {
                    g.bfitBestNum = acc;
                    g_bfitBestDen = eq;
                    best[0] = i;
                    best[1] = j;
                    best[2] = k;
                }
                --k;
                if (k < j || *mode < 3)
                    break;
                level = 3;
                for (int ch = 0; ch < 3; ++ch)
                    S[2][ch] = R(S[2][ch] + g.ordSum2[k][ch]);
                Q[2] = R(R(R(R(5.0 - R(C[2] + C[2])) - g.ordW2[k]) * g.ordW2[k]) + Q[2]);
                C[2] = R(C[2] + g.ordW2[k]);
            }
            --j;
            if (j < i)
                break;
            level = 2;
            for (int ch = 0; ch < 3; ++ch) {
                S[1][ch] = R(S[1][ch] + g.ordSum2[j][ch]);
                S[2][ch] = S[1][ch];
            }
            Q[1] = R(R(R(R(3.0 - R(C[1] + C[1])) - g.ordW2[j]) * g.ordW2[j]) + Q[1]);
            Q[2] = Q[1];
            C[1] = R(C[1] + g.ordW2[j]);
            C[2] = C[1];
        }
        --i;
        if (i < 1)
            break;
        level = 1;
        for (int ch = 0; ch < 3; ++ch) {
            S[0][ch] = R(S[0][ch] + g.ordSum2[i][ch]);
            S[1][ch] = S[2][ch] = S[0][ch];
        }
        Q[0] = R(R(R(R(1.0 - R(C[0] + C[0])) - g.ordW2[i]) * g.ordW2[i]) + Q[0]);
        Q[1] = Q[2] = Q[0];
        C[0] = R(C[0] + g.ordW2[i]);
        C[1] = C[2] = C[0];
    }

    best[3] = n;
    const int distinct = (best[1] != best[2]) + (best[2] != n) + (best[0] != best[1]);
    if (distinct == 1)
        best[1] = best[0];
    double qs;
    int cnt;
    ClusterSums(n, best, g.ordW2, g.ordSum2, S[2], &C[2], &qs, &cnt);
    const double q = R(qs - R(C[2] * C[2]));
    Q[2] = q;
    const double negC = -C[2];
    const double T = cnt;
    const double TmC = R(T - C[2]);
    const double a = R(R(negC * negC) + q);
    const double b = R(R(TmC * negC) + q);
    const double c = R(R(TmC * TmC) + q);
    const double d = R(c * a);
    const double e = R(R(R(R(T * T) * d) * d) * q);
    double acc = 0.0;
    for (int ch = 0; ch < 3; ++ch) {
        const double s = S[2][ch];
        const double Qlo = R(q * bounds[ch]);
        const double Qhi = R(q * bounds[ch + 3]);
        const double nS = R(negC * s);
        const double tS = R(TmC * s);
        const double u = R(Qlo - nS);
        const double v = R(Qhi - nS);
        const double w1 = R(R(Qlo - tS) * a);
        const double w2 = R(R(Qhi - tS) * a);
        const double p = R(R(R(R(v - std::fabs(v)) + R(std::fabs(u) + u)) * 0.5) * b);
        const double ud = R(u * d);
        const double vd = R(v * d);
        const double r = R(R(R(w2 - std::fabs(R(w2 - p))) + R(std::fabs(R(w1 - p)) + w1)) * 0.5);
        const double x1 = R(r * b);
        const double rc = R(r * c);
        const double z = R(R(R(vd - std::fabs(R(vd - x1))) + R(std::fabs(R(ud - x1)) + ud)) * 0.5);
        const double diff = R(z - rc);
        const double gg = R(R(rc * negC) - R(z * TmC));
        const double h = R(R(diff * diff) * q);
        acc = R(acc + R(R(h + R(gg * gg)) - R(R(s * s) * e)));
        endpoints[0][ch] = R(R(R(z / d) + nS) / q);
        endpoints[1][ch] = R(R(R(TmC * s) + R(rc / d)) / q);
        axis[ch] = s;
    }
    *mode = distinct == 1 ? 1 : cnt;
    return R(acc / R(e * q));
}

// Quantisation -----------------------------------------------------------------------------------

// The endpoints rounded to 5:6:5 both ways per channel; the best of the 64 combinations for the
// colours; the palette indices (0x10084470). `mode` 3 or 4 colours.
double Quantize(int mode, int n, const double (*points)[3], const double *pointWeights, const double *chanW,
                const double (*ep)[3], int (*epOut)[3], int *indices)
{
    // Per channel four endpoint choices of four entries (low/high of each endpoint, then the
    // interpolated colours).
    int A[3][4][4] = {};
    for (int c = 0; c < 3; ++c) {
        const int bits = kBits[c];
        const int step = 1 << (8 - bits);
        const int mask = 0x100 - step;
        for (int e = 0; e < 2; ++e) {
            if (Eq(chanW[c], 0.0))
                g.quantTmp = 0.0;
            else
                g.quantTmp = R(R(ep[e][c] / chanW[c]) * 255.0);
            int q = Ftol(std::floor(g.quantTmp));
            if (q < 0)
                q = 0;
            else if (q >= 256)
                q = mask;
            q &= mask;
            if (!Le(static_cast<double>((q >> bits) + q), g.quantTmp) && q - step >= 0)
                q -= step;
            const int lo = (q >> bits) + q;
            if (q + step < 256)
                q += step;
            const int hi = q + (q >> bits);
            if (e == 0) {
                A[c][0][0] = A[c][1][0] = lo;
                A[c][2][0] = A[c][3][0] = hi;
            } else {
                A[c][0][1] = A[c][2][1] = lo;
                A[c][1][1] = A[c][3][1] = hi;
            }
        }
    }

    int best = -1;
    const int stride = mode == 3 ? 3 : 4;
    if (mode == 3 || mode == 4) {
        for (int c = 0; c < 3; ++c) {
            for (int gi = 0; gi < 4; ++gi) {
                int *G = A[c][gi];
                if (mode == 3) {
                    G[2] = (G[1] + G[0]) / 2;
                } else {
                    G[2] = (G[1] + G[0] * 2 + 1) / 3;
                    G[3] = (G[0] + G[1] * 2 + 1) / 3;
                }
                double *pal = g.palette[c][gi];
                for (int k = 0; k < mode; ++k)
                    pal[k] = R(R(static_cast<double>(G[k]) * chanW[c]) * kInv255);
                double *tab = &g.errTab[(c * 4 + gi) * 64];
                for (int p = 0; p < n; ++p) {
                    const double x = points[p][c];
                    const double w = pointWeights[p];
                    for (int k = 0; k < mode; ++k) {
                        const double d = R(x - pal[k]);
                        tab[p * stride + k] = (mode == 3 && k < 2) ? R(R(d * w) * d) : R(R(d * d) * w);
                    }
                }
            }
        }
        g.bestQuantErr =
            R(R(R(R(chanW[0] * chanW[0]) + R(chanW[1] * chanW[1])) + R(chanW[2] * chanW[2])) * 32.0);
        for (int combo = 0; combo < 64; ++combo) {
            const double *t0 = &g.errTab[(0 * 4 + (combo & 3)) * 64];
            const double *t1 = &g.errTab[(1 * 4 + ((combo >> 2) & 3)) * 64];
            const double *t2 = &g.errTab[(2 * 4 + (combo >> 4)) * 64];
            double sum = 0.0;
            if (static_cast<unsigned>(n - 1) <= 15) {
                for (int p = n - 1; p >= 0; --p) {
                    const int o = p * stride;
                    double e[4];
                    for (int k = 0; k < mode; ++k)
                        e[k] = R(R(t2[o + k] + t1[o + k]) + t0[o + k]);
                    const double m01 = R(R(e[1] + e[0]) - std::fabs(R(e[0] - e[1])));
                    double m;
                    if (mode == 3) {
                        const double e2 = R(e[2] + e[2]);
                        m = R(R(m01 + e2) - std::fabs(R(e2 - m01)));
                    } else {
                        const double m23 = R(R(e[3] + e[2]) - std::fabs(R(e[2] - e[3])));
                        m = R(R(m01 + m23) - std::fabs(R(m23 - m01)));
                    }
                    sum = p == n - 1 ? m : R(sum + m);
                }
            }
            if (Lt(sum, g.bestQuantErr)) {
                g.bestQuantErr = sum;
                best = combo;
            }
        }
    }

    for (int c = 0; c < 3; ++c) {
        const int *G = A[c][(best >> (2 * c)) & 3];
        for (int e = 0; e < 2; ++e)
            epOut[e][c] = G[e] >> (8 - kBits[c]);
    }
    const double *P0 = g.palette[0][best & 3];
    const double *P1 = g.palette[1][(best >> 2) & 3];
    const double *P2 = g.palette[2][(best >> 4) & 3];
    double total = 0.0;
    for (int p = 0; p < n; ++p) {
        int bestK = -1;
        double minErr = R(R(R(R(chanW[0] * chanW[0]) + R(chanW[1] * chanW[1])) + R(chanW[2] * chanW[2])) * 2.0);
        for (int k = 0; k < mode; ++k) {
            const double dz = R(points[p][2] - P2[k]);
            const double dy = R(points[p][1] - P1[k]);
            const double dx = R(points[p][0] - P0[k]);
            const double err = R(R(R(R(dx * dx) + R(dz * dz)) + R(dy * dy)) * pointWeights[p]);
            if (Lt(err, minErr)) {
                minErr = err;
                bestK = k;
            }
        }
        indices[p] = bestK;
        total = R(total + minErr);
    }
    return total;
}

// Quantize, and with extended endpoints for the other colour counts; keeps the better
// (0x10084290). resultMode: in the fit's mode, out 3 or 4.
double FinalQuantize(int *resultMode, int mode, int n, const double (*points)[3], const double *pointWeights,
                     const double *chanW, const double (*ep)[3], int (*epOut)[3], int *indices)
{
    int kinds[2];
    int candEp[2][2][3];
    int candIdx[2][16];
    kinds[0] = *resultMode == 4 ? 1 : 0;
    g.quantErr[0] = Quantize(kinds[0] + 3, n, points, pointWeights, chanW, ep, candEp[0], candIdx[0]);
    int cur = 1;
    const int variants = mode - 2;
    *resultMode -= 2;
    for (int v = 0; v < variants; ++v) {
        const int count = kVariantCount[v * 3 + *resultMode];
        for (int s = 0; s < count; ++s) {
            for (int e = 0; e < 2; ++e) {
                const double *k = kXform[((v * 2 + *resultMode) * 6 + s) * 2 + e];
                for (int c = 0; c < 3; ++c)
                    g.xformEp[e][c] = R(R(k[1] * ep[1][c]) + R(k[0] * ep[0][c]));
            }
            kinds[cur] = v;
            const double err = Quantize(v + 3, n, points, pointWeights, chanW, g.xformEp, candEp[cur], candIdx[cur]);
            g.quantErr[cur] = err;
            if (Lt(err, g.quantErr[1 - cur]))
                cur = 1 - cur;
        }
    }
    const int best = 1 - cur;
    if (n > 0)
        std::memcpy(indices, candIdx[best], sizeof(int) * static_cast<size_t>(n));
    std::memcpy(epOut, candEp[best], sizeof candEp[best]);
    *resultMode = kinds[best] + 3;
    return g.quantErr[best];
}

// The colour block ---------------------------------------------------------------------------

void EncodeBlock(S_block *b)
{
    if (!b || !b->count)
        return;
    const int n = b->count;
    const double nD = n;
    g.invCount = R(1.0 / nD);
    for (int c = 0; c < 3; ++c)
        g.weights[c] = Lt(b->weights[c], 0.0) ? 0.0 : b->weights[c];
    bool uniform = true;
    for (int c = 0; c < 3; ++c) {
        g.mean[c] = 0.0;
        for (int i = 0; i < n; ++i) {
            uniform &= Lt(std::fabs(R(b->colors[i][c] - b->colors[0][c])), 0.00048828125);
            g.scaled[i][c] = R(g.weights[c] * b->colors[i][c]);
            g.mean[c] = R(g.scaled[i][c] + g.mean[c]);
        }
    }
    if (uniform) {
        int mode = b->mode;
        EncodeSingleColor(&mode, n, g.scaled, g.weights, b->endpoints, b->indices);
        b->resultMode = mode;
        return;
    }
    for (int c = 0; c < 3; ++c) {
        const double m = R(g.mean[c] / nD);
        g.mean[c] = m;
        for (int i = 0; i < n; ++i)
            g.centered[i][c] = R(g.scaled[i][c] - m);
        g.lo[c] = -m;
        g.hi[c] = R(g.weights[c] - m);
    }

    // The distinct colours, with weights and centred sums.
    int unique = 0;
    int map[16];
    for (int i = 0; i < n; ++i) {
        int j = 0;
        for (; j < unique; ++j) {
            if (Eq(g.unique[j][0], g.scaled[i][0]) && Eq(g.unique[j][1], g.scaled[i][1]) &&
                Eq(g.unique[j][2], g.scaled[i][2]))
                break;
        }
        if (j == unique) {
            g.uniqueWeight[j] = 1.0;
            ++unique;
            for (int c = 0; c < 3; ++c) {
                g.unique[j][c] = g.scaled[i][c];
                g.uniqueSum[j][c] = g.centered[i][c];
            }
        } else {
            for (int c = 0; c < 3; ++c)
                g.uniqueSum[j][c] = R(g.centered[i][c] + g.uniqueSum[j][c]);
            g.uniqueWeight[j] = R(g.uniqueWeight[j] + 1.0);
        }
        map[i] = j;
    }
    for (int j = 0; j < unique; ++j) {
        g.uniqueWeight[j] = R(g.invCount * g.uniqueWeight[j]);
        for (int c = 0; c < 3; ++c)
            g.uniqueSum[j][c] = R(g.invCount * g.uniqueSum[j][c]);
    }

    int localIdx[16];
    if (unique == 2) {
        b->resultMode = unique;
        FinalQuantize(&b->resultMode, b->mode, unique, g.unique, g.uniqueWeight, g.weights, g.unique, b->endpoints,
                      localIdx);
        for (int i = 0; i < n; ++i)
            b->indices[i] = localIdx[map[i]];
        return;
    }

    // Two passes of "fit the clusters along the axis, sort along the fit's axis" until an ordering
    // repeats: from the principal axis, then from the farthest pair. The orderings go round a ring
    // of 16.
    int orderings[16][16];
    int bestOrder[2][16];
    int modes[2];
    PrincipalAxis(n, g.centered, g.axis);
    SortByProjection(unique, g.unique, g.axis, orderings[0], 0);
    int slot = 0;
    bool wrapped = false;
    bool found = true;                  // (the original's register starts non-zero)
    int lastEval = 0;
    auto sortAndCheck = [&](bool checkBudget, int budget) {
        for (int tie = 0;;) {
            SortByProjection(unique, g.unique, g.axis, orderings[slot], tie);
            bool hit;
            if (slot - 1 >= 0) {
                found = false;
                for (int s = slot - 1; s >= 0 && !found; --s)
                    found = SameOrder(unique, orderings[s], orderings[slot]);
                hit = found;
            } else {
                hit = found;
            }
            if (!hit && wrapped) {
                for (int s = slot + 1; s < 16 && !found; ++s)
                    found = SameOrder(unique, orderings[s], orderings[slot]);
                hit = found;
            }
            if (!hit)
                return;
            if (checkBudget && budget == 0)
                return;
            if (++tie >= 2)
                return;
        }
    };
    for (int pass = 0; pass < 2; ++pass) {
        for (int budget = 16;; --budget) {
            modes[pass] = b->mode - 1;
            if (pass + budget - 1 == 16) {
                FarthestPairAxis(unique, g.unique, g.axis);
            } else {
                g.passErr[pass] =
                    FitClusters(&modes[pass], unique, g.uniqueSum, g.uniqueWeight, orderings[slot], g.passEp[pass], g.axis);
                lastEval = pass;
            }
            slot = (slot + 1) & 15;
            wrapped |= slot == 0;
            sortAndCheck(true, budget);
            if (budget == 0 || found)
                break;
        }
        std::memcpy(bestOrder[pass], orderings[slot], sizeof(int) * static_cast<size_t>(unique));
    }

    int best = (lastEval == 1 && !Lt(g.passErr[0], g.passErr[1])) ? 1 : 0;
    bool out = false;
    for (int k = 0; k < 2; ++k) {
        for (int c = 0; c < 3; ++c) {
            if (!out) {
                const double e = g.passEp[best][k][c];
                out = Lt(e, g.lo[c]) || !Le(e, g.hi[c]);
            }
        }
    }
    if (out) {
        // An endpoint left the colour range: fit again with clamping, from the best ordering.
        std::memcpy(orderings[0], bestOrder[best], sizeof(int) * static_cast<size_t>(unique));
        slot = 0;
        wrapped = false;
        for (int budget = 8;; --budget) {
            modes[best] = b->mode - 1;
            g.passErr[best] = FitClustersBounded(&modes[best], unique, g.uniqueSum, g.uniqueWeight, orderings[slot],
                                                 &g.lo[0], g.passEp[best], g.axis);
            slot = (slot + 1) & 15;
            wrapped |= slot == 0;
            sortAndCheck(false, budget);
            if (budget == 0 || found)
                break;
        }
    }

    // Un-centred endpoints into the other pass's slot, then the 5:6:5 quantisation.
    for (int k = 0; k < 2; ++k) {
        for (int c = 0; c < 3; ++c)
            g.passEp[1 - best][k][c] = R(g.passEp[best][k][c] + g.mean[c]);
    }
    b->resultMode = modes[best] + 1;
    FinalQuantize(&b->resultMode, b->mode, unique, g.unique, g.uniqueWeight, g.weights, g.passEp[1 - best],
                  b->endpoints, localIdx);
    for (int i = 0; i < n; ++i)
        b->indices[i] = localIdx[map[i]];
}

// The alpha block ----------------------------------------------------------------------------

// 1-D least-squares fit of `levels` evenly spaced values to `n` alphas (0x10085a80): the endpoints
// (0 - 255) and each alpha's level; returns the error.
double FitAlpha(int levels, int n, const double *v, int *ends, int *idx)
{
    int budget = 64;
    if (n == 0)
        return 0.0;
    g.alphaN = n;
    g.alphaInvN = R(1.0 / g.alphaN);
    g.alphaHi = g.alphaLo = g.alphaMean = v[0];
    idx[0] = 0;
    for (int i = 1; i < n; ++i) {
        if (!Le(g.alphaLo, v[i]))
            g.alphaLo = v[i];
        if (Lt(g.alphaHi, v[i]))
            g.alphaHi = v[i];
        g.alphaMean = R(g.alphaMean + v[i]);
        idx[0] = 0;                     // (the original clears only the first index here)
    }
    g.alphaMean = R(g.alphaMean * g.alphaInvN);
    for (int i = 0; i < n; ++i)
        g.alphaCentered[i] = R(v[i] - g.alphaMean);
    g.alphaLo = R(g.alphaLo - g.alphaMean);
    g.alphaHi = R(g.alphaHi - g.alphaMean);

    const int lm1 = levels - 1;
    int hist[8] = {};
    for (;;) {
        bool converged = true;
        if (lm1 > 0) {
            const double step = R(R(g.alphaHi - g.alphaLo) / static_cast<double>(lm1));
            for (int k = 0; k < lm1; ++k) {
                hist[k] = 0;
                g.alphaThresh[k] = R(R(R(static_cast<double>(k) + 0.5) * step) + g.alphaLo);
            }
        }
        hist[levels - 1] = 0;
        for (int i = 0; i < n; ++i) {
            int k = 0;
            while (k < lm1 && !Lt(g.alphaCentered[i], g.alphaThresh[k]))
                ++k;
            const int old = idx[i];
            idx[i] = k;
            ++hist[k];
            converged &= old == k;
        }
        int nonEmpty = 0;
        for (int k = 0; k < levels; ++k)
            nonEmpty += hist[k] != 0;
        if (levels > 0 && nonEmpty == 1)
            break;

        g.alphaSq = 0.0;
        g.alphaSk = 0.0;
        g.alphaSkc = 0.0;
        for (int i = 0; i < n; ++i) {
            const double k = idx[i];
            g.alphaSkc = R(R(k * g.alphaCentered[i]) + g.alphaSkc);
            g.alphaSk = R(k + g.alphaSk);
            g.alphaSq = R(R(k * k) + g.alphaSq);
        }
        g.alphaSk = R(g.alphaSk * g.alphaInvN);
        g.alphaSq = R(g.alphaSq - R(R(g.alphaSk * g.alphaSk) * g.alphaN));
        const double negK = -g.alphaSk;
        const double lm1D = R(static_cast<double>(levels) - 1.0);
        const double LK = R(lm1D - g.alphaSk);
        const double sq = g.alphaSq;
        const double a = R(R(R(g.alphaN * negK) * negK) + sq);
        const double nL = R(g.alphaN * LK);
        const double b = R(R(nL * negK) + sq);
        const double c = R(R(nL * LK) + sq);
        const double d = R(c * a);
        const double L2 = R(lm1D * lm1D);
        const double e = R(R(R(L2 * sq) * d) * d);
        const double nsm = -R(sq * g.alphaMean);
        const double omm = R(1.0 - g.alphaMean);
        const double ommSq = R(omm * sq);
        const double nKc = R(g.alphaSkc * negK);
        const double LKc = R(g.alphaSkc * LK);
        const double u = R(nsm - nKc);
        const double vv = R(ommSq - nKc);
        const double w1 = R(R(nsm - LKc) * a);
        const double w2 = R(R(ommSq - LKc) * a);
        const double p = R(R(R(R(vv - std::fabs(vv)) + R(std::fabs(u) + u)) * 0.5) * b);
        const double ud = R(u * d);
        g.alphaPrevLo = g.alphaLo;
        g.alphaPrevHi = g.alphaHi;
        const double vd = R(vv * d);
        const double r = R(R(R(w2 - std::fabs(R(w2 - p))) + R(std::fabs(R(w1 - p)) + w1)) * 0.5);
        const double x1 = R(r * b);
        const double rc = R(r * c);
        const double z = R(R(R(vd - std::fabs(R(vd - x1))) + R(std::fabs(R(ud - x1)) + ud)) * 0.5);
        const double diff = R(z - rc);
        const double gg = R(R(negK * rc) - R(z * LK));
        const double h = R(R(sq * diff) * diff);
        const double ng = R(R(g.alphaN * gg) * gg);
        g.alphaErrNum = R(R(h + ng) - R(R(g.alphaSkc * g.alphaSkc) * e));
        g.alphaErrDen = R(sq * e);
        g.alphaLo = R(R(R(z / d) + nKc) / sq);
        g.alphaHi = R(R(R(rc / d) + LKc) / sq);
        if (budget-- == 0)
            break;
        if (!converged)
            continue;

        // Converged: try moving the extreme alpha of a level to its neighbour.
        int first[8], last[8];
        for (int k = 0; k < levels; ++k)
            first[k] = -1;
        for (int i = 0; i < n; ++i) {
            const int k = idx[i];
            if (first[k] == -1) {
                last[k] = i;
                first[k] = i;
            } else {
                if (!Le(g.alphaCentered[first[k]], g.alphaCentered[i]))
                    first[k] = i;
                if (Lt(g.alphaCentered[last[k]], g.alphaCentered[i]))
                    last[k] = i;
            }
        }
        const double sq1 = R(sq + 1.0);
        bool improved = false;
        for (int sgn = 0; sgn < 2 && !improved; ++sgn) {
            const double dir = sgn ? -1.0 : 1.0;
            const double K1 = R(R(g.alphaInvN * dir) + g.alphaSk);
            const double q1 = R(R(sq1 - R(R(g.alphaSk * dir) + R(g.alphaSk * dir))) - g.alphaInvN);
            for (int k = sgn; k < lm1 + sgn; ++k) {
                const int count = hist[k];
                if (count == 0 || (count <= 1 && nonEmpty <= 2))
                    continue;
                const double cv = g.alphaCentered[sgn ? first[k] : last[k]];
                const double skc1 = R(R(dir * cv) + g.alphaSkc);
                const double kd = R(static_cast<double>(k) * dir);
                const double sqk = R(R(kd + kd) + q1);
                const double negK1 = -K1;
                const double LK1 = R(lm1D - K1);
                const double a1 = R(R(R(negK1 * negK1) * g.alphaN) + sqk);
                const double b1 = R(R(R(LK1 * negK1) * g.alphaN) + sqk);
                const double c1 = R(R(R(LK1 * LK1) * g.alphaN) + sqk);
                const double d1 = R(c1 * a1);
                const double e1 = R(R(R(L2 * d1) * d1) * sqk);
                const double nsm1 = -R(sqk * g.alphaMean);
                const double ommSq1 = R(omm * sqk);
                const double nKc1 = R(negK1 * skc1);
                const double LKc1 = R(LK1 * skc1);
                const double u1 = R(nsm1 - nKc1);
                const double v1 = R(ommSq1 - nKc1);
                const double w11 = R(R(nsm1 - LKc1) * a1);
                const double w21 = R(R(ommSq1 - LKc1) * a1);
                const double p1 = R(R(R(R(v1 - std::fabs(v1)) + R(std::fabs(u1) + u1)) * 0.5) * b1);
                const double ud1 = R(u1 * d1);
                const double vd1 = R(v1 * d1);
                const double r1 = R(R(R(w21 - std::fabs(R(w21 - p1))) + R(std::fabs(R(w11 - p1)) + w11)) * 0.5);
                const double x11 = R(r1 * b1);
                const double rc1 = R(r1 * c1);
                const double z1 = R(R(R(vd1 - std::fabs(R(vd1 - x11))) + R(std::fabs(R(ud1 - x11)) + ud1)) * 0.5);
                const double diff1 = R(z1 - rc1);
                const double gg1 = R(R(rc1 * negK1) - R(z1 * LK1));
                const double s1 = R(R(R(gg1 * gg1) * g.alphaN) + R(R(diff1 * diff1) * sqk));
                const double err1 = R(s1 - R(R(e1 * skc1) * skc1));
                const double den1 = R(e1 * sqk);
                const double t1 = R(den1 * g.alphaErrNum);
                const double t2 = R(err1 * g.alphaErrDen);
                if (Lt(t2, t1)) {
                    g.alphaLo = R(R(R(z1 / d1) + R(negK1 * skc1)) / sqk);
                    g.alphaErrDen = den1;
                    g.alphaHi = R(R(R(rc1 / d1) + R(LK1 * skc1)) / sqk);
                    g.alphaErrNum = err1;
                    improved = true;
                    break;
                }
            }
        }
        if (!improved)
            break;
    }

    int lv[8];
    for (int e = 0; e < 2; ++e) {
        int q = Ftol(std::floor(R(R(R(g.alphaMean + (e ? g.alphaHi : g.alphaLo)) * 255.0) + 0.5)));
        if (q < 0)
            q = 0;
        else if (q > 255)
            q = 255;
        lv[e] = q;
    }
    if (levels > 2) {
        const int half = (levels - 2) / 2;
        for (int j = 2, w0 = levels - 2; j < levels; ++j, --w0)
            lv[j] = (w0 * lv[0] + (half + (j - 1) * lv[1])) / lm1;
    }
    ends[0] = lv[0];
    ends[1] = lv[1];
    g.alphaErr = 0.0;
    for (int i = 0; i < n; ++i) {
        int best = 0;
        if (levels > 1) {
            const double x = R(v[i] * 255.0);
            for (int j = 1; j < levels; ++j) {
                if (!Le(std::fabs(R(lv[best] - x)), std::fabs(R(lv[j] - x))))
                    best = j;
            }
        }
        idx[i] = best;
        const double d = R(static_cast<double>(lv[best]) - R(v[i] * 255.0));
        g.alphaErr = R(R(d * d) + g.alphaErr);
    }
    return g.alphaErr;
}

// The interpolated alpha block (0x10085740): fits five sets (all alphas with 8 or 6 levels;
// without the zeros, without the ones, without both: 6 levels, the dropped values coded as the
// mode's explicit 0 / 255) and keeps the best.
void EncodeAlphaBlock(S_alphaBlock *ab)
{
    static const int kModes[5] = {8, 6, 6, 6, 6};
    static const int kZeroCode[5] = {0, 0, 1, 0, 1};
    static const int kOneCode[5] = {0, 0, 0, 1, 1};
    int counts[5] = {};
    for (int i = 0; i < ab->count; ++i) {
        const double a = ab->alpha[i];
        g.alphaSets[0][counts[0]++] = a;
        g.alphaSets[1][counts[1]++] = a;
        if (!Eq(a, 0.0))
            g.alphaSets[2][counts[2]++] = a;
        if (!Eq(a, 1.0))
            g.alphaSets[3][counts[3]++] = a;
        if (!Eq(a, 0.0) && !Eq(a, 1.0))
            g.alphaSets[4][counts[4]++] = a;
    }
    const bool zeroNeeded = ab->needZero && counts[0] != counts[2];
    const bool oneNeeded = ab->needOne && counts[0] != counts[3];
    int enabled[5] = {1, 1, counts[2] != counts[1], counts[3] != counts[1], counts[4] != counts[1]};
    int ends[5][2] = {};
    int idx[5][16] = {};
    for (int s = 0; s < 5; ++s) {
        if (!enabled[s])
            continue;
        g.alphaSetErr[s] = FitAlpha(kModes[s], counts[s], g.alphaSets[s], ends[s], idx[s]);
        enabled[s] = 1;
        // (The "one" check compares the endpoints with 1, not 255; needOne is never set here.)
        if (zeroNeeded && !kZeroCode[s])
            enabled[s] &= ends[s][0] == 0 || ends[s][1] == 0;
        if (oneNeeded && !kOneCode[s])
            enabled[s] &= ends[s][0] == 1 || ends[s][1] == 1;
    }
    int best = 0;
    while (best < 4 && !enabled[best])
        ++best;
    for (int s = best + 1; s < 5; ++s) {
        if (enabled[s] && Lt(g.alphaSetErr[s], g.alphaSetErr[best]))
            best = s;
    }
    ab->mode = kModes[best];
    ab->alpha0 = ends[best][0];
    ab->alpha1 = ends[best][1];
    int j = 0;
    for (int i = 0; i < ab->count; ++i) {
        const double a = ab->alpha[i];
        if (Eq(a, 0.0) && kZeroCode[best])
            ab->indices[i] = 6;
        else if (Eq(a, 1.0) && kOneCode[best])
            ab->indices[i] = 7;
        else
            ab->indices[i] = idx[best][j++];
    }
}

// Pixels ---------------------------------------------------------------------------------------

uint32_t ReadPixel(const uint8_t *p, uint32_t bytes)
{
    // (The original reads 4 bytes whatever the pixel size; the masks keep the same bits.)
    uint32_t v = 0;
    std::memcpy(&v, p, bytes < 4 ? bytes : 4);
    return v;
}

int LowBit(uint32_t mask)
{
    // (The original loops forever on a zero mask.)
    int n = 0;
    while (!(mask & 1) && n < 32) {
        mask >>= 1;
        ++n;
    }
    return n;
}

}  // namespace

double DebugFitAlpha(int levels, int n, const double *values, int *ends, int *indices)
{
    return FitAlpha(levels, n, values, ends, indices);
}

void ResetState()
{
    std::memset(&g, 0, sizeof g);
    g_fitBestDen = 1.0;
    g_bfitBestDen = 1.0;
}

const void *DebugWorkArea(size_t *size, const double *bestDens[2])
{
    *size = sizeof g;
    bestDens[0] = &g_fitBestDen;
    bestDens[1] = &g_bfitBestDen;
    return &g;
}

int Encode(const S_surfaceDesc *src, const uint8_t *palette, S_surfaceDesc *dst, void *bits, uint32_t type,
           const float *weights, ProgressFn progress, void *context)
{
    dst->flags = 0x1806;
    dst->width = src->width;
    dst->height = src->height;
    dst->surface = bits;
    dst->pfSize = 0x20;
    dst->pfFlags = 4;                   // DDPF_FOURCC
    dst->bitCount = type;
    dst->pfFourCC = 0x12345678;

    float progressValue = 0.0f;
    const uint32_t bpp = (src->bitCount + 7) >> 3;
    int shift[3];
    for (int c = 0; c < 3; ++c)
        shift[c] = LowBit(src->mask[c]);
    const int alphaShift = LowBit(src->mask[3]);
    const uint32_t keyMode = type & 0xff;
    uint32_t alphaRef = 0;
    if (keyMode == S3TC_ALPHA_KEY)
        alphaRef = (((src->mask[3] >> alphaShift) * g_alphaThreshold) / 255) << alphaShift;
    float progressStep = 0.0f;
    if (progress)
        progressStep = static_cast<float>(100.0 / static_cast<double>((src->height >> 2) * (src->width >> 2)));

    uint8_t *out = static_cast<uint8_t *>(bits);
    const uint8_t *row = static_cast<const uint8_t *>(src->surface);
    for (uint32_t y = 0; y < src->height; y += 4) {
        const uint32_t rows = src->height - y < 4 ? src->height - y : 4;
        const uint8_t *blockPtr = row;
        for (uint32_t x = 0; x < src->width; x += 4) {
            const uint32_t cols = src->width - x < 4 ? src->width - x : 4;
            S_block block = {};
            for (int c = 0; c < 3; ++c)
                block.weights[c] = weights[c];
            int map[16];

            if ((type & 0xff00) == S3TC_ALPHA_EXPLICIT) {
                const uint8_t *p = blockPtr;
                uint32_t acc = 0;
                for (uint32_t r = 0; r < 4; ++r) {
                    if (r < rows) {
                        for (uint32_t c = 0; c < 4; ++c) {
                            if (c < cols) {
                                const uint32_t a = (src->mask[3] & ReadPixel(p, bpp)) >> alphaShift;
                                const uint32_t amax = src->mask[3] >> alphaShift;
                                const double v = std::floor(
                                    R(R(R(static_cast<double>(a) / static_cast<double>(amax)) * 15.0) + 0.5));
                                acc = (static_cast<uint32_t>(Ftol(v)) << 12) | ((acc & 0xffff) >> 4);
                            } else {
                                acc = (acc & 0xffff) >> 4;
                            }
                            p += bpp;
                        }
                        const uint16_t word = static_cast<uint16_t>(acc);
                        std::memcpy(out, &word, 2);
                        p += src->pitch - static_cast<int>(bpp * 4);
                    } else {
                        out[0] = out[1] = 0;
                    }
                    out += 2;
                }
            } else if ((type & 0xff00) == S3TC_ALPHA_INTERPOLATED) {
                S_alphaBlock ab = {};
                const uint8_t *p = blockPtr;
                int alphaMap[16];
                for (uint32_t r = 0; r < 4; ++r) {
                    for (uint32_t c = 0; c < 4; ++c) {
                        const int i = static_cast<int>(r * 4 + c);
                        if (c < cols && r < rows) {
                            const uint32_t a = (src->mask[3] & ReadPixel(p, bpp)) >> alphaShift;
                            const uint32_t amax = src->mask[3] >> alphaShift;
                            alphaMap[i] = ab.count;
                            ab.alpha[ab.count++] =
                                a == amax ? 1.0f
                                          : static_cast<float>(static_cast<double>(a) / static_cast<double>(amax));
                        } else {
                            alphaMap[i] = -1;
                        }
                        p += bpp;
                    }
                    p += src->pitch - static_cast<int>(bpp * 4);
                }
                ab.needZero = 0;
                ab.needOne = 0;
                bool reset = false;
                if (ab.count == 0) {
                    ab.alpha0 = 0;
                    ab.alpha1 = 1;
                    reset = true;
                } else {
                    EncodeAlphaBlock(&ab);
                    if (ab.alpha0 == ab.alpha1) {
                        if (ab.alpha0 != 255)
                            ++ab.alpha1;
                        else
                            --ab.alpha1;
                        reset = true;
                    }
                }
                if (reset) {
                    for (int i = 0; i < ab.count; ++i)
                        ab.indices[i] = 0;
                    ab.mode = 6;
                }
                const bool swap = (ab.alpha1 < ab.alpha0) == (ab.mode == 6);
                if (swap) {
                    const int t = ab.alpha0;
                    ab.alpha0 = ab.alpha1;
                    ab.alpha1 = t;
                }
                out[0] = static_cast<uint8_t>(ab.alpha0);
                out[1] = static_cast<uint8_t>(ab.alpha1);
                for (int i = 2; i < 8; ++i)
                    out[i] = 0;
                uint8_t *q = out + 2;
                uint32_t codes = 0;
                for (int r = 0; r < 4; ++r) {
                    for (int c = 0; c < 4; ++c) {
                        const int i = r * 4 + c;
                        codes >>= 3;
                        if (alphaMap[i] >= 0) {
                            uint32_t code = static_cast<uint32_t>(ab.indices[alphaMap[i]]);
                            if (swap) {
                                if (ab.mode == 8)
                                    code = code < 2 ? (code == 0) : 9 - code;
                                else if (ab.mode == 6 && code < 6)
                                    code = code < 2 ? (code == 0) : 7 - code;
                            }
                            codes |= code << 21;
                        }
                    }
                    if (r & 1) {
                        q[0] |= static_cast<uint8_t>(codes);
                        q[1] |= static_cast<uint8_t>(codes >> 8);
                        q[2] |= static_cast<uint8_t>(codes >> 16);
                        q += 3;
                        codes = 0;
                    }
                }
                out += 8;
            }

            // The colours (opaque pixels only).
            const uint8_t *p = blockPtr;
            for (uint32_t r = 0; r < 4; ++r) {
                for (uint32_t c = 0; c < 4; ++c) {
                    const int i = static_cast<int>(r * 4 + c);
                    map[i] = -1;
                    if (c < cols && r < rows) {
                        if (!palette) {
                            const uint32_t pixel = ReadPixel(p, bpp);
                            bool opaque = true;
                            if (keyMode == S3TC_COLOR_KEY) {
                                const uint32_t key = (src->mask[2] | src->mask[1] | src->mask[0]) & pixel;
                                opaque = key < src->keyLow || src->keyHigh < key;
                            } else if (keyMode == S3TC_ALPHA_KEY) {
                                opaque = alphaRef < (src->mask[3] & pixel);
                            } else if (keyMode != S3TC_OPAQUE) {
                                opaque = false;
                            }
                            if (opaque) {
                                map[i] = block.count;
                                for (int ch = 0; ch < 3; ++ch) {
                                    const uint32_t v = (src->mask[ch] & pixel) >> shift[ch];
                                    const uint32_t vmax = src->mask[ch] >> shift[ch];
                                    block.colors[block.count][ch] =
                                        R(static_cast<double>(v) / static_cast<double>(vmax));
                                }
                                ++block.count;
                            }
                        } else {
                            // (A palette with S3TC_ALPHA_KEY leaves the original's index map unset.)
                            const uint8_t *e = palette + static_cast<size_t>(*p) * 4;
                            bool opaque = keyMode == S3TC_OPAQUE;
                            if (keyMode == S3TC_COLOR_KEY) {
                                uint32_t key;
                                std::memcpy(&key, e, 4);
                                key &= 0xffffff;
                                opaque = !(src->keyLow <= key && key <= src->keyHigh);
                            }
                            if (opaque) {
                                map[i] = block.count;
                                for (int ch = 0; ch < 3; ++ch)
                                    block.colors[block.count][ch] = R(static_cast<double>(e[ch]) * kInv255);
                                ++block.count;
                            }
                        }
                    }
                    p += palette ? 1 : bpp;
                }
                p += src->pitch - static_cast<int>((palette ? 1 : bpp) * 4);
            }

            block.mode = block.count >= static_cast<int>(cols * rows) ? 4 : 3;
            if (block.count == 0)
                std::memset(block.endpoints, 0, sizeof block.endpoints);
            else
                EncodeBlock(&block);
            uint16_t c0 = static_cast<uint16_t>(((block.endpoints[0][0] << 6) | block.endpoints[0][1]) << 5 |
                                                block.endpoints[0][2]);
            uint16_t c1 = static_cast<uint16_t>(((block.endpoints[1][0] << 6) | block.endpoints[1][1]) << 5 |
                                                block.endpoints[1][2]);
            if (c1 == c0) {
                ++c1;
                for (int i = 0; i < block.count; ++i)
                    block.indices[i] = 0;
                block.resultMode = 3;
            }
            const bool swap = (c1 < c0) == (block.resultMode == 3);
            if (swap) {
                const uint16_t t = c0;
                c0 = c1;
                c1 = t;
            }
            const uint32_t colors = c0 | static_cast<uint32_t>(c1) << 16;
            uint32_t codes = 0;
            for (int i = 15; i >= 0; --i) {
                if (map[i] < 0) {
                    codes = codes << 2 | 3;
                } else {
                    uint32_t code = static_cast<uint32_t>(block.indices[map[i]]);
                    if (swap)
                        code ^= block.resultMode == 4 ? 1 : ((~code >> 1) & 1);
                    codes = codes << 2 | code;
                }
            }
            std::memcpy(out, &colors, 4);
            std::memcpy(out + 4, &codes, 4);
            out += 8;
            if (progress) {
                progressValue = progressStep + progressValue;
                if (progress(progressValue, context))
                    return -1;
            }
            blockPtr += bpp * 4;
        }
        row += src->pitch * 4;
    }
    return 0;
}

}  // namespace ls3d::s3tc
