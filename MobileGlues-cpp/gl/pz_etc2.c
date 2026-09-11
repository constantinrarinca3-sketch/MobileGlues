// MobileGlues - gl/pz_etc2.c
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v2.1:
//   https://www.gnu.org/licenses/old-licenses/lgpl-2.1.txt
// SPDX-License-Identifier: LGPL-2.1-only
// End of Source File Header
//
// CPU ETC2 encoder used by the opt-in Project Zomboid texture path. Colour
// blocks use ETC1-compatible ETC2 modes; RGBA textures add an EAC alpha block.
//
// Field lessons inherited from the RimDroid deployment, do not relearn them:
//  - the encoder must be compiled optimized (this tree builds -O2 globally, verified);
//    at -O0 the full search once turned an atlas bake into a SIGKILLed 10-minute stall;
//  - uniform blocks (flat fills, atlas padding) take a one-quantization fast path;
//  - the ETC1 table search tries only the 3 tables bracketing the actual max deviation;
//  - cumulative encode time is counted, because a slowness complaint needs a NUMBER.
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "pz_etc2.h"

// Cumulative wall time spent inside zetc2_etc2_encode, in milliseconds. The Tecno report ("fps
// halved with ETC2") needs a NUMBER, not a theory: the caller logs this every N textures, so a
// field log states the price outright — and doubles as the build fingerprint (version strings
// don't change between test builds; the watchdog taught us that lesson).
static uint64_t zetc2_etc2_total_ns = 0;
static uint64_t zetc2_encode_n = 0;
uint64_t mg_pz_etc2_total_ms(void) { return zetc2_etc2_total_ns / 1000000u; }
uint64_t mg_pz_etc2_encode_count(void) { return zetc2_encode_n; }

static const int zetc2_etc1_mod[8][2] = {{2,8},{5,17},{9,29},{13,42},{18,60},{24,80},{33,106},{47,183}};
static const int8_t zetc2_eac_mod[16][8] = {
    {-3,-6,-9,-15,2,5,8,14},{-3,-7,-10,-13,2,6,9,12},{-2,-5,-8,-13,1,4,7,12},{-2,-4,-6,-13,1,3,5,12},
    {-3,-6,-8,-12,2,5,7,11},{-3,-7,-9,-11,2,6,8,10},{-4,-7,-8,-11,3,6,7,10},{-3,-5,-8,-11,2,4,7,10},
    {-2,-6,-8,-10,1,5,7,9},{-2,-5,-8,-10,1,4,7,9},{-2,-4,-8,-10,1,3,7,9},{-2,-5,-7,-10,1,4,6,9},
    {-3,-4,-7,-10,2,3,6,9},{-1,-2,-3,-10,0,1,2,9},{-4,-6,-8,-9,3,5,7,8},{-3,-5,-7,-9,2,4,6,8}};

static inline int zetc2_clamp255(int v) { return v < 0 ? 0 : (v > 255 ? 255 : v); }

