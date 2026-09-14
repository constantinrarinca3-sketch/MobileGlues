#include "pz_static_sequence_census.h"

#include "log.h"

#include <algorithm>
#include <atomic>
#include <array>
#include <cstdlib>
#include <cstring>
#include <vector>

bool mg_pz_static_sequence_census_active = false;

namespace {

using count_t = unsigned long long;
constexpr uint32_t kReportFrames = 300;
constexpr std::size_t kCaptureLimit = 16384;
constexpr std::size_t kBlockDraws = 16;

struct block_ref_t {
    uint64_t hash = 0;
    uint32_t index = 0;
};

struct window_t {
    count_t frames = 0;
    count_t draws = 0;
    count_t captured = 0;
    count_t overflow = 0;
    count_t compared_frames = 0;
    count_t shape_pos_repeat = 0;
    count_t block_repeat = 0;
    count_t repeatable = 0;
    count_t resource_known = 0;
    count_t resource_repeat = 0;
    count_t persist2 = 0;
    count_t persist10 = 0;
    count_t persist100 = 0;
    count_t context_resets = 0;
    count_t longest_pos = 0;
    count_t longest_block = 0;
    count_t highwater = 0;
};

struct draw_sig_t {
    uint64_t shape = 0;
    uint64_t resource = 0;
    bool resource_known = false;
};

struct state_t {
    std::vector<draw_sig_t> current;
    std::vector<draw_sig_t> previous;
    std::vector<uint16_t> current_streak;
    std::vector<uint16_t> previous_streak;
    std::vector<uint8_t> block_mask;
    std::vector<uint8_t> repeat_mask;
    std::vector<block_ref_t> previous_blocks;
    count_t frame_draws = 0;
    count_t frame_overflow = 0;
    unsigned long long context_id = 0;
    bool have_previous = false;
    window_t window;
};

thread_local state_t g_state;
std::atomic<bool> g_process_initialized{false};

bool exact_opt_in(const char* name) {
    const char* value = std::getenv(name);
    return value != nullptr && std::strcmp(value, "1") == 0;
}

uint64_t mix64(uint64_t h, uint64_t value) {
    value ^= value >> 30U;
    value *= 0xbf58476d1ce4e5b9ULL;
    value ^= value >> 27U;
    value *= 0x94d049bb133111ebULL;
    value ^= value >> 31U;
    h ^= value + 0x9e3779b97f4a7c15ULL + (h << 6U) + (h >> 2U);
    return h;
}

uint64_t shape_signature(const mg_pz_static_draw_t& d) {
    uint64_t h = 0x6a09e667f3bcc909ULL;
    h = mix64(h, d.indexed ? 1U : 0U);
    h = mix64(h, d.mode);
    h = mix64(h, static_cast<uint32_t>(d.count));
    h = mix64(h, static_cast<uint32_t>(d.instances));
    h = mix64(h, d.type);
    h = mix64(h, static_cast<uint32_t>(d.first));
    h = mix64(h, static_cast<uint32_t>(d.basevertex));
    h = mix64(h, d.baseinstance);
    h = mix64(h, d.index_token);
    h = mix64(h, d.program);
    h = mix64(h, d.vao);
    h = mix64(h, d.array_buffer);
    h = mix64(h, d.element_buffer);
    h = mix64(h, d.draw_framebuffer);
    h = mix64(h, d.texture_known_mask);
    for (unsigned unit = 0; unit < MG_PZ_STATIC_TEXTURE_UNITS; ++unit) {
        h = mix64(h, d.texture_2d[unit]);
        h = mix64(h, d.texture_2d_array[unit]);
    }
    return h;
}

draw_sig_t draw_signature(const mg_pz_static_draw_t& d) {
    draw_sig_t out{};
    out.shape = shape_signature(d);
    const bool buffers_known = d.array_content_known && (!d.indexed || d.element_content_known);
    const bool textures_known = d.texture_known_mask == 0xffU;
    out.resource_known = buffers_known && textures_known;
    uint64_t h = out.shape;
    if (out.resource_known) {
        h = mix64(h, d.array_lifetime);
        h = mix64(h, d.array_version);
        h = mix64(h, d.array_size);
        if (d.indexed) {
            h = mix64(h, d.element_lifetime);
            h = mix64(h, d.element_version);
            h = mix64(h, d.element_size);
        }
    }
    out.resource = h;
    return out;
}

uint64_t block_hash(const std::vector<draw_sig_t>& values, std::size_t start) {
    uint64_t h = 0x510e527fade682d1ULL;
    for (std::size_t i = 0; i < kBlockDraws; ++i) h = mix64(h, values[start + i].shape);
    return h;
}

bool block_equal(const std::vector<draw_sig_t>& a, std::size_t ai,
                 const std::vector<draw_sig_t>& b, std::size_t bi) {
    for (std::size_t i = 0; i < kBlockDraws; ++i) {
        if (a[ai + i].shape != b[bi + i].shape) return false;
    }
    return true;
}

void clear_history(state_t& state) {
    state.current.clear();
    state.previous.clear();
    state.current_streak.clear();
    state.previous_streak.clear();
    state.block_mask.clear();
    state.repeat_mask.clear();
    state.previous_blocks.clear();
    state.frame_draws = 0;
    state.frame_overflow = 0;
    state.have_previous = false;
}

void reserve_storage(state_t& state) {
    state.current.reserve(kCaptureLimit);
    state.previous.reserve(kCaptureLimit);
    state.current_streak.reserve(kCaptureLimit);
    state.previous_streak.reserve(kCaptureLimit);
    state.block_mask.reserve(kCaptureLimit);
    state.repeat_mask.reserve(kCaptureLimit);
    state.previous_blocks.reserve(kCaptureLimit);
}

void build_previous_blocks(state_t& state) {
    state.previous_blocks.clear();
    if (state.previous.size() < kBlockDraws) return;
    const std::size_t count = state.previous.size() - kBlockDraws + 1;
    state.previous_blocks.resize(count);
    for (std::size_t i = 0; i < count; ++i) {
        state.previous_blocks[i].hash = block_hash(state.previous, i);
        state.previous_blocks[i].index = static_cast<uint32_t>(i);
    }
    std::sort(state.previous_blocks.begin(), state.previous_blocks.end(),
              [](const block_ref_t& a, const block_ref_t& b) {
                  if (a.hash != b.hash) return a.hash < b.hash;
                  return a.index < b.index;
              });
}

count_t mark_block_reuse(state_t& state) {
    state.block_mask.assign(state.current.size(), 0);
    if (state.current.size() < kBlockDraws || state.previous.size() < kBlockDraws) return 0;

    build_previous_blocks(state);
    for (std::size_t start = 0; start + kBlockDraws <= state.current.size(); ++start) {
        const uint64_t hash = block_hash(state.current, start);
        const block_ref_t key{hash, 0};
        auto lower = std::lower_bound(state.previous_blocks.begin(), state.previous_blocks.end(), key,
                                      [](const block_ref_t& a, const block_ref_t& b) { return a.hash < b.hash; });
        bool matched = false;
        for (auto it = lower; it != state.previous_blocks.end() && it->hash == hash; ++it) {
            if (block_equal(state.current, start, state.previous, it->index)) {
                matched = true;
                break;
            }
        }
        if (!matched) continue;
        for (std::size_t i = 0; i < kBlockDraws; ++i) state.block_mask[start + i] = 1;
    }

    count_t count = 0;
    for (uint8_t v : state.block_mask) count += v != 0;
    return count;
}

count_t longest_true_run(const std::vector<uint8_t>& mask) {
    count_t best = 0;
    count_t run = 0;
    for (uint8_t v : mask) {
        if (v) {
            ++run;
            best = std::max(best, run);
        } else {
            run = 0;
        }
    }
    return best;
}

void report_and_reset(state_t& state) {
    const double repeat_pct = state.window.captured
                                  ? 100.0 * static_cast<double>(state.window.repeatable) /
                                        static_cast<double>(state.window.captured)
                                  : 0.0;
    const double resource_pct = state.window.resource_known
                                   ? 100.0 * static_cast<double>(state.window.resource_repeat) /
                                         static_cast<double>(state.window.resource_known)
                                   : 0.0;
    LOG_I("ZOMDROID_PZ_STATIC_SEQ schema=1 frames=%llu compared=%llu draws=%llu captured=%llu overflow=%llu "
          "shape_pos_repeat=%llu block_repeat=%llu repeatable=%llu repeat_pct=%.2f "
          "tracked_resource_known=%llu tracked_resource_repeat=%llu tracked_resource_pct=%.2f "
          "tracked_persist2=%llu tracked_persist10=%llu tracked_persist100=%llu longest_pos=%llu longest_block=%llu "
          "highwater=%llu context_resets=%llu",
          state.window.frames, state.window.compared_frames, state.window.draws, state.window.captured,
          state.window.overflow, state.window.shape_pos_repeat, state.window.block_repeat, state.window.repeatable,
          repeat_pct, state.window.resource_known, state.window.resource_repeat, resource_pct,
          state.window.persist2, state.window.persist10, state.window.persist100,
          state.window.longest_pos, state.window.longest_block, state.window.highwater, state.window.context_resets);
    state.window = {};
}

} // namespace

