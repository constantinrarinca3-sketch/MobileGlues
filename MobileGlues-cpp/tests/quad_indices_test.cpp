#include "gl/quad_indices.h"

#include <array>
#include <cstdio>

static int failures = 0;

static void expect(const char* name, std::uint32_t got, std::uint32_t wanted) {
    if (got != wanted) {
        std::printf("FAIL %s: got=%u wanted=%u\n", name, got, wanted);
        ++failures;
    }
}

int main() {
    expect("capacity ignores incomplete quad", mg_quad_detail::triangle_index_capacity(11), 12);

    std::array<std::uint32_t, 12> sequence{};
    expect("sequence output", mg_quad_detail::expand_sequence(sequence.data(), 11, 4), 12);
    const std::array<std::uint32_t, 12> expected_sequence = {4, 5, 6, 4, 6, 7, 8, 9, 10, 8, 10, 11};
    for (std::size_t i = 0; i < sequence.size(); ++i) {
        expect("sequence index", sequence[i], expected_sequence[i]);
    }

    const std::array<std::uint16_t, 11> indexed = {0, 1, 99, 4, 5, 6, 7, 8, 9, 10, 11};
    std::array<std::uint32_t, 12> triangles{};
    expect("restart output",
           mg_quad_detail::expand_elements(triangles.data(), indexed.data(), indexed.size(), 20, true, 99), 12);
    const std::array<std::uint32_t, 12> expected_triangles = {
        24, 25, 26, 24, 26, 27, 28, 29, 30, 28, 30, 31,
    };
    for (std::size_t i = 0; i < triangles.size(); ++i) {
        expect("indexed triangle", triangles[i], expected_triangles[i]);
    }

    std::printf("%s (%d failures)\n", failures ? "FAILED" : "quad index checks passed", failures);
    return failures != 0;
}