/* Encode one 4x4 RGB block (px[16][3], col-major p = x*4+y) into ETC1-subset bits. */
static void zetc2_etc1_encode_block(const uint8_t px[16][3], uint8_t out[8]) {
    // Fast path: a uniform block needs no search — both subblocks share the quantized base,
    // every pixel takes the smallest modifier (+2 from table 0; max error 2 per channel).
    int uniform = 1;
    for (int p = 1; p < 16 && uniform; p++)
        uniform = px[p][0] == px[0][0] && px[p][1] == px[0][1] && px[p][2] == px[0][2];
    if (uniform) {
        int b5[3];
        for (int k = 0; k < 3; k++) b5[k] = (px[0][k] * 31 + 127) / 255;
        out[0] = (uint8_t)(b5[0] << 3);          /* delta 0 */
        out[1] = (uint8_t)(b5[1] << 3);
        out[2] = (uint8_t)(b5[2] << 3);
        out[3] = 2;                              /* tables 0/0, diff mode, no flip */
        out[4] = 0; out[5] = 0;                  /* all indices 00 = +a (+2) */
        out[6] = 0; out[7] = 0;
        return;
    }
    int best_err = 0x7fffffff;
    for (int flip = 0; flip < 2; flip++) {
        int avg[2][3] = {{0,0,0},{0,0,0}};
        for (int p = 0; p < 16; p++) {
            int sub = flip ? ((p & 3) >> 1) : (p >> 3);
            for (int k = 0; k < 3; k++) avg[sub][k] += px[p][k];
        }
        for (int s = 0; s < 2; s++) for (int k = 0; k < 3; k++) avg[s][k] = (avg[s][k] + 4) / 8;
        int b5[2][3], diff_ok = 1;
        for (int k = 0; k < 3; k++) {
            b5[0][k] = (avg[0][k] * 31 + 127) / 255;
            b5[1][k] = (avg[1][k] * 31 + 127) / 255;
            int d = b5[1][k] - b5[0][k];
            if (d < -4 || d > 3) diff_ok = 0;
        }
        int base[2][3];
        if (diff_ok) {
            for (int s = 0; s < 2; s++) for (int k = 0; k < 3; k++)
                base[s][k] = (b5[s][k] << 3) | (b5[s][k] >> 2);
        } else {
            for (int s = 0; s < 2; s++) for (int k = 0; k < 3; k++) {
                int q = (avg[s][k] * 15 + 127) / 255;
                base[s][k] = (q << 4) | q;
            }
        }
        int tbl[2] = {0,0}; uint32_t idx[2] = {0,0}; int err_total = 0;
        for (int s = 0; s < 2; s++) {
            // Table preselect: the tables are sorted by magnitude; only the ones bracketing the
            // subblock's max per-channel deviation from base can win. Try [t-1, t, t+1].
            int maxdev = 0;
            for (int p = 0; p < 16; p++) {
                int sub = flip ? ((p & 3) >> 1) : (p >> 3);
                if (sub != s) continue;
                for (int k = 0; k < 3; k++) {
                    int d = px[p][k] - base[s][k];
                    if (d < 0) d = -d;
                    if (d > maxdev) maxdev = d;
                }
            }
            int t_pick = 7;
            for (int t = 0; t < 8; t++) if (zetc2_etc1_mod[t][1] >= maxdev) { t_pick = t; break; }
            int t_lo = t_pick > 0 ? t_pick - 1 : 0;
            int t_hi = t_pick < 7 ? t_pick + 1 : 7;
            int best_sub = 0x7fffffff, best_t = t_lo; uint32_t best_idx = 0;
            for (int t = t_lo; t <= t_hi; t++) {
                int e_sum = 0; uint32_t ind = 0;
                for (int p = 0; p < 16; p++) {
                    int sub = flip ? ((p & 3) >> 1) : (p >> 3);
                    if (sub != s) continue;
                    int be = 0x7fffffff, bi = 0;
                    for (int m = 0; m < 4; m++) {
                        int mod = (m & 1) ? zetc2_etc1_mod[t][1] : zetc2_etc1_mod[t][0];
                        if (m & 2) mod = -mod;
                        int e = 0;
                        for (int k = 0; k < 3; k++) {
                            int d = px[p][k] - zetc2_clamp255(base[s][k] + mod);
                            e += d * d;
                        }
                        if (e < be) { be = e; bi = ((m & 2) ? 2 : 0) | (m & 1); }
                    }
                    e_sum += be;
                    ind |= (uint32_t)bi << (2 * p);
                }
                if (e_sum < best_sub) { best_sub = e_sum; best_t = t; best_idx = ind; }
            }
            tbl[s] = best_t; idx[s] = best_idx; err_total += best_sub;
        }
        if (err_total < best_err) {
            best_err = err_total;
            uint8_t o[8] = {0};
            if (diff_ok) {
                o[0] = (uint8_t)((b5[0][0] << 3) | ((b5[1][0] - b5[0][0]) & 7));
                o[1] = (uint8_t)((b5[0][1] << 3) | ((b5[1][1] - b5[0][1]) & 7));
                o[2] = (uint8_t)((b5[0][2] << 3) | ((b5[1][2] - b5[0][2]) & 7));
            } else {
                o[0] = (uint8_t)(((base[0][0] >> 4) << 4) | (base[1][0] >> 4));
                o[1] = (uint8_t)(((base[0][1] >> 4) << 4) | (base[1][1] >> 4));
                o[2] = (uint8_t)(((base[0][2] >> 4) << 4) | (base[1][2] >> 4));
            }
            o[3] = (uint8_t)((tbl[0] << 5) | (tbl[1] << 2) | (diff_ok ? 2 : 0) | flip);
            uint16_t msb = 0, lsb = 0;
            uint32_t all = idx[0] | idx[1];
            for (int p = 0; p < 16; p++) {
                uint32_t bi = (all >> (2 * p)) & 3;
                if (bi & 2) msb |= (uint16_t)(1u << p);
                if (bi & 1) lsb |= (uint16_t)(1u << p);
            }
            o[4] = (uint8_t)(msb >> 8); o[5] = (uint8_t)(msb & 0xff);
            o[6] = (uint8_t)(lsb >> 8); o[7] = (uint8_t)(lsb & 0xff);
            memcpy(out, o, 8);
        }
    }
}

