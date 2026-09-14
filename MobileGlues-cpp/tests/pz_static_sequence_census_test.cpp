#include "gl/pz_static_sequence_census.h"

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

static std::string last_log;
static int failures = 0;

extern "C" void write_log(const char* format, ...) {
    char line[8192] = {};
    va_list args;
    va_start(args, format);
    std::vsnprintf(line, sizeof(line), format, args);
    va_end(args);
    last_log = line;
}
extern "C" void write_log_n(const char*, ...) {}
int __android_log_print(int, const char*, const char*, ...) { return 0; }

static void expect(bool condition, const char* message) {
    if (condition) return;
    std::printf("FAIL %s\n", message);
    ++failures;
}

static mg_pz_static_draw_t draw(unsigned id) {
    mg_pz_static_draw_t d{};
    d.indexed = true;
    d.mode = GL_TRIANGLES;
    d.count = 6 + static_cast<GLsizei>(id % 11);
    d.instances = 1;
    d.type = GL_UNSIGNED_SHORT;
    d.index_token = static_cast<uint64_t>(id) * 12U;
    d.program = 7 + (id % 3);
    d.vao = 2;
    d.array_buffer = 10 + (id % 5);
    d.element_buffer = 20 + (id % 7);
    d.draw_framebuffer = 3;
    d.texture_known_mask = 0xff;
    d.array_content_known = true;
    d.array_lifetime = 1000 + (id % 5);
    d.array_version = 1;
    d.array_size = 4096;
    d.element_content_known = true;
    d.element_lifetime = 2000 + (id % 7);
    d.element_version = 1;
    d.element_size = 2048;
    for (unsigned unit = 0; unit < MG_PZ_STATIC_TEXTURE_UNITS; ++unit) {
        d.texture_2d[unit] = 100 + unit + id * 5;
        d.texture_2d_array[unit] = 0;
    }
    return d;
}

static void submit_frame(unsigned begin, unsigned count) {
    for (unsigned i = 0; i < count; ++i) {
        const auto d = draw(begin + i);
        mg_pz_static_sequence_draw(d);
    }
    mg_pz_static_sequence_present();
}

int main() {
    unsetenv("MOBILEGLUES_PZ_STATIC_SEQUENCE_CENSUS");
    mg_pz_static_sequence_init();
    expect(!mg_pz_static_sequence_census_active, "static sequence census must default off");

    setenv("MOBILEGLUES_PZ_STATIC_SEQUENCE_CENSUS", "true", 1);
    mg_pz_static_sequence_init();
    expect(!mg_pz_static_sequence_census_active, "only exact value 1 may enable static sequence census");

    setenv("MOBILEGLUES_PZ_STATIC_SEQUENCE_CENSUS", "1", 1);
    mg_pz_static_sequence_init();
    expect(mg_pz_static_sequence_census_active, "1 must enable static sequence census");

    // 300 identical 32-draw frames: after the first frame every draw should be
    // stable at the same position and reusable in 16-draw blocks.
    for (unsigned frame = 0; frame < 300; ++frame) submit_frame(0, 32);
    expect(last_log.find("ZOMDROID_PZ_STATIC_SEQ schema=1") != std::string::npos,
           "V5 report line must be emitted");
    expect(last_log.find("frames=300") != std::string::npos, "report interval must be 300 frames");
    expect(last_log.find("draws=9600") != std::string::npos, "draws must be aggregated");
    expect(last_log.find("overflow=0") != std::string::npos, "bounded capture must report no overflow here");
    expect(last_log.find("shape_pos_repeat=9568") != std::string::npos,
           "identical frames must repeat every structural position after frame one");
    expect(last_log.find("tracked_resource_repeat=9568") != std::string::npos,
           "identical buffer content identities must repeat after frame one");
    expect(last_log.find("tracked_persist100=6432") != std::string::npos,
           "100-frame persistence bucket must advance deterministically");



    // Unknown tracked texture state must not be counted as strict content reuse.
    mg_pz_static_sequence_init();
    for (unsigned frame = 0; frame < 300; ++frame) {
        for (unsigned i = 0; i < 32; ++i) {
            auto d = draw(i);
            if (frame != 0) d.texture_known_mask = 0x0f;
            mg_pz_static_sequence_draw(d);
        }
        mg_pz_static_sequence_present();
    }
    expect(last_log.find("tracked_resource_repeat=0") != std::string::npos,
           "unknown texture bindings must invalidate strict content reuse");

    // Structural draw shapes may repeat even when the streamed buffer content
    // changes every frame. The strict content metric must reject those frames.
    mg_pz_static_sequence_init();
    for (unsigned frame = 0; frame < 300; ++frame) {
        for (unsigned i = 0; i < 32; ++i) {
            auto d = draw(i);
            d.array_version = frame + 1;
            mg_pz_static_sequence_draw(d);
        }
        mg_pz_static_sequence_present();
    }
    expect(last_log.find("shape_pos_repeat=9568") != std::string::npos,
           "shape census must still see repeated draw structure");
    expect(last_log.find("tracked_resource_repeat=0") != std::string::npos,
           "content census must reject changing buffer versions");

    // Re-init resets history. Insert one draw at the start of every alternating
    // frame. Same-position matches collapse, but shifted 16-draw windows remain
    // discoverable by block reuse.
    mg_pz_static_sequence_init();
    for (unsigned frame = 0; frame < 300; ++frame) {
        if ((frame & 1U) != 0) {
            const auto extra = draw(9999);
            mg_pz_static_sequence_draw(extra);
        }
        for (unsigned i = 0; i < 32; ++i) {
            const auto d = draw(i);
            mg_pz_static_sequence_draw(d);
        }
        mg_pz_static_sequence_present();
    }
    const std::size_t block_pos = last_log.find("block_repeat=");
    expect(block_pos != std::string::npos, "block reuse must be reported");
    if (block_pos != std::string::npos) {
        const unsigned long long block_repeat = std::strtoull(last_log.c_str() + block_pos + 13, nullptr, 10);
        expect(block_repeat > 7000, "shifted stable sequences must still be detected by block reuse");
    }

    // A context change must break cross-context history.
    mg_pz_static_sequence_init();
    submit_frame(0, 32);
    mg_pz_static_sequence_context_changed(77);
    for (unsigned frame = 1; frame < 300; ++frame) submit_frame(0, 32);
    expect(last_log.find("context_resets=1") != std::string::npos,
           "context changes must be explicit and invalidate history");

    for (unsigned frame = 0; frame < 300; ++frame) submit_frame(0, 32);
    expect(last_log.find("context_resets=0") != std::string::npos,
           "context reset count must be window-local rather than cumulative");

    // Diagnostic storage must stay tightly bounded even if a pathological frame
    // submits far more draws than a normal PZ frame. Capture the first 16K and
    // account for the rest explicitly rather than growing on the render path.
    mg_pz_static_sequence_init();
    submit_frame(0, 17000);
    for (unsigned frame = 1; frame < 300; ++frame) submit_frame(0, 1);
    expect(last_log.find("highwater=16384") != std::string::npos,
           "capture storage must be capped at 16384 draws per frame");
    expect(last_log.find("overflow=616") != std::string::npos,
           "draws above the capture limit must be reported as overflow");

    std::printf("%s (%d failures)\n", failures ? "FAILED" : "PZ static sequence census checks passed", failures);
    return failures != 0;
}