void mg_pz_static_sequence_init(void) {
#if defined(ZOMDROID_EXPERIMENTAL)
    mg_pz_static_sequence_census_active = exact_opt_in("MOBILEGLUES_PZ_STATIC_SEQUENCE_CENSUS");
    g_state = {};
    if (mg_pz_static_sequence_census_active) reserve_storage(g_state);
    g_process_initialized.store(true, std::memory_order_release);
#else
    mg_pz_static_sequence_census_active = false;
    g_process_initialized.store(true, std::memory_order_release);
#endif
}

void mg_pz_static_sequence_ensure_initialized(void) {
    if (g_process_initialized.load(std::memory_order_acquire)) return;
    mg_pz_static_sequence_init();
}

void mg_pz_static_sequence_context_changed(unsigned long long context_id) {
#if defined(ZOMDROID_EXPERIMENTAL)
    if (!mg_pz_static_sequence_census_active) return;
    if (g_state.context_id == context_id) return;
    g_state.context_id = context_id;
    clear_history(g_state);
    reserve_storage(g_state);
    ++g_state.window.context_resets;
#else
    (void)context_id;
#endif
}

void mg_pz_static_sequence_draw(const mg_pz_static_draw_t& draw) {
#if defined(ZOMDROID_EXPERIMENTAL)
    if (!mg_pz_static_sequence_census_active) return;
    ++g_state.frame_draws;
    if (g_state.current.size() >= kCaptureLimit) {
        ++g_state.frame_overflow;
        return;
    }
    g_state.current.push_back(draw_signature(draw));
#else
    (void)draw;
#endif
}