/* Encode one 4x4 alpha block (a[16], col-major) into EAC bits. Bounded search. */
static void zetc2_eac_encode_block(const uint8_t a[16], uint8_t out[8]) {
    int amin = 255, amax = 0;
    for (int p = 0; p < 16; p++) { if (a[p] < amin) amin = a[p]; if (a[p] > amax) amax = a[p]; }
    if (amin == amax) {   // flat alpha (fully opaque sprites dominate) — exact, no search
        out[0] = (uint8_t)amin;
        out[1] = (uint8_t)((1 << 4) | 13);       /* mult 1, table 13 has a 0 modifier at index 4 */
        uint64_t bits = 0;
        for (int p = 0; p < 16; p++) bits |= (uint64_t)4 << (45 - 3 * p);
        for (int i = 0; i < 6; i++) out[2 + i] = (uint8_t)(bits >> (40 - 8 * i));
        return;
    }
    int best_err = 0x7fffffff;
    for (int t = 0; t < 16; t++) {
        int span = zetc2_eac_mod[t][7] - zetc2_eac_mod[t][3];
        int m0 = span ? (amax - amin + span / 2) / span : 1;
        for (int dm = -1; dm <= 1; dm++) {
            int mult = m0 + dm; if (mult < 1) mult = 1; if (mult > 15) mult = 15;
            int b0 = (amin + amax) / 2 - mult * (zetc2_eac_mod[t][3] + zetc2_eac_mod[t][7]) / 2;
            for (int db = -1; db <= 1; db++) {
                int base = zetc2_clamp255(b0 + db);
                int e_sum = 0; uint8_t sel[16];
                for (int p = 0; p < 16; p++) {
                    int be = 0x7fffffff, bs = 0;
                    for (int s = 0; s < 8; s++) {
                        int v = zetc2_clamp255(base + mult * zetc2_eac_mod[t][s]);
                        int d = a[p] - v, e = d * d;
                        if (e < be) { be = e; bs = s; }
                    }
                    e_sum += be; sel[p] = (uint8_t)bs;
                }
                if (e_sum < best_err) {
                    best_err = e_sum;
                    out[0] = (uint8_t)base;
                    out[1] = (uint8_t)((mult << 4) | t);
                    uint64_t bits = 0;
                    for (int p = 0; p < 16; p++) bits |= (uint64_t)sel[p] << (45 - 3 * p);
                    for (int i = 0; i < 6; i++) out[2 + i] = (uint8_t)(bits >> (40 - 8 * i));
                }
            }
        }
    }
}

