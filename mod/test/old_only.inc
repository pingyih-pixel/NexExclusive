/* ---- float pipeline: rate conversion + dithered quantisation ------------ */
//
// Used only when the device refuses the client's own rate or depth. The path is
// integer/float -> float32 -> windowed-sinc resample -> quantise, so the same
// code covers 192->96, 44.1->48 and 96->44.1 alike. Upsampling stretches the
// kernel to suppress images, downsampling squeezes it to suppress aliasing;
// either way the cutoff follows min(1, out/in).
#define SRC_TAPS   32
#define SRC_CAP    32768

typedef struct {
    float *buf;      /* interleaved input history, SRC_CAP frames */
    int    count;    /* valid frames */
    double pos;      /* read position in frames, relative to buf[0] */
    double step;     /* in_rate / out_rate */
    double cutoff;   /* min(1, 1/step) */
    double half;     /* kernel half width, in input frames */
} Src;

static Src g_src;

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
    if (g_src.buf) { free(g_src.buf); g_src.buf = NULL; }
    g_src.count = 0;
    g_src.pos = 0;
}

static int src_init(int in_rate, int out_rate, int ch) {
    src_free();
    g_src.buf = (float *)calloc((size_t)SRC_CAP * (size_t)ch, sizeof(float));
    if (!g_src.buf) return 0;
    g_src.step   = (double)in_rate / (double)out_rate;
    g_src.cutoff = g_src.step > 1.0 ? 1.0 / g_src.step : 1.0;
    g_src.half   = (double)SRC_TAPS / 2.0 / g_src.cutoff;
    if (g_src.half > 96.0) g_src.half = 96.0;   /* kernel stays bounded */
    /* start with a full kernel of history so the first output frame is clean */
    g_src.pos    = g_src.half;
    g_src.count  = (int)ceil(g_src.half);
    return 1;
}

static double src_sinc(double x) {
    if (fabs(x) < 1e-9) return 1.0;
    double px = 3.14159265358979323846 * x;
    return sin(px) / px;
}

/* Blackman over [-half, half] */
static double src_window(double t, double half) {
    double x = (t + half) / (2.0 * half);
    if (x < 0.0) x = 0.0;
    if (x > 1.0) x = 1.0;
    return 0.42 - 0.5 * cos(2.0 * 3.14159265358979323846 * x)
                + 0.08 * cos(4.0 * 3.14159265358979323846 * x);
}

static void src_compact(int ch) {
    if (!g_src.buf) return;
    int drop = (int)floor(g_src.pos) - (int)ceil(g_src.half) - 1;
    if (drop <= 0) return;
    if (drop > g_src.count) drop = g_src.count;
    memmove(g_src.buf, g_src.buf + (size_t)drop * ch,
            (size_t)(g_src.count - drop) * ch * sizeof(float));
    g_src.count -= drop;
    g_src.pos   -= drop;
}

/* append converted float frames; caller passes interleaved float */
static int src_push(const float *in, int frames, int ch) {
    if (!g_src.buf) return 0;
    if (g_src.count + frames > SRC_CAP) {
        /* drop everything already consumed */
        int drop = (int)floor(g_src.pos) - (int)ceil(g_src.half) - 1;
        if (drop > 0) {
            if (drop > g_src.count) drop = g_src.count;
            memmove(g_src.buf, g_src.buf + (size_t)drop * ch,
                    (size_t)(g_src.count - drop) * ch * sizeof(float));
            g_src.count -= drop;
            g_src.pos   -= drop;
        }
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
    double last = g_src.pos + g_src.half;          /* needs input up to here */
    if (last >= (double)g_src.count) return 0;
    double n = ((double)g_src.count - g_src.half - g_src.pos) / g_src.step;
    return n > 0 ? (int)n : 0;
}

/* Kernels depend only on the fractional position, and a 2:1 decimation only
   ever asks for two of them. Computing 256 sines per output frame would cost
   more than the filtering itself, so cache by phase. */
typedef struct {
    int    valid;
    long   key;
    int    kmin, kmax;
    double h[2 * SRC_TAPS + 4];
} Kern;

static Kern g_kern[16];
static int  g_kern_next;

static const Kern *kernel_for(double frac) {
    long key = (long)llrint(frac * 4096.0);
    for (int i = 0; i < 16; i++)
        if (g_kern[i].valid && g_kern[i].key == key) return &g_kern[i];

    Kern *k = &g_kern[g_kern_next];
    g_kern_next = (g_kern_next + 1) % 16;

    int K = (int)ceil(g_src.half);
    k->kmin = -K + 1;
    k->kmax = K;
    if (k->kmax - k->kmin + 1 > (int)(sizeof(k->h) / sizeof(k->h[0])))
        k->kmax = k->kmin + (int)(sizeof(k->h) / sizeof(k->h[0])) - 1;

    double sum = 0.0;
    for (int kk = k->kmin; kk <= k->kmax; kk++) {
        double t = (double)kk - frac;
        double v = 0.0;
        if (fabs(t) <= g_src.half)
            v = src_sinc(g_src.cutoff * t) * g_src.cutoff * src_window(t, g_src.half);
        k->h[kk - k->kmin] = v;
        sum += v;
    }
    if (sum != 0.0)
        for (int kk = k->kmin; kk <= k->kmax; kk++) k->h[kk - k->kmin] /= sum;

    k->key = key;
    k->valid = 1;
    return k;
}

static void src_pull(float *out, int frames, int ch) {
    for (int f = 0; f < frames; f++) {
        int i0 = (int)floor(g_src.pos);
        double frac = g_src.pos - (double)i0;
        const Kern *k = kernel_for(frac);
        for (int c = 0; c < ch; c++) {
            double acc = 0.0;
            int base = i0 * ch + c;
            for (int kk = k->kmin; kk <= k->kmax; kk++)
                acc += (double)g_src.buf[base + kk * ch] * k->h[kk - k->kmin];
            out[(size_t)f * ch + c] = (float)acc;
        }
        g_src.pos += g_src.step;
    }
}

