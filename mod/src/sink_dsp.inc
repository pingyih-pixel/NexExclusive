/* ---- ring buffer (output format) ---------------------------------------- */
// Compact so the occupied span starts at index 0. Caller holds g_lock.
static void ring_compact(void) {
    UINT32 used = g_sink.w - g_sink.r;
    if (!used) { g_sink.w = 0; g_sink.r = 0; return; }
    UINT32 rmod = g_sink.r % g_sink.cap;
    size_t ob  = (size_t)g_sink.in_block;
    /* Preallocated staging prevents the first copy overwriting a wrapped head. */
    if (rmod + used <= g_sink.cap) {
        memmove(g_sink.ring, g_sink.ring + rmod * ob, (size_t)used * ob);
    } else {
        UINT32 first = g_sink.cap - rmod;
        memcpy(g_sink.conv, g_sink.ring + rmod * ob, (size_t)first * ob);
        memcpy(g_sink.conv + (size_t)first * ob, g_sink.ring, (size_t)(used-first) * ob);
        memcpy(g_sink.ring, g_sink.conv, (size_t)used * ob);
    }
    g_sink.w = used;
    g_sink.r = 0;
}

// Reserve n contiguous input frames. Blocks until room appears.
static BYTE *ring_reserve(UINT32 n) {
    if (n > g_sink.cap) {
        /* Cannot fit by definition. Waiting here would spin forever, because
           the contiguity check below can never pass for n == cap at a
           non-zero write offset - and a client stuck in GetBuffer is a frozen
           transport bar. Fail the call so it restarts the stream instead. */
        log_line("ring: %lu-frame reservation on a %lu-frame ring, refusing",
                 (unsigned long)n, (unsigned long)g_sink.cap);
        return NULL;
    }
    int waits = 0;
    for (;;) {
        EnterCriticalSection(&g_lock);
        if (!g_sink.engaged) { LeaveCriticalSection(&g_lock); return NULL; }
        UINT32 used = g_sink.w - g_sink.r;
        if (g_sink.cap - used >= n) {
            if ((g_sink.w % g_sink.cap) + n > g_sink.cap) ring_compact();
            if ((g_sink.w % g_sink.cap) + n <= g_sink.cap) {
                BYTE *p = g_sink.ring + (size_t)(g_sink.w % g_sink.cap) * g_sink.in_block;
                LeaveCriticalSection(&g_lock);
                return p;
            }
        }
        LeaveCriticalSection(&g_lock);
        if (!g_sink.engaged) return NULL;
        if (++waits > 50) {          /* ~1 s with no room: the drain is wedged */
            log_line("ring: no room for %lu frames after 1 s (w=%lu r=%lu cap=%lu)",
                     (unsigned long)n, (unsigned long)g_sink.w,
                     (unsigned long)g_sink.r, (unsigned long)g_sink.cap);
            return NULL;
        }
        WaitForSingleObject(g_sink.h_space, 20);
    }
}