/* ---------------------------------------------------------------------------
 * Parallel block encoding (2026-09-02). Blocks are independent, so the first run's
 * 50 seconds of encoding (measured: 6150 textures, 49.5 s on a Snapdragon 8 Elite)
 * splits across cores with no change to the output: every block is encoded by the
 * same function from the same pixels, only the block ROW is handed out by an atomic
 * counter. A persistent pool of workers waits on a condition variable between
 * textures -- creating threads per texture would cost more than it saves on the
 * thousands of small mip levels -- and small images (fewer than ZETC2_PAR_MIN_BLOCKS
 * blocks) are encoded on the calling thread alone. MOBILEGLUES_PZ_ETC2_THREADS overrides
 * the thread count; 1 disables the pool.
 * --------------------------------------------------------------------------- */
#include <pthread.h>
#include <stdatomic.h>
#include <unistd.h>

#define ZETC2_PAR_MIN_BLOCKS 1024   /* ~128x128: below this the hand-off is not worth it */
#define ZETC2_MAX_THREADS    16

typedef struct {
    const uint8_t* rgba; int w, h, stride, bw, bh, bs, alpha; uint8_t* out;
} zetc2_job_t;

static void zetc2_encode_rows(const zetc2_job_t* j, atomic_int* next_row) {
    for (;;) {
        int by = atomic_fetch_add(next_row, 1);
        if (by >= j->bh) return;
        for (int bx = 0; bx < j->bw; bx++) {
            uint8_t px[16][3], al[16];
            for (int p = 0; p < 16; p++) {                   /* col-major p = x*4+y, edge clamp */
                int x = bx * 4 + (p >> 2), y = by * 4 + (p & 3);
                if (x >= j->w) x = j->w - 1;
                if (y >= j->h) y = j->h - 1;
                const uint8_t* s = j->rgba + ((size_t)y * j->w + x) * j->stride;
                px[p][0] = s[0]; px[p][1] = s[1]; px[p][2] = s[2];
                al[p] = (j->stride == 4) ? s[3] : 255;
            }
            uint8_t* o = j->out + ((size_t)by * j->bw + bx) * j->bs;
            if (j->alpha) { zetc2_eac_encode_block(al, o); zetc2_etc1_encode_block(px, o + 8); }
            else zetc2_etc1_encode_block(px, o);
        }
    }
}

/* Pool state. One job at a time: the encoder is called from the GL thread only. */
static pthread_mutex_t zetc2_pool_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  zetc2_pool_cv = PTHREAD_COND_INITIALIZER;   /* workers wait here */
static pthread_cond_t  zetc2_done_cv = PTHREAD_COND_INITIALIZER;   /* caller waits here */
static int zetc2_workers = -1;           /* -1 = not initialised, 0 = serial */
static unsigned zetc2_generation = 0;    /* bumped per job; workers wake on change */
static int zetc2_busy = 0;               /* workers still inside the current job */
static const zetc2_job_t* zetc2_cur_job = NULL;
static atomic_int zetc2_cur_row;

static void* zetc2_worker(void* arg) {
    (void)arg;
    unsigned seen = 0;
    pthread_mutex_lock(&zetc2_pool_mu);
    for (;;) {
        while (zetc2_generation == seen) pthread_cond_wait(&zetc2_pool_cv, &zetc2_pool_mu);
        seen = zetc2_generation;
        const zetc2_job_t* j = zetc2_cur_job;
        pthread_mutex_unlock(&zetc2_pool_mu);
        if (j) zetc2_encode_rows(j, &zetc2_cur_row);
        pthread_mutex_lock(&zetc2_pool_mu);
        if (--zetc2_busy == 0) pthread_cond_signal(&zetc2_done_cv);
    }
    return NULL;
}