void mg_pz_static_sequence_present(void) {
#if defined(ZOMDROID_EXPERIMENTAL)
    if (!mg_pz_static_sequence_census_active) return;

    state_t& state = g_state;
    window_t& window = state.window;
    ++window.frames;
    window.draws += state.frame_draws;
    window.captured += state.current.size();
    window.overflow += state.frame_overflow;
    window.highwater = std::max<count_t>(window.highwater, state.current.size());

    state.current_streak.assign(state.current.size(), 1);
    state.repeat_mask.assign(state.current.size(), 0);

    count_t pos_repeat = 0;
    count_t longest_pos = 0;
    if (state.have_previous) {
        ++window.compared_frames;
        const std::size_t common = std::min(state.current.size(), state.previous.size());
        count_t run = 0;
        for (std::size_t i = 0; i < common; ++i) {
            if (state.current[i].shape == state.previous[i].shape) {
                ++pos_repeat;
                state.repeat_mask[i] = 1;
                ++run;
                longest_pos = std::max(longest_pos, run);
            } else {
                run = 0;
            }
            const bool resource_equal = state.current[i].resource_known && state.previous[i].resource_known &&
                                       state.current[i].resource == state.previous[i].resource;
            if (resource_equal) {
                ++window.resource_repeat;
                const uint32_t prior = i < state.previous_streak.size() ? state.previous_streak[i] : 1U;
                state.current_streak[i] = static_cast<uint16_t>(std::min<uint32_t>(65535U, prior + 1U));
            }
        }

        const count_t block_repeat = mark_block_reuse(state);
        window.block_repeat += block_repeat;
        for (std::size_t i = 0; i < state.block_mask.size(); ++i) {
            if (state.block_mask[i]) state.repeat_mask[i] = 1;
        }
        window.longest_block = std::max(window.longest_block, longest_true_run(state.block_mask));
    } else {
        state.block_mask.assign(state.current.size(), 0);
    }

    window.shape_pos_repeat += pos_repeat;
    window.longest_pos = std::max(window.longest_pos, longest_pos);

    for (std::size_t i = 0; i < state.current_streak.size(); ++i) {
        if (state.current[i].resource_known) ++window.resource_known;
        const uint16_t streak = state.current_streak[i];
        if (streak >= 2) ++window.persist2;
        if (streak >= 10) ++window.persist10;
        if (streak >= 100) ++window.persist100;
        if (state.repeat_mask[i]) ++window.repeatable;
    }

    state.previous.swap(state.current);
    state.previous_streak.swap(state.current_streak);
    state.current.clear();
    state.current_streak.clear();
    state.frame_draws = 0;
    state.frame_overflow = 0;
    state.have_previous = true;

    if (window.frames >= kReportFrames) report_and_reset(state);
#endif
}
