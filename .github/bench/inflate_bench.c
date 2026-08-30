/* inflate_bench.c -- time inflate() and inflateBack() on a prepared stream
 * Not part of the library.  Used by .github/workflows/bench-inflate.yml.
 *
 * Only the inflate loop is timed; init and end are outside the clock.  The
 * default clock is per-process CPU time, which is far less sensitive to other
 * tenants on a shared CI runner than wall time.  On Windows, clock_gettime is
 * either missing at link time (clangarm64) or ~15.6 ms, so QueryPerformanceCounter
 * is used instead.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#if defined(_WIN32)
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#endif
#include "zlib.h"

#define WINSIZE 32768
#define MAX_ITERS 1000

#if defined(CLOCK_PROCESS_CPUTIME_ID)
#  define HAVE_CPU_CLOCK
#endif

static int use_cpu_clock = 1;

static double now(void) {
#if defined(_WIN32)
    static LARGE_INTEGER freq;
    LARGE_INTEGER counter;

    (void)use_cpu_clock;
    if (freq.QuadPart == 0)
        QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&counter);
    return (double)counter.QuadPart / (double)freq.QuadPart;
#else
    struct timespec ts;

#ifdef HAVE_CPU_CLOCK
    if (use_cpu_clock) {
        if (clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts) == 0)
            return ts.tv_sec + ts.tv_nsec * 1e-9;
    }
#endif
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
#endif
}

static int cmp_double(const void *a, const void *b) {
    double da = *(const double *)a;
    double db = *(const double *)b;
    return (da > db) - (da < db);
}

static unsigned char *read_file(const char *path, long *lenp) {
    FILE *fp;
    unsigned char *buf;
    long len;

    fp = fopen(path, "rb");
    if (fp == NULL) {
        perror(path);
        return NULL;
    }
    if (fseek(fp, 0, SEEK_END) != 0 || (len = ftell(fp)) < 0 ||
        fseek(fp, 0, SEEK_SET) != 0) {
        perror(path);
        fclose(fp);
        return NULL;
    }
    buf = malloc((size_t)len);
    if (buf == NULL) {
        fprintf(stderr, "out of memory reading %s\n", path);
        fclose(fp);
        return NULL;
    }
    if (len > 0 && fread(buf, 1, (size_t)len, fp) != (size_t)len) {
        perror(path);
        free(buf);
        fclose(fp);
        return NULL;
    }
    fclose(fp);
    *lenp = len;
    return buf;
}

/* inflateBack() callbacks.  All input is handed over in one call. */

struct in_state {
    unsigned char *in;
    unsigned len;
    unsigned used;
};

struct out_state {
    unsigned char *out;
    unsigned cap;
    unsigned used;
};

static unsigned pull_all(void *desc, unsigned char **buf) {
    struct in_state *d = desc;
    unsigned left;

    if (d->used >= d->len)
        return 0;
    *buf = d->in + d->used;
    left = d->len - d->used;
    d->used = d->len;
    return left;
}

static int push_all(void *desc, unsigned char *buf, unsigned len) {
    struct out_state *d = desc;

    if (d->used + len > d->cap)
        return 1;
    if (len)
        memcpy(d->out + d->used, buf, len);
    d->used += len;
    return 0;
}

/* One inflate of the whole stream.  Returns elapsed seconds, or -1 on error. */
static double run_full(unsigned char *in, long inlen, unsigned char *out,
                       unsigned long outlen, int windowBits) {
    z_stream strm;
    double t0, t1;
    int ret;

    memset(&strm, 0, sizeof(strm));
    if (inflateInit2(&strm, windowBits) != Z_OK)
        return -1;
    strm.next_in = in;
    strm.avail_in = (uInt)inlen;
    strm.next_out = out;
    strm.avail_out = (uInt)outlen;
    t0 = now();
    ret = inflate(&strm, Z_FINISH);
    t1 = now();
    if (ret != Z_STREAM_END || strm.total_out != outlen) {
        inflateEnd(&strm);
        return -1;
    }
    inflateEnd(&strm);
    return t1 - t0;
}

static double run_chunk(unsigned char *in, long inlen, unsigned char *out,
                        unsigned long outlen, int windowBits, unsigned chunk) {
    z_stream strm;
    unsigned long done = 0;
    double t0, t1;
    int ret;

    memset(&strm, 0, sizeof(strm));
    if (inflateInit2(&strm, windowBits) != Z_OK)
        return -1;
    strm.next_in = in;
    strm.avail_in = (uInt)inlen;
    t0 = now();
    do {
        unsigned left = (unsigned)(outlen - done);
        unsigned n = chunk < left ? chunk : left;
        strm.next_out = out + done;
        strm.avail_out = n;
        ret = inflate(&strm, Z_NO_FLUSH);
        done += n - strm.avail_out;
    } while (ret == Z_OK || ret == Z_BUF_ERROR);
    t1 = now();
    inflateEnd(&strm);
    if (ret != Z_STREAM_END || done != outlen)
        return -1;
    return t1 - t0;
}