static int zetc2_pool_size(void) {
    if (zetc2_workers >= 0) return zetc2_workers;
    int n = 0;
    const char* e = getenv("MOBILEGLUES_PZ_ETC2_THREADS");
    if (e && e[0]) n = atoi(e);
    if (n <= 0) {
        long c = sysconf(_SC_NPROCESSORS_ONLN);
        n = (c > 1) ? (int)c : 1;
    }
    if (n > ZETC2_MAX_THREADS) n = ZETC2_MAX_THREADS;
    int workers = n - 1;                 /* the caller is one of the encoders */
    int started = 0;
    for (int i = 0; i < workers; i++) {
        pthread_t t;
        pthread_attr_t a;
        pthread_attr_init(&a);
        pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
        if (pthread_create(&t, &a, zetc2_worker, NULL) == 0) started++;
        pthread_attr_destroy(&a);
    }
    zetc2_workers = started;
    return started;
}

static void zetc2_encode_parallel(const zetc2_job_t* j) {
    int workers = ((size_t)j->bw * j->bh >= ZETC2_PAR_MIN_BLOCKS) ? zetc2_pool_size() : 0;
    atomic_store(&zetc2_cur_row, 0);
    if (workers > 0) {
        pthread_mutex_lock(&zetc2_pool_mu);
        zetc2_cur_job = j;
        zetc2_busy = workers;
        zetc2_generation++;
        pthread_cond_broadcast(&zetc2_pool_cv);
        pthread_mutex_unlock(&zetc2_pool_mu);
    }
    zetc2_encode_rows(j, &zetc2_cur_row);   /* the caller works too */
    if (workers > 0) {
        pthread_mutex_lock(&zetc2_pool_mu);
        while (zetc2_busy > 0) pthread_cond_wait(&zetc2_done_cv, &zetc2_pool_mu);
        zetc2_cur_job = NULL;
        pthread_mutex_unlock(&zetc2_pool_mu);
    }
}

const void* mg_pz_etc2_encode(uint32_t etc2fmt, int32_t w, int32_t h, const uint8_t* rgba, int stride,
                              size_t* out_sz) {
    if (!rgba || !out_sz || w <= 0 || h <= 0 || (stride != 3 && stride != 4)) return NULL;
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    static _Thread_local uint8_t* ebuf = NULL; static _Thread_local size_t ecap = 0;
    const int alpha = (etc2fmt == 0x9278u || etc2fmt == 0x9279u);
    const int bs = alpha ? 16 : 8;
    const int bw = (w + 3) / 4, bh = (h + 3) / 4;
    size_t need = (size_t)bw * bh * bs;
    if (!need || need > (128u << 20)) return NULL;
    if (ecap < need) { void* nb = realloc(ebuf, need); if (!nb) return NULL; ebuf = (uint8_t*)nb; ecap = need; }
    zetc2_job_t job = { rgba, w, h, stride, bw, bh, bs, alpha, ebuf };
    zetc2_encode_parallel(&job);
    *out_sz = need;
    zetc2_encode_n++;
    clock_gettime(CLOCK_MONOTONIC, &t1);
    zetc2_etc2_total_ns += (uint64_t)(t1.tv_sec - t0.tv_sec) * 1000000000u + (uint64_t)(t1.tv_nsec - t0.tv_nsec);
    return ebuf;
}

// ---------------------------------------------------------------------------
// Disk cache: measured 2026-08-31, one session cost 52.8 SECONDS of encoding
// (6324 textures) paid in-frame during world streaming. Content-addressed --
// the stream has no stable names, so the key is a 64-bit hash of the pixels
// (+ format/size in the seed). One file per texture, written atomically
// (tmp+rename), validated on read; anything wrong = silent miss + rewrite.
// Identical tiles deduplicate by construction. The caller selects whether the
// cache is used; this file only implements the cached operation.
// ---------------------------------------------------------------------------
#include <stdio.h>
#include <sys/stat.h>
#include <errno.h>
#include <dirent.h>
#include <fcntl.h>
#include <limits.h>