/* ---- sample conversion -------------------------------------------------- */
static double rnd01(void);
static double sample_read(const BYTE *p, int bits, int is_float) {
    if (is_float) { float f; memcpy(&f,p,4); return isfinite(f) ? f : 0.0; }
    if (bits == 16) { short x; memcpy(&x,p,2); return (double)x / 32768.0; }
    if (bits == 24) {
        int x = p[0] | (p[1]<<8) | (p[2]<<16);
        if (x & 0x800000) x |= ~0xFFFFFF;
        return (double)x / 8388608.0;
    }
    int x; memcpy(&x,p,4); return (double)x / 2147483648.0;
}
static void sample_write(double v, BYTE *p, int bits, int is_float, int dither) {
    if (!isfinite(v)) v = 0.0;
    if (is_float) { float f = (float)v; memcpy(p,&f,4); return; }
    double scale = ldexp(1.0,bits-1);
    v *= scale;
    if (dither) v += rnd01()-rnd01();
    if (v > scale-1) v = scale-1;
    if (v < -scale) v = -scale;
    INT64 q = (INT64)llrint(v);
    if (bits == 16) { short x=(short)q; memcpy(p,&x,2); }
    else if (bits == 24) { p[0]=(BYTE)q; p[1]=(BYTE)(q>>8); p[2]=(BYTE)(q>>16); }
    else { int x=(int)q; memcpy(p,&x,4); }
}
static void convert(const BYTE *src, BYTE *dst, UINT32 frames) {
    size_t n = (size_t)frames * g_sink.in_channels;
    int ib = g_sink.in_container_bits, ob = g_sink.out_bits;
    if (ib == ob && g_sink.in_is_float == g_sink.out_is_float && !g_sink.dither) {
        memcpy(dst,src,n*(ib/8)); return;
    }
    for (size_t i=0; i<n; i++)
        sample_write(sample_read(src+i*(ib/8),ib,g_sink.in_is_float),
                     dst+i*(ob/8),ob,g_sink.out_is_float,g_sink.dither);
}

/* ---- float pipeline: rate conversion + dithered quantisation ------------ */
//
// Used only when the device refuses the client's own rate or depth. The path is
// integer/float -> float32 -> polyphase windowed-sinc resample -> quantise, so
// the same code covers 192->96, 44.1->48 and 96->44.1 alike.
//
// Both rates are integers, so the fractional read position is not an arbitrary
// real: after g = gcd(in,out) the step is num/den and the phase sequence takes
// exactly `den` distinct values, repeating forever. Building one kernel per
// phase therefore makes the interpolation exact - no phase quantisation, no
// drift, no interpolated coefficient tables - and leaves one dot product per
// output frame. The kernel is symmetric (linear phase) and normalised per phase
// so DC gain is exactly unity at every phase.
//
// Window: Kaiser, beta = 12, which puts the stopband floor near -120 dB. The
// earlier 32-tap Blackman version bottomed out at -74 dB - that is the Blackman
// sidelobe floor, not a tap-count problem - so aliasing images from 192->96 sat
// only ~20 dB under the 16-bit noise floor. 128 taps at 2:1 gives a transition
// band of about +/-3 kHz around 48 kHz: flat to 45 kHz, -120 dB above 51 kHz.
#define SRC_TAPS        128          /* filter length, in output samples */
#define SRC_MAX_HALF    256          /* bound on the kernel half width */
#define SRC_MAX_PHASES  512
#define SRC_CAP         32768
#define SRC_BETA        12.0

typedef struct {
    int     built;
    int     kmin;    /* first input index relative to floor(pos) */
    int     n;       /* coefficient count */
    double *h;
} Phase;

typedef struct {
    float    *buf;   /* interleaved input history, SRC_CAP frames */
    int       count; /* valid frames */
    long long pos;   /* read position, in units of 1/den input frames */
    long long num;   /* step numerator   = in_rate / gcd */
    long long den;   /* step denominator = out_rate / gcd */
    int       phases;
    double    step;  /* num / den */
    double    cutoff;/* min(1, 1/step) */
    double    half;  /* kernel half width, in input frames */
    Phase     ph[SRC_MAX_PHASES];
    double   *pool;
} Src;

static Src g_src;
static double g_i0_beta = 1.0;

#define SCRATCH_FRAMES 4096
static BYTE  *g_inb;    /* client-format scratch */
static float *g_fin;    /* the same frames as float */
static float *g_fout;   /* resampled output, up to one device buffer */

static void scratch_free(void) {
    if (g_inb)  { free(g_inb);  g_inb  = NULL; }
    if (g_fin)  { free(g_fin);  g_fin  = NULL; }
    if (g_fout) { free(g_fout); g_fout = NULL; }
}

/* A transport Reset discards input history, not just the client ring. Cached
   coefficients remain valid because the rate pair has not changed. */