static double run_back(unsigned char *in, long inlen, unsigned char *out,
                       unsigned long outlen, unsigned char *win) {
    z_stream strm;
    struct in_state ind;
    struct out_state outd;
    double t0, t1;
    int ret;

    memset(&strm, 0, sizeof(strm));
    if (inflateBackInit(&strm, 15, win) != Z_OK)
        return -1;
    ind.in = in;
    ind.len = (unsigned)inlen;
    ind.used = 0;
    outd.out = out;
    outd.cap = (unsigned)outlen;
    outd.used = 0;
    strm.next_in = Z_NULL;
    strm.avail_in = 0;
    t0 = now();
    ret = inflateBack(&strm, pull_all, &ind, push_all, &outd);
    t1 = now();
    inflateBackEnd(&strm);
    if (ret != Z_STREAM_END || outd.used != (unsigned)outlen)
        return -1;
    return t1 - t0;
}

static void usage(const char *argv0) {
    fprintf(stderr, "usage: %s [--mode full|chunk|back] [--chunk N] "
                    "[--iters I] [--warmup W] [--clock cpu|mono] "
                    "file out_bytes\n", argv0);
}

int main(int argc, char **argv) {
    const char *mode = "full";
    const char *path;
    unsigned chunk = 16384;
    int iters = 25, warmup = 3;
    unsigned long outlen;
    unsigned char *in, *out, *win = NULL;
    long inlen;
    int windowBits, i, a, back;
    double t, times[MAX_ITERS];
    double med, p10, p90, best;

    a = 1;
    while (a + 1 < argc && argv[a][0] == '-') {
        if (strcmp(argv[a], "--mode") == 0)
            mode = argv[++a];
        else if (strcmp(argv[a], "--chunk") == 0)
            chunk = (unsigned)strtoul(argv[++a], NULL, 10);
        else if (strcmp(argv[a], "--iters") == 0)
            iters = atoi(argv[++a]);
        else if (strcmp(argv[a], "--warmup") == 0)
            warmup = atoi(argv[++a]);
        else if (strcmp(argv[a], "--clock") == 0)
            use_cpu_clock = strcmp(argv[++a], "mono") != 0;
        else {
            usage(argv[0]);
            return 1;
        }
        a++;
    }
    if (a + 2 != argc || iters < 1 || iters > MAX_ITERS || warmup < 0 ||
        chunk == 0) {
        usage(argv[0]);
        return 1;
    }
    path = argv[a];
    outlen = strtoul(argv[a + 1], NULL, 10);
    back = strcmp(mode, "back") == 0;
    if (outlen == 0 || (!back && strcmp(mode, "full") != 0 &&
                        strcmp(mode, "chunk") != 0)) {
        usage(argv[0]);
        return 1;
    }

    in = read_file(path, &inlen);
    if (in == NULL)
        return 1;
    out = malloc(outlen);
    if (out == NULL) {
        fprintf(stderr, "out of memory\n");
        return 1;
    }
    if (back) {
        win = malloc(WINSIZE);
        if (win == NULL) {
            fprintf(stderr, "out of memory\n");
            return 1;
        }
        windowBits = -15;
    }
    else if (inlen >= 2 && in[0] == 0x1f && in[1] == 0x8b)
        windowBits = 15 + 16;
    else
        windowBits = 15;

    for (i = 0; i < warmup + iters; i++) {
        if (back)
            t = run_back(in, inlen, out, outlen, win);
        else if (strcmp(mode, "chunk") == 0)
            t = run_chunk(in, inlen, out, outlen, windowBits, chunk);
        else
            t = run_full(in, inlen, out, outlen, windowBits);
        if (t < 0) {
            fprintf(stderr, "inflate failed (mode=%s)\n", mode);
            return 1;
        }
        if (i >= warmup)
            times[i - warmup] = t;
    }

    qsort(times, (size_t)iters, sizeof(double), cmp_double);
    best = times[0];
    med = times[iters / 2];
    p10 = times[(iters - 1) * 10 / 100];
    p90 = times[(iters - 1) * 90 / 100];
    if (med <= 0.0) {
        fprintf(stderr, "clock resolution too coarse (median=%.9f)\n", med);
        return 1;
    }
    printf("mode=%s chunk=%u median=%.9f p10=%.9f p90=%.9f best=%.9f "
           "mbs=%.2f iters=%d clock=%s\n",
           mode, strcmp(mode, "chunk") == 0 ? chunk : 0u, med, p10, p90, best,
           (outlen / 1e6) / med, iters, use_cpu_clock ? "cpu" : "mono");
    return 0;
}