static uint64_t zetc2_hits = 0;
static uint64_t zetc2_evicted = 0;
uint64_t mg_pz_etc2_cache_hit_count(void) { return zetc2_hits; }
uint64_t mg_pz_etc2_eviction_count(void) { return zetc2_evicted; }

// ---- cap & LRU eviction (default 1.5 GB, MOBILEGLUES_PZ_ETC2_CACHE_MB overrides) ----
// The working set is live: one of her sessions HIT nearly the whole 879 MB store,
// so the cap must clear the working set with headroom or the cache silently thrashes
// into permanent re-encoding. Hits touch the file's mtime; eviction drops the oldest
// mtime first. An evicted texture just re-encodes on next sight -- nothing breaks.
static long long zetc2_total = -1; // bytes on disk, -1 = not measured yet
static long long zetc2_cap(void) {
    static long long cap = -1;
    if (cap < 0) {
        const char* e = getenv("MOBILEGLUES_PZ_ETC2_CACHE_MB");
        long mb = e ? atol(e) : 1536;
        if (mb < 64) mb = 64;
        cap = (long long)mb << 20;
    }
    return cap;
}
typedef struct { long long mtime; long long size; char name[24]; } zetc2_ent;
static int zetc2_ent_cmp(const void* a, const void* b) {
    long long d = ((const zetc2_ent*)a)->mtime - ((const zetc2_ent*)b)->mtime;
    return d < 0 ? -1 : (d > 0 ? 1 : 0);
}
static void zetc2_scan_evict(const char* dir) {
    DIR* d = opendir(dir);
    if (!d) return;
    zetc2_ent* v = NULL;
    size_t n = 0, capn = 0;
    long long total = 0;
    struct dirent* de;
    char path[PATH_MAX];
    while ((de = readdir(d))) {
        size_t ln = strlen(de->d_name);
        if (ln < 4 || ln >= sizeof(v->name) || strcmp(de->d_name + ln - 3, ".e2")) continue;
        const int path_len = snprintf(path, sizeof(path), "%s/%s", dir, de->d_name);
        if (path_len < 0 || (size_t)path_len >= sizeof(path)) continue;
        struct stat st;
        if (stat(path, &st)) continue;
        if (n == capn) {
            capn = capn ? capn * 2 : 1024;
            void* nv = realloc(v, capn * sizeof(zetc2_ent));
            if (!nv) break;
            v = (zetc2_ent*)nv;
        }
        v[n].mtime = st.st_mtime;
        v[n].size = st.st_size;
        memcpy(v[n].name, de->d_name, ln + 1);
        n++;
        total += st.st_size;
    }
    closedir(d);
    if (v && total > zetc2_cap()) {
        // oldest first; evict down to 15/16 of the cap so one new file does not
        // immediately trigger the next full scan
        qsort(v, n, sizeof(zetc2_ent), zetc2_ent_cmp);
        long long floor_ = zetc2_cap() / 16 * 15;
        for (size_t i = 0; i < n && total > floor_; i++) {
            const int path_len = snprintf(path, sizeof(path), "%s/%s", dir, v[i].name);
            if (path_len < 0 || (size_t)path_len >= sizeof(path)) continue;
            if (!remove(path)) {
                total -= v[i].size;
                zetc2_evicted++;
            }
        }
    }
    free(v);
    zetc2_total = total;
}
static uint64_t zetc2_io_ns = 0;
uint64_t mg_pz_etc2_io_ms(void) { return zetc2_io_ns / 1000000u; }

/* Content hash of a texture's pixels: the cache key. Eight independent lanes, each a
 * xor-multiply-shift chain over every eighth 8-byte word, folded together at the end.
 * A single chain is bound by the latency of the 64-bit multiply and hashed a 4 MB
 * texture in 1.3 ms; eight lanes hide that latency and run at memory speed, 0.28 ms
 * (measured 2026-09-03 on the test phone: 3.2 -> 15 GB/s). Over the ~6000 textures of
 * a session that is the difference between 8 s and under 2 s of cache overhead.
 * Deterministic: no threads, no dependence on the core count. Changing this function
 * changes every cache key -- NG_GL4ES and the Mesa (ZINK) build must carry the same one. */