static void src_reset(int ch) {
    if (!g_src.buf) return;
    memset(g_src.buf, 0, (size_t)SRC_CAP * (size_t)ch * sizeof(float));
    int K = (int)ceil(g_src.half);
    g_src.pos = (long long)K * g_src.den;
    g_src.count = 2 * K + 1;
}

static void src_free(void) {
    if (g_src.buf)  { free(g_src.buf);  g_src.buf  = NULL; }
    if (g_src.pool) { free(g_src.pool); g_src.pool = NULL; }
    memset(g_src.ph, 0, sizeof(g_src.ph));
    g_src.count = 0;
    g_src.pos   = 0;
}

static long long src_gcd(long long a, long long b) {
    while (b) { long long t = a % b; a = b; b = t; }
    return a > 0 ? a : 1;
}

/* Modified Bessel function of the first kind, order 0. */
static double src_i0(double x) {
    double sum = 1.0, term = 1.0, y = x * x * 0.25;
    for (int k = 1; k < 64; k++) {
        term *= y / ((double)k * (double)k);
        sum  += term;
        if (term < 1e-17 * sum) break;
    }
    return sum;
}

static double src_sinc(double x) {
    if (fabs(x) < 1e-9) return 1.0;
    double px = 3.14159265358979323846 * x;
    return sin(px) / px;
}

/* Kaiser over [-half, half] */
static double src_window(double t, double half) {
    double r = t / half;
    if (r <= -1.0 || r >= 1.0) return 0.0;
    return src_i0(SRC_BETA * sqrt(1.0 - r * r)) / g_i0_beta;
}

static int src_init(int in_rate, int out_rate, int ch) {
    src_free();
    if (in_rate <= 0 || out_rate <= 0 || ch < 1 || ch > 8) return 0;

    long long g = src_gcd(in_rate, out_rate);
    g_src.num  = (long long)in_rate / g;
    g_src.den  = (long long)out_rate / g;
    g_src.step = (double)g_src.num / (double)g_src.den;
    g_src.cutoff = g_src.step > 1.0 ? 1.0 / g_src.step : 1.0;

    double half = (double)SRC_TAPS / 2.0 / g_src.cutoff;
    if (half > SRC_MAX_HALF) return 0; /* quality bound: reject rather than shorten */
    if (half < 2.0) half = 2.0;
    g_src.half = half;

    /* Never silently approximate an unsupported phase denominator. */
    if (g_src.den > SRC_MAX_PHASES) return 0;
    int phases = (int)g_src.den;
    if (phases < 1) phases = 1;
    g_src.phases = phases;

    int    K      = (int)ceil(half);
    size_t stride = (size_t)(2 * K + 1);

    g_src.buf  = (float *)calloc((size_t)SRC_CAP * (size_t)ch, sizeof(float));
    g_src.pool = (double *)calloc((size_t)phases * stride, sizeof(double));
    if (!g_src.buf || !g_src.pool) { src_free(); return 0; }

    g_i0_beta = src_i0(SRC_BETA);
    for (int i = 0; i < phases; i++) {
        g_src.ph[i].h     = g_src.pool + (size_t)i * stride;
        /* 区间必须是 [-K, +K]。早先是 [-K+1, +K]：区间本身不对称，最外侧的
           +K 抽头没有配对。整数相位时它恰好落在窗函数零点上（K >= half 时
           窗值为 0），看不出问题；小数相位下 |K - frac| 可能小于 half，那个
           抽头就带上了 ~1e-3 的权重，核不再严格对称，线性相位也就名不副实。
           回归里的 src_kernel_symmetric 就是卡这一条的。 */
        g_src.ph[i].kmin  = -K;
        g_src.ph[i].n     = 2 * K + 1;
        g_src.ph[i].built = 0;
    }

    /* Prime with the same clean history used by a transport Reset. */
    src_reset(ch);
    log_line("resample: %d -> %d Hz, taps=%d kaiser b=%.0f, phases=%d (den=%lld), half=%.1f frames",
             in_rate, out_rate, SRC_TAPS, SRC_BETA, phases, g_src.den, half);
    return 1;
}

