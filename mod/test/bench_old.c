// Offline bench for the resampler in nexsink.c. Measures passband response and
// residual aliasing after decimation, so the numbers quoted to the user come
// from the shipped code rather than from a paper design.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

typedef unsigned char BYTE;
#include "old_only.inc"

#define CH 2
#define PI 3.14159265358979323846

/* Run `in` through the resampler, return the produced output (interleaved). */
static double *run(const float *in, long n_in, long *n_out, int in_rate, int out_rate) {
    if (!src_init(in_rate, out_rate, CH)) { fprintf(stderr, "src_init failed\n"); exit(1); }
    long cap = (long)((double)n_in * out_rate / in_rate) + 65536;
    double *out = (double *)calloc((size_t)cap * CH, sizeof(double));
    float  *fout = (float *)calloc((size_t)SCRATCH_FRAMES * CH, sizeof(float));
    long produced = 0, fed = 0;

    while (fed < n_in) {
        long k = n_in - fed;
        if (k > SCRATCH_FRAMES) k = SCRATCH_FRAMES;
        src_push(in + fed * CH, (int)k, CH);
        fed += k;
        for (;;) {
            int r = src_ready(CH);
            if (r <= 0) break;
            if (r > SCRATCH_FRAMES) r = SCRATCH_FRAMES;
            src_pull(fout, r, CH);
            for (long i = 0; i < (long)r * CH; i++) out[produced * CH + i] = fout[i];
            produced += r;
        }
    }
    *n_out = produced;
    free(fout);
    return out;
}

/* RMS of channel 0 over [from, to) frames, in dB relative to full scale. */
static double rms_db(const double *y, long n, long from, long to) {
    double s = 0.0; long c = 0;
    for (long i = from; i < to && i < n; i++) { double v = y[i * CH]; s += v * v; c++; }
    if (!c) return -999.0;
    return 10.0 * log10(s / (double)c + 1e-300);
}

static void tone(float *x, long n, double f, double fs, double amp) {
    for (long i = 0; i < n; i++) {
        float v = (float)(amp * sin(2.0 * PI * f * (double)i / fs));
        x[i * CH] = v; x[i * CH + 1] = v;
    }
}

int main(int argc, char **argv) {
    int in_rate  = argc > 1 ? atoi(argv[1]) : 192000;
    int out_rate = argc > 2 ? atoi(argv[2]) : 96000;
    long n_in    = (long)in_rate * 2;
    float *in    = (float *)calloc((size_t)n_in * CH, sizeof(float));
    double ref   = 0.0;

    if (!src_init(in_rate, out_rate, CH)) { fprintf(stderr, "src_init failed\n"); return 1; }
    long long g = 1;
    printf("=== %d -> %d | taps=%d blackman phases=%lld half=%.1f in-frames step=%.4f\n",
           in_rate, out_rate, SRC_TAPS, (long long)out_rate, g_src.half, g_src.step);
    src_free();

    static const double pass[] = { 20, 100, 1000, 5000, 10000, 15000, 18000, 20000, 22000 };
    for (int k = 0; k < (int)(sizeof(pass) / sizeof(pass[0])); k++) {
        double f = pass[k];
        if (f >= out_rate / 2.0) continue;
        tone(in, n_in, f, in_rate, 0.5);
        long n; double *y = run(in, n_in, &n, in_rate, out_rate);
        double db = rms_db(y, n, (long)out_rate / 5, n);
        if (k == 2) ref = db;
        printf("  pass %7.0f Hz  %8.3f dBFS   rel %+7.3f dB\n", f, db, db - ref);
        free(y);
    }

    printf("  --- stopband: a perfect 2:1 filter leaves nothing above 48 kHz ---\n");
    static const double stop[] = { 49000, 51000, 55000, 60000, 70000, 80000, 90000, 95000 };
    for (int k = 0; k < (int)(sizeof(stop) / sizeof(stop[0])); k++) {
        double f = stop[k];
        if (f >= in_rate / 2.0) continue;
        tone(in, n_in, f, in_rate, 0.5);
        long n; double *y = run(in, n_in, &n, in_rate, out_rate);
        double db = rms_db(y, n, (long)out_rate / 5, n);
        printf("  stop %7.0f Hz  %8.1f dBFS   suppression %6.1f dB\n", f, db, ref - db);
        free(y);
    }
    free(in);
    return 0;
}