static uint64_t zetc2_hash(const uint8_t* p, size_t n, uint64_t seed) {
    enum { L = 8 };
    uint64_t lane[L];
    for (int l = 0; l < L; l++)
        lane[l] = (seed ^ 0x9e3779b97f4a7c15ull) + (uint64_t)l * 0x9e3779b97f4a7c15ull;
    size_t i = 0;
    for (; i + 8 * L <= n; i += 8 * L) {
        for (int l = 0; l < L; l++) {
            uint64_t k;
            memcpy(&k, p + i + 8 * l, 8);
            lane[l] ^= k;
            lane[l] *= 0xff51afd7ed558ccdull;
            lane[l] ^= lane[l] >> 29;
        }
    }
    uint64_t hh = 0;
    for (int l = 0; l < L; l++) {
        hh ^= lane[l];
        hh *= 0xff51afd7ed558ccdull;
        hh ^= hh >> 29;
    }
    for (; i + 8 <= n; i += 8) {
        uint64_t k;
        memcpy(&k, p + i, 8);
        hh ^= k;
        hh *= 0xff51afd7ed558ccdull;
        hh ^= hh >> 29;
    }
    for (; i < n; i++) {
        hh ^= p[i];
        hh *= 0x100000001b3ull;
    }
    hh ^= hh >> 32;
    return hh;
}