/* Build one phase. frac is exact: idx/phases == idx/den whenever den fits. */
static void src_phase_build(int idx) {
    Phase *p    = &g_src.ph[idx];
    double frac = (double)idx / (double)g_src.phases;
    double half = g_src.half;
    double cut  = g_src.cutoff;
    double sum  = 0.0;

    for (int i = 0; i < p->n; i++) {
        int    kk = p->kmin + i;
        double t  = (double)kk - frac;
        double v  = 0.0;
        if (fabs(t) < half)
            v = src_sinc(cut * t) * cut * src_window(t, half);
        p->h[i] = v;
        sum    += v;
    }
    if (sum != 0.0)
        for (int i = 0; i < p->n; i++) p->h[i] /= sum;

    p->built = 1;
}

/* Drop history that no future kernel can reach. */
static void src_compact(int ch) {
    if (!g_src.buf) return;
    int i0   = (int)(g_src.pos / g_src.den);
    int drop = i0 - (int)ceil(g_src.half) - 1;
    if (drop <= 0) return;
    if (drop > g_src.count) drop = g_src.count;
    memmove(g_src.buf, g_src.buf + (size_t)drop * ch,
            (size_t)(g_src.count - drop) * ch * sizeof(float));
    g_src.count -= drop;
    g_src.pos   -= (long long)drop * g_src.den;
}

/* append converted float frames; caller passes interleaved float */
static int src_push(const float *in, int frames, int ch) {
    if (!g_src.buf) return 0;
    if (g_src.count + frames > SRC_CAP) {
        src_compact(ch);
        if (g_src.count + frames > SRC_CAP) return 0;
    }
    memcpy(g_src.buf + (size_t)g_src.count * ch, in,
           (size_t)frames * ch * sizeof(float));
    g_src.count += frames;
    return frames;
}

/* how many output frames can be produced right now */
static int src_ready(int ch) {
    (void)ch;
    if (!g_src.buf) return 0;
    double pos  = (double)g_src.pos / (double)g_src.den;
    double need = pos + g_src.half + 1.0;
    if (need >= (double)g_src.count) return 0;
    double n = ((double)g_src.count - g_src.half - 1.0 - pos) / g_src.step;
    return n > 0 ? (int)n : 0;
}

static void src_pull(float *out, int frames, int ch) {
    const long long den = g_src.den;
    const int       np  = g_src.phases;
    const int       ex  = (np == (int)den);

    for (int f = 0; f < frames; f++) {
        int i0  = (int)(g_src.pos / den);
        int q   = (int)(g_src.pos % den);
        int idx = ex ? q : (int)((long long)q * np / den);
        if (idx >= np) idx = np - 1;

        Phase *p = &g_src.ph[idx];
        if (!p->built) src_phase_build(idx);

        const float  *base = g_src.buf + (size_t)(i0 + p->kmin) * ch;
        const double *hh   = p->h;
        for (int c = 0; c < ch; c++) {
            const float *x = base + c;
            double acc = 0.0;
            for (int i = 0; i < p->n; i++)
                acc += (double)x[(size_t)i * ch] * hh[i];
            out[(size_t)f * ch + c] = (float)acc;
        }
        g_src.pos += g_src.num;
    }
}

/* client format -> float32, interleaved */
static void to_float(const BYTE *src, float *dst, UINT32 frames) {
    const int ch = g_sink.in_channels;
    const UINT32 n = frames * (UINT32)ch;
    if (g_sink.in_is_float) {
        memcpy(dst, src, (size_t)n * sizeof(float));
        return;
    }
    int ib = g_sink.in_container_bits;
    if (ib == 32) {
        const int *s = (const int *)src;
        for (UINT32 i = 0; i < n; i++) dst[i] = (float)((double)s[i] / 2147483648.0);
    } else if (ib == 24) {
        for (UINT32 i = 0; i < n; i++) {
            int v = src[i * 3] | (src[i * 3 + 1] << 8) | (src[i * 3 + 2] << 16);
            if (v & 0x800000) v |= ~0xFFFFFF;
            dst[i] = (float)((double)v / 8388608.0);
        }
    } else {
        const short *s = (const short *)src;
        for (UINT32 i = 0; i < n; i++) dst[i] = (float)((double)s[i] / 32768.0);
    }
}

