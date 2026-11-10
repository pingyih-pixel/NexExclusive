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
    if (in_rate <= 0 || out_rate <= 0) return 0;

    long long g = src_gcd(in_rate, out_rate);
    g_src.num  = (long long)in_rate / g;
    g_src.den  = (long long)out_rate / g;
    g_src.step = (double)g_src.num / (double)g_src.den;
    g_src.cutoff = g_src.step > 1.0 ? 1.0 / g_src.step : 1.0;

    double half = (double)SRC_TAPS / 2.0 / g_src.cutoff;
    if (half > SRC_MAX_HALF) half = SRC_MAX_HALF;   /* bounded worst case */
    if (half < 2.0) half = 2.0;
    g_src.half = half;

    int phases = (int)(g_src.den > SRC_MAX_PHASES ? SRC_MAX_PHASES : g_src.den);
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
        g_src.ph[i].kmin  = -K + 1;
        g_src.ph[i].n     = 2 * K;
        g_src.ph[i].built = 0;
    }

    /* Prime with a full kernel of silence so the first output frame is clean. */
    g_src.pos   = (long long)K * g_src.den;
    g_src.count = 2 * K + 1;
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
