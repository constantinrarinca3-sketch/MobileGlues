#include "gl/pz_uniform_location_cache.h"

#include <cstdio>

static int failures = 0;

static void expect(bool condition, const char* message) {
    if (condition) return;
    std::printf("FAIL %s\n", message);
    ++failures;
}

int main() {
    mg_pz_uniform_location_cache cache;
    GLint location = -1;

    expect(!cache.lookup(1, 7, "WOffset", &location), "an empty cache must miss");
    expect(cache.store(1, 7, "WOffset", 3), "a positive location must be cached");
    expect(cache.lookup(1, 7, "WOffset", &location) && location == 3,
           "the same program and name must hit");
    expect(!cache.lookup(1, 8, "WOffset", &location), "programs must not share locations");
    expect(!cache.store(1, 7, "missing", -1), "negative locations must not be cached");
    expect(!cache.lookup(1, 7, "missing", &location), "negative locations must keep reaching the driver");

    cache.forget_program(7);
    expect(!cache.lookup(1, 7, "WOffset", &location), "relink or delete must invalidate the program");

    expect(cache.store(1, 7, "WViewport", 4), "the cache must accept a new value after invalidation");
    expect(!cache.lookup(2, 7, "WViewport", &location), "a context switch must clear cached locations");
    expect(!cache.store(0, 7, "WOffset", 3), "calls without a tracked context must not be cached");

    std::printf("%s (%d failures)\n", failures ? "FAILED" : "uniform-location cache checks passed", failures);
    return failures != 0;
}