/* deterministic TPDF source: two rectangular draws subtracted */
static unsigned long g_rng = 0x9E3779B9ul;
static double rnd01(void) {
    g_rng = g_rng * 1103515245ul + 12345ul;
    return (double)((g_rng >> 8) & 0xFFFFFFul) / 16777216.0;
}

static void quantize(const float *in, UINT32 frames, BYTE *dst) {
    const int ch = g_sink.in_channels;
    const UINT32 n = frames * (UINT32)ch;
    const int bits = g_sink.out_bits;

    if (g_sink.out_is_float) {
        memcpy(dst, in, (size_t)n * sizeof(float));
        return;
    }
    for (UINT32 i = 0; i < n; i++)
        sample_write(in[i], dst + (size_t)i * (bits/8), bits, 0, g_sink.dither);
}

/* ---- drain thread ------------------------------------------------------- */
/* An exclusive stream that starts on a fresh buffer goes from nothing to full
   scale in one sample. Ramping the first few milliseconds keeps that step off
   the DAC, which is where the opening pop comes from. */
static void apply_fade(BYTE *dst, UINT32 frames) {
    if (!g_sink.fade_left) return;
    const int ch = g_sink.in_channels;
    const int bytes = g_sink.out_bits / 8;
    if (!ch || !bytes) return;

    for (UINT32 f = 0; f < frames && g_sink.fade_left; f++) {
        UINT32 done = g_sink.fade_total - g_sink.fade_left;
        int g = (int)(((UINT64)done * 65536u) / g_sink.fade_total);   /* Q16 */
        for (int c = 0; c < ch; c++) {
            BYTE *p = dst + ((size_t)f * ch + c) * (size_t)bytes;
            if (bytes == 3) {
                int v = p[0] | (p[1] << 8) | (p[2] << 16);
                if (v & 0x800000) v |= ~0xFFFFFF;
                /* A 24-bit sample times Q16 gain needs up to 40 bits.
                   A 32-bit intermediate wraps into a short noise burst. */
                v = (int)(((INT64)v * g) >> 16);
                p[0] = (BYTE)(v & 0xFF);
                p[1] = (BYTE)((v >> 8) & 0xFF);
                p[2] = (BYTE)((v >> 16) & 0xFF);
            } else if (bytes == 2) {
                short v; memcpy(&v, p, 2);
                v = (short)((v * g) >> 16);
                memcpy(p, &v, 2);
            } else if (g_sink.out_is_float) {
                float v; memcpy(&v, p, 4);
                v *= (float)((double)g / 65536.0);
                memcpy(p, &v, 4);
            } else {
                int v; memcpy(&v, p, 4);
                v = (int)(((INT64)v * g) >> 16);
                memcpy(p, &v, 4);
            }
        }
        g_sink.fade_left--;
    }
}

/* pull up to maxframes from the ring into a client-format scratch buffer */
static UINT32 ring_take(BYTE *dst, UINT32 maxframes) {
    EnterCriticalSection(&g_lock);
    UINT32 avail = g_sink.w - g_sink.r;
    UINT32 start = g_sink.r % g_sink.cap;
    if (start + avail > g_sink.cap) avail = g_sink.cap - start;
    UINT32 n = avail < maxframes ? avail : maxframes;
    if (n)
        memcpy(dst, g_sink.ring + (size_t)start * g_sink.in_block,
               (size_t)n * g_sink.in_block);
    g_sink.r += n;
    LeaveCriticalSection(&g_lock);
    return n;
}