const void* mg_pz_etc2_cached(uint32_t etc2fmt, int32_t w, int32_t h, const uint8_t* rgba, int stride,
                              size_t* out_sz) {
    if (!rgba || !out_sz || w <= 0 || h <= 0 || w > 65535 || h > 65535 || (stride != 3 && stride != 4)) return NULL;
    static _Thread_local uint8_t* lbuf = NULL;
    static _Thread_local size_t lcap = 0;
    static int dir_ok = -1;
    // Same store NG_GL4ES uses, so the two renderers share one cache: same encoder, same
    // hash, same file layout. The launcher passes the real app-data path (it can be
    // /data/user/0/... rather than /data/data/...); the literal is the NG default.
    static const char* dir = NULL;
    static char dir_from_mg[PATH_MAX];
    if (!dir) {
        const char* e = getenv("MOBILEGLUES_PZ_ETC2_CACHE_DIR");
        if (e && e[0]) {
            dir = e;
        } else {
            // MG_DIR_PATH is supplied by the launcher and already points into
            // the current package's private storage. Using it avoids baking an
            // obsolete Android package name into the renderer.
            const char* mg_dir = getenv("MG_DIR_PATH");
            if (mg_dir && mg_dir[0]) {
                const int n = snprintf(dir_from_mg, sizeof(dir_from_mg), "%s/etc2-cache", mg_dir);
                if (n > 0 && (size_t)n < sizeof(dir_from_mg)) dir = dir_from_mg;
            }
            if (!dir) dir = "/data/data/com.zomdroid.macos/files/mobileglues-zomdroid/etc2-cache";
        }
    }
    if (dir_ok < 0) dir_ok = (mkdir(dir, 0700) == 0 || errno == EEXIST) ? 1 : 0;
    if (!dir_ok) return mg_pz_etc2_encode(etc2fmt, w, h, rgba, stride, out_sz);
    // Small levels skip the disk. A mip chain is encoded whole because its format may not
    // mix, so the tails come through here too: on one session 2628 of 6151 cache files were
    // under a kilobyte (32x32 and below), each costing a hash, an open, a read and an mtime
    // touch for a texture that encodes in microseconds. Measured cost of the cache was 8.5 s
    // of I/O per session against 0.26 s of encoding; this is where most of it went.
    if (w <= 64 && h <= 64) return mg_pz_etc2_encode(etc2fmt, w, h, rgba, stride, out_sz);

    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    uint64_t hh = zetc2_hash(rgba, (size_t)w * h * stride,
                             ((uint64_t)etc2fmt << 32) ^ ((uint64_t)(uint32_t)w << 16) ^ (uint32_t)h);
    char path[PATH_MAX];
    const int path_len = snprintf(path, sizeof(path), "%s/%016llx.e2", dir, (unsigned long long)hh);
    if (path_len < 0 || (size_t)path_len >= sizeof(path))
        return mg_pz_etc2_encode(etc2fmt, w, h, rgba, stride, out_sz);

    FILE* f = fopen(path, "rb");
    if (f) {
        uint32_t hdr[4]; // magic 'NGE2', fmt, w<<16|h, payload bytes
        if (fread(hdr, sizeof(hdr), 1, f) == 1 && hdr[0] == 0x4e474532u && hdr[1] == etc2fmt &&
            hdr[2] == (((uint32_t)w << 16) | (uint32_t)h) && hdr[3] > 0 && hdr[3] <= (128u << 20)) {
            if (lcap < hdr[3]) {
                void* nb = realloc(lbuf, hdr[3]);
                if (nb) {
                    lbuf = (uint8_t*)nb;
                    lcap = hdr[3];
                }
            }
            if (lcap >= hdr[3] && fread(lbuf, 1, hdr[3], f) == hdr[3]) {
                // LRU: a hit makes the file young again -- but not on every hit. Touching
                // the mtime is a metadata write, and a session hits ~6000 files; eviction
                // only cares about days, so a file touched within the last day is left alone.
                struct stat fst;
                int stale = fstat(fileno(f), &fst) != 0 || (time(NULL) - fst.st_mtime) > 86400;
                fclose(f);
                zetc2_hits++;
                if (stale) utimensat(AT_FDCWD, path, NULL, 0);
                *out_sz = hdr[3];
                clock_gettime(CLOCK_MONOTONIC, &t1);
                zetc2_io_ns += (uint64_t)(t1.tv_sec - t0.tv_sec) * 1000000000u + (uint64_t)(t1.tv_nsec - t0.tv_nsec);
                return lbuf;
            }
        }
        fclose(f); // damaged or stale: treated as a miss, re-encoded and overwritten below
    }

    uint64_t enc_before = zetc2_etc2_total_ns;
    const void* blocks = mg_pz_etc2_encode(etc2fmt, w, h, rgba, stride, out_sz);
    if (blocks) {
        char tmp[PATH_MAX];
        const int tmp_len = snprintf(tmp, sizeof(tmp), "%s.tmp", path);
        if (tmp_len < 0 || (size_t)tmp_len >= sizeof(tmp)) return blocks;
        FILE* o = fopen(tmp, "wb");
        if (o) {
            uint32_t hdr[4] = {0x4e474532u, etc2fmt, ((uint32_t)w << 16) | (uint32_t)h, (uint32_t)*out_sz};
            int ok = fwrite(hdr, sizeof(hdr), 1, o) == 1 && fwrite(blocks, 1, *out_sz, o) == *out_sz;
            ok = (fclose(o) == 0) && ok;
            if (ok) {
                rename(tmp, path);
                if (zetc2_total < 0) zetc2_scan_evict(dir); // first write: measure what is there
                else {
                    zetc2_total += (long long)sizeof(uint32_t) * 4 + (long long)*out_sz;
                    if (zetc2_total > zetc2_cap()) zetc2_scan_evict(dir);
                }
            } else
                remove(tmp);
        }
    }
    clock_gettime(CLOCK_MONOTONIC, &t1);
    // io time = everything spent here except the encode itself (it counts its own time)
    uint64_t total = (uint64_t)(t1.tv_sec - t0.tv_sec) * 1000000000u + (uint64_t)(t1.tv_nsec - t0.tv_nsec);
    uint64_t enc = zetc2_etc2_total_ns - enc_before;
    zetc2_io_ns += (total > enc) ? (total - enc) : 0;
    return blocks;
}
