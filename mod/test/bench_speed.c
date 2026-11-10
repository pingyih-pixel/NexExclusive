// Throughput check: how much CPU does the resampler burn at 96 kHz stereo?
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>

typedef unsigned char BYTE;
#include "src_only.inc"

#define CH 2

int main(int argc, char **argv) {
    int in_rate  = argc > 1 ? atoi(argv[1]) : 192000;
    int out_rate = argc > 2 ? atoi(argv[2]) : 96000;
    long secs    = argc > 3 ? atol(argv[3]) : 60;
    long n_in    = (long)in_rate * secs;

    float *in  = (float *)malloc((size_t)n_in * CH * sizeof(float));
    float *out = (float *)malloc((size_t)SCRATCH_FRAMES * CH * sizeof(float));
    for (long i = 0; i < n_in * CH; i++) in[i] = (float)(0.3 * sin(i * 0.001));

    src_init(in_rate, out_rate, CH);

    clock_t t0 = clock();
    long produced = 0, fed = 0;
    double sink = 0.0;
    while (fed < n_in) {
        long k = n_in - fed;
        if (k > SCRATCH_FRAMES) k = SCRATCH_FRAMES;
        src_push(in + fed * CH, (int)k, CH);
        fed += k;
        for (;;) {
            int r = src_ready(CH);
            if (r <= 0) break;
            if (r > SCRATCH_FRAMES) r = SCRATCH_FRAMES;
            src_pull(out, r, CH);
            for (long i = 0; i < (long)r * CH; i++) sink += out[i];
            produced += r;
        }
    }
    double wall = (double)(clock() - t0) / CLOCKS_PER_SEC;

    printf("%d -> %d, %ld s of audio (%ld out frames) in %.3f s wall\n",
           in_rate, out_rate, secs, produced, wall);
    printf("  realtime factor %.0fx, one core load %.1f%%  (checksum %.4f)\n",
           (double)secs / wall, 100.0 * wall / (double)secs, sink);
    return 0;
}
