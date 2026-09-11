// MobileGlues - gl/threaded_submission.cpp
// High-risk ZomDroid experiment: execute backend GL on a dedicated thread.

#include "threaded_submission.h"

#if defined(ZOMDROID_EXPERIMENTAL)

#include "log.h"
#include "pz_census.h"
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>

namespace mg_ts {
namespace {

constexpr uint64_t kPacketCapacity = 1024;
constexpr uint64_t kPacketMask = kPacketCapacity - 1;
constexpr size_t kCommandsPerPacket = 32;
constexpr size_t kPacketPayloadBytes = kCommandsPerPacket * kCommandPayloadBytes;
static_assert((kPacketCapacity & kPacketMask) == 0);

struct packet_entry {
    command_fn execute = nullptr;
    command_fn destroy = nullptr;
    uint32_t payload_offset = 0;
    command_kind kind = command_kind::barrier;
    uint8_t segment = 0;
    coalesce_key key{};
    bool superseded = false;
};

struct alignas(std::max_align_t) command_packet {
    uint32_t count = 0;
    uint8_t segment_count = 0;
    std::array<packet_entry, kCommandsPerPacket> entries;
    alignas(std::max_align_t) unsigned char payload[kPacketPayloadBytes];
};

struct context_binding {
    EGLDisplay display = EGL_NO_DISPLAY;
    EGLSurface draw = EGL_NO_SURFACE;
    EGLSurface read = EGL_NO_SURFACE;
    EGLContext context = EGL_NO_CONTEXT;
    egl_bind_api_fn bind_api = nullptr;
    egl_make_current_fn make_current = nullptr;
    egl_release_thread_fn release_thread = nullptr;
};

enum class control_request : uint8_t { none, adopt, release, stop };

class submission_state {
  public:
    bool isActive() const { return active_.load(std::memory_order_acquire); }

    reservation reserveCommand(command_fn execute, command_fn destroy, size_t payload_size,
                               size_t payload_alignment, command_kind kind, coalesce_key key) {
        if (payload_size > kCommandPayloadBytes || payload_alignment == 0 ||
            payload_alignment > alignof(std::max_align_t) || (payload_alignment & (payload_alignment - 1)) != 0)
            return {nullptr, 0};

        size_t payload_offset = alignUp(pending_payload_bytes_, payload_alignment);
        if (pending_count_ == kCommandsPerPacket || payload_offset + payload_size > kPacketPayloadBytes) {
            flushProducerPacket();
            payload_offset = 0;
        }

        waitForPacketSpace();
        command_packet& packet = queue_[producer_head_ & kPacketMask];
        packet_entry& entry = packet.entries[pending_count_];
        entry.execute = execute;
        entry.destroy = destroy;
        entry.payload_offset = static_cast<uint32_t>(payload_offset);
        entry.kind = kind;
        entry.segment = pending_segment_;
        entry.key = key;
        entry.superseded = false;
        reserved_kind_ = kind;
        reserved_key_ = key;
        reserved_payload_end_ = payload_offset + payload_size;
        reservation_open_ = true;
        return {packet.payload + payload_offset, producer_head_ + 1};
    }

    void publishCommand(uint64_t sequence) {
        if (!reservation_open_ || sequence != producer_head_ + 1) return;
        reservation_open_ = false;
        if ((reserved_kind_ == command_kind::state || reserved_kind_ == command_kind::uniform) &&
            reserved_key_.valid()) {
            command_packet& packet = queue_[producer_head_ & kPacketMask];
            for (size_t index = pending_count_; index-- > 0;) {
                packet_entry& previous = packet.entries[index];
                if (previous.segment != pending_segment_) break;
                if (!previous.superseded && previous.kind == reserved_kind_ &&
                    previous.key == reserved_key_) {
                    previous.superseded = true;
                    if (reserved_kind_ == command_kind::state)
                        ++state_commands_dropped_;
                    else
                        ++uniform_commands_dropped_;
                    break;
                }
            }
        }
        pending_payload_bytes_ = reserved_payload_end_;
        ++pending_count_;
        if (endsRendererSegment(reserved_kind_)) ++pending_segment_;

        if (mg_pz_census_active) ++submitted_commands_;

        if (pending_count_ == kCommandsPerPacket) flushProducerPacket();
    }

    void flushPending() {
        if (ownsForCaller()) flushProducerPacket();
    }

    void waitFor(uint64_t sequence, bool record_wait = true) {
        if (sequence == 0) return;
        flushProducerPacket();
        uint64_t completed = tail_.load(std::memory_order_acquire);
        if (completed >= sequence) return;
        const bool record_telemetry = record_wait && mg_pz_census_active;
        if (record_telemetry) synchronous_waits_.fetch_add(1, std::memory_order_relaxed);
        const auto started = record_telemetry ? std::chrono::steady_clock::now()
                                              : std::chrono::steady_clock::time_point{};
        while (completed < sequence) {
            tail_.wait(completed, std::memory_order_acquire);
            completed = tail_.load(std::memory_order_acquire);
        }
        if (record_telemetry) recordSynchronousWait(started);
    }

    bool adopt(const context_binding& binding) {
        if (!mg_pz_threaded_submission_active || binding.context == EGL_NO_CONTEXT || binding.bind_api == nullptr ||
            binding.make_current == nullptr) {
            return false;
        }
        if (isActive()) {
            // A second EGL thread may own a different shared context while the
            // render thread is active. It must keep submitting directly on its
            // own context; stealing this single-producer queue would unbind the
            // first thread's context and strand it in the next synchronous call.
            if (owner_ != std::this_thread::get_id()) {
                if (mg_pz_census_active)
                    LOG_W_FORCE("ZOMDROID_PZ_THREADED_SUBMISSION bypass reason=second_context_thread")
                return false;
            }
            if (!release()) return false;
        }
        if (!ensureWorker()) return false;

        if (binding.make_current(binding.display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT) != EGL_TRUE) {
            disable("producer_release_failed");
            return false;
        }

        std::unique_lock<std::mutex> lock(control_mutex_);
        binding_ = binding;
        control_done_ = false;
        request_ = control_request::adopt;
        lock.unlock();
        signalWorker();
        lock.lock();
        control_cv_.wait(lock, [this] { return control_done_; });
        const bool adopted = control_result_;
        if (!adopted) {
            lock.unlock();
            binding.make_current(binding.display, binding.draw, binding.read, binding.context);
            disable("worker_adopt_failed");
            return false;
        }

        owner_ = std::this_thread::get_id();
        active_.store(true, std::memory_order_release);
        if (mg_pz_census_active) {
            LOG_I("ZOMDROID_PZ_THREADED_SUBMISSION active=1 context=%p owner=%llu packet_slots=%llu "
                  "commands_per_packet=%llu frame_depth=1",
                  binding.context,
                  static_cast<unsigned long long>(std::hash<std::thread::id>{}(owner_)),
                  static_cast<unsigned long long>(kPacketCapacity),
                  static_cast<unsigned long long>(kCommandsPerPacket))
        }
        return true;
    }

    bool release() {
        if (!isActive()) return true;
        flushProducerPacket();
        const uint64_t target = head_.load(std::memory_order_acquire);
        waitFor(target, false);
        active_.store(false, std::memory_order_release);

        std::unique_lock<std::mutex> lock(control_mutex_);
        control_done_ = false;
        request_ = control_request::release;
        lock.unlock();
        signalWorker();
        lock.lock();
        control_cv_.wait(lock, [this] { return control_done_; });
        const bool result = control_result_;
        last_swap_sequence_ = 0;
        if (!result) disable("worker_release_failed");
        return result;
    }

    void stopWorker() {
        release();
        std::unique_lock<std::mutex> lock(control_mutex_);
        if (!worker_.joinable()) return;
        control_done_ = false;
        request_ = control_request::stop;
        lock.unlock();
        signalWorker();
        worker_.join();
        lock.lock();
        request_ = control_request::none;
        control_done_ = true;
    }

    bool ownsForCaller() const {
        return isActive() && owner_ == std::this_thread::get_id();
    }

    uint64_t previousSwapSequence() const { return last_swap_sequence_; }
    void setLastSwapSequence(uint64_t sequence) { last_swap_sequence_ = sequence; }

    void recordFrameThrottle(const std::chrono::steady_clock::time_point& started) {
        if (!mg_pz_census_active) return;
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
        frame_wait_us_.fetch_add(static_cast<uint64_t>(ms * 1000.0), std::memory_order_relaxed);
        uint64_t maximum = frame_wait_max_us_.load(std::memory_order_relaxed);
        const uint64_t us = static_cast<uint64_t>(ms * 1000.0);
        while (us > maximum &&
               !frame_wait_max_us_.compare_exchange_weak(maximum, us, std::memory_order_relaxed)) {
        }
    }

    void recordSwap(bool succeeded) {
        if (!mg_pz_census_active) return;
        swaps_completed_.fetch_add(1, std::memory_order_relaxed);
        if (!succeeded) swap_failures_.fetch_add(1, std::memory_order_relaxed);
    }

    uint64_t nextSwapNumber() {
        return mg_pz_census_active ? swaps_submitted_.fetch_add(1, std::memory_order_relaxed) + 1 : 0;
    }

    void report(uint64_t frames) {
        if (!mg_pz_census_active) return;
        if (frames != 60 && frames % 300 != 0) return;
        const uint64_t sync_waits = synchronous_waits_.load(std::memory_order_relaxed);
        const uint64_t frame_wait_us = frame_wait_us_.load(std::memory_order_relaxed);
        const uint64_t packets = packets_submitted_;
        const uint64_t submitted = submitted_commands_;
        LOG_I("ZOMDROID_PZ_THREADED_SUBMISSION frames=%llu submitted=%llu executed=%llu packets=%llu "
              "packet_avg=%.2f sync_waits=%llu sync_avg_ms=%.3f sync_max_ms=%.3f queue_waits=%llu "
              "queue_wait_avg_ms=%.3f queue_wait_max_ms=%.3f frame_wait_avg_ms=%.3f "
              "frame_wait_max_ms=%.3f queue_highwater=%llu packet_highwater=%llu swap_done=%llu swap_fail=%llu "
              "renderer=segments:%llu/state:%llu/uniform:%llu/resource:%llu/draw:%llu/barrier:%llu/"
              "drop_s:%llu/drop_u:%llu",
              static_cast<unsigned long long>(frames),
              static_cast<unsigned long long>(submitted),
              static_cast<unsigned long long>(executed_.load(std::memory_order_relaxed)),
              static_cast<unsigned long long>(packets),
              packets == 0 ? 0.0 : static_cast<double>(submitted) / static_cast<double>(packets),
              static_cast<unsigned long long>(sync_waits),
              sync_waits == 0 ? 0.0
                              : static_cast<double>(synchronous_wait_us_.load(std::memory_order_relaxed)) /
                                    (1000.0 * static_cast<double>(sync_waits)),
              static_cast<double>(synchronous_wait_max_us_.load(std::memory_order_relaxed)) / 1000.0,
              static_cast<unsigned long long>(producer_waits_.load(std::memory_order_relaxed)),
              producer_waits_.load(std::memory_order_relaxed) == 0
                  ? 0.0
                  : static_cast<double>(producer_wait_us_.load(std::memory_order_relaxed)) /
                        (1000.0 * static_cast<double>(producer_waits_.load(std::memory_order_relaxed))),
              static_cast<double>(producer_wait_max_us_.load(std::memory_order_relaxed)) / 1000.0,
              frames == 0 ? 0.0 : static_cast<double>(frame_wait_us) / (1000.0 * static_cast<double>(frames)),
              static_cast<double>(frame_wait_max_us_.load(std::memory_order_relaxed)) / 1000.0,
              static_cast<unsigned long long>(queue_highwater_),
              static_cast<unsigned long long>(packet_highwater_),
              static_cast<unsigned long long>(swaps_completed_.load(std::memory_order_relaxed)),
              static_cast<unsigned long long>(swap_failures_.load(std::memory_order_relaxed)),
              static_cast<unsigned long long>(renderer_segments_.load(std::memory_order_relaxed)),
              static_cast<unsigned long long>(renderer_state_.load(std::memory_order_relaxed)),
              static_cast<unsigned long long>(renderer_uniform_.load(std::memory_order_relaxed)),
              static_cast<unsigned long long>(renderer_resource_.load(std::memory_order_relaxed)),
              static_cast<unsigned long long>(renderer_draw_.load(std::memory_order_relaxed)),
              static_cast<unsigned long long>(renderer_barrier_.load(std::memory_order_relaxed)),
              static_cast<unsigned long long>(state_commands_dropped_),
              static_cast<unsigned long long>(uniform_commands_dropped_))
    }

  private:
    static size_t alignUp(size_t value, size_t alignment) { return (value + alignment - 1) & ~(alignment - 1); }

    void waitForPacketSpace() {
        if (pending_count_ != 0) return;
        const uint64_t head = producer_head_;
        std::chrono::steady_clock::time_point wait_started{};
        bool blocked = false;
        uint64_t completed = tail_.load(std::memory_order_acquire);
        while (head - completed >= kPacketCapacity) {
            if (!blocked) {
                blocked = true;
                if (mg_pz_census_active) wait_started = std::chrono::steady_clock::now();
            }
            tail_.wait(completed, std::memory_order_acquire);
            completed = tail_.load(std::memory_order_acquire);
        }
        if (blocked && mg_pz_census_active) recordProducerWait(wait_started);
    }

    uint64_t flushProducerPacket() {
        if (pending_count_ == 0) return producer_head_;
        command_packet& packet = queue_[producer_head_ & kPacketMask];
        packet.count = static_cast<uint32_t>(pending_count_);
        packet.segment_count = packet.entries[pending_count_ - 1].segment + 1;
        const uint64_t sequence = ++producer_head_;
        if (mg_pz_census_active) {
            ++packets_submitted_;
            const uint64_t completed_packets = tail_.load(std::memory_order_acquire);
            const uint64_t packet_depth = sequence - completed_packets;
            packet_highwater_ = std::max(packet_highwater_, packet_depth);
            const uint64_t executed_commands = executed_.load(std::memory_order_relaxed);
            const uint64_t command_depth = submitted_commands_ >= executed_commands
                                               ? submitted_commands_ - executed_commands
                                               : 0;
            queue_highwater_ = std::max(queue_highwater_, command_depth);
        }

        head_.store(sequence, std::memory_order_release);
        pending_count_ = 0;
        pending_payload_bytes_ = 0;
        reserved_payload_end_ = 0;
        pending_segment_ = 0;
        signalWorker();
        return sequence;
    }

    void signalWorker() {
        wake_counter_.fetch_add(1, std::memory_order_release);
        wake_counter_.notify_one();
    }

    bool ensureWorker() {
        std::lock_guard<std::mutex> lock(control_mutex_);
        if (disabled_) return false;
        if (worker_.joinable()) return true;
        try {
            if (!queue_) queue_ = std::make_unique<command_packet[]>(kPacketCapacity);
            worker_ = std::thread(&submission_state::workerLoop, this);
            return true;
        } catch (...) {
            disabled_ = true;
            LOG_W_FORCE("ZOMDROID_PZ_THREADED_SUBMISSION disabled reason=worker_start_failed")
            return false;
        }
    }

    void workerLoop() {
        uint64_t consumer_tail = tail_.load(std::memory_order_relaxed);
        for (;;) {
            // Read the event generation before checking either work source. If
            // a publisher wins any race after this point, atomic::wait observes
            // a changed value and cannot sleep through the notification.
            const uint64_t wake_value = wake_counter_.load(std::memory_order_acquire);
            const uint64_t available = head_.load(std::memory_order_acquire);
            if (consumer_tail < available) {
                command_packet& packet = queue_[consumer_tail & kPacketMask];
                const uint32_t count = packet.count;
                if (mg_pz_census_active) {
                    renderer_segments_.fetch_add(packet.segment_count, std::memory_order_relaxed);
                    for (uint32_t index = 0; index < count; ++index) {
                        switch (packet.entries[index].kind) {
                        case command_kind::state:
                            renderer_state_.fetch_add(1, std::memory_order_relaxed);
                            break;
                        case command_kind::uniform:
                            renderer_uniform_.fetch_add(1, std::memory_order_relaxed);
                            break;
                        case command_kind::program_state:
                            renderer_state_.fetch_add(1, std::memory_order_relaxed);
                            break;
                        case command_kind::resource_write:
                            renderer_resource_.fetch_add(1, std::memory_order_relaxed);
                            break;
                        case command_kind::draw:
                            renderer_draw_.fetch_add(1, std::memory_order_relaxed);
                            break;
                        case command_kind::barrier:
                            renderer_barrier_.fetch_add(1, std::memory_order_relaxed);
                            break;
                        }
                    }
                }
                for (uint32_t index = 0; index < count; ++index) {
                    packet_entry& entry = packet.entries[index];
                    void* payload = packet.payload + entry.payload_offset;
                    if (!entry.superseded) entry.execute(payload);
                    entry.destroy(payload);
                }
                ++consumer_tail;
                if (mg_pz_census_active) executed_.fetch_add(count, std::memory_order_relaxed);
                tail_.store(consumer_tail, std::memory_order_release);
                tail_.notify_all();
                continue;
            }

            control_request request = control_request::none;
            context_binding binding;
            {
                std::unique_lock<std::mutex> lock(control_mutex_);
                if (request_ == control_request::none) {
                    lock.unlock();
                    wake_counter_.wait(wake_value, std::memory_order_acquire);
                    continue;
                }
                request = request_;
                binding = binding_;
                request_ = control_request::none;
            }

            bool result = true;
            if (request == control_request::adopt) {
                result = binding.bind_api(EGL_OPENGL_ES_API) == EGL_TRUE &&
                         binding.make_current(binding.display, binding.draw, binding.read, binding.context) == EGL_TRUE;
            } else if (request == control_request::release) {
                result = binding.make_current(binding.display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT) ==
                         EGL_TRUE;
                if (!result && binding.release_thread != nullptr) result = binding.release_thread() == EGL_TRUE;
            } else if (request == control_request::stop) {
                std::lock_guard<std::mutex> lock(control_mutex_);
                control_result_ = true;
                control_done_ = true;
                control_cv_.notify_all();
                return;
            }

            {
                std::lock_guard<std::mutex> lock(control_mutex_);
                control_result_ = result;
                control_done_ = true;
            }
            control_cv_.notify_all();
        }
    }

    void disable(const char* reason) {
        active_.store(false, std::memory_order_release);
        {
            std::lock_guard<std::mutex> lock(control_mutex_);
            disabled_ = true;
        }
        LOG_W_FORCE("ZOMDROID_PZ_THREADED_SUBMISSION disabled reason=%s", reason)
    }

    void recordProducerWait(const std::chrono::steady_clock::time_point& started) {
        const uint64_t us = static_cast<uint64_t>(
            std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - started).count());
        producer_waits_.fetch_add(1, std::memory_order_relaxed);
        producer_wait_us_.fetch_add(us, std::memory_order_relaxed);
        uint64_t maximum = producer_wait_max_us_.load(std::memory_order_relaxed);
        while (us > maximum &&
               !producer_wait_max_us_.compare_exchange_weak(maximum, us, std::memory_order_relaxed)) {
        }
    }

    void recordSynchronousWait(const std::chrono::steady_clock::time_point& started) {
        const uint64_t us = static_cast<uint64_t>(
            std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - started).count());
        synchronous_wait_us_.fetch_add(us, std::memory_order_relaxed);
        uint64_t maximum = synchronous_wait_max_us_.load(std::memory_order_relaxed);
        while (us > maximum &&
               !synchronous_wait_max_us_.compare_exchange_weak(maximum, us, std::memory_order_relaxed)) {
        }
    }

    // Allocated only after the feature is enabled and a real context is adopted.
    // The normal renderer path does not pay the roughly 8.5 MiB queue cost.
    std::unique_ptr<command_packet[]> queue_;
    std::atomic<uint64_t> head_{0};
    std::atomic<uint64_t> tail_{0};
    std::atomic<uint64_t> wake_counter_{0};
    uint64_t producer_head_ = 0;
    size_t pending_count_ = 0;
    size_t pending_payload_bytes_ = 0;
    size_t reserved_payload_end_ = 0;
    uint8_t pending_segment_ = 0;
    command_kind reserved_kind_ = command_kind::barrier;
    coalesce_key reserved_key_{};
    bool reservation_open_ = false;
    std::atomic<bool> active_{false};
    std::thread::id owner_;

    std::mutex control_mutex_;
    std::condition_variable control_cv_;
    std::thread worker_;
    context_binding binding_;
    control_request request_ = control_request::none;
    bool control_done_ = false;
    bool control_result_ = false;
    bool disabled_ = false;
    uint64_t last_swap_sequence_ = 0;

    uint64_t submitted_commands_ = 0;
    std::atomic<uint64_t> executed_{0};
    uint64_t packets_submitted_ = 0;
    std::atomic<uint64_t> synchronous_waits_{0};
    std::atomic<uint64_t> synchronous_wait_us_{0};
    std::atomic<uint64_t> synchronous_wait_max_us_{0};
    std::atomic<uint64_t> producer_waits_{0};
    std::atomic<uint64_t> producer_wait_us_{0};
    std::atomic<uint64_t> producer_wait_max_us_{0};
    std::atomic<uint64_t> frame_wait_us_{0};
    std::atomic<uint64_t> frame_wait_max_us_{0};
    uint64_t queue_highwater_ = 0;
    uint64_t packet_highwater_ = 0;
    std::atomic<uint64_t> swaps_submitted_{0};
    std::atomic<uint64_t> swaps_completed_{0};
    std::atomic<uint64_t> swap_failures_{0};
    std::atomic<uint64_t> renderer_segments_{0};
    std::atomic<uint64_t> renderer_state_{0};
    std::atomic<uint64_t> renderer_uniform_{0};
    std::atomic<uint64_t> renderer_resource_{0};
    std::atomic<uint64_t> renderer_draw_{0};
    std::atomic<uint64_t> renderer_barrier_{0};
    uint64_t state_commands_dropped_ = 0;
    uint64_t uniform_commands_dropped_ = 0;
};

submission_state& state() {
    // Explicit EGL shutdown owns the worker's lifetime. Keeping the state itself
    // allocated avoids cross-translation-unit destructor ordering with dlclose.
    static submission_state* value = new submission_state();
    return *value;
}

struct swap_command {
    EGLDisplay display;
    EGLSurface surface;
    egl_swap_buffers_fn full_swap;
    egl_swap_damage_fn damage_swap;
    EGLint* rectangles;
    EGLint rectangle_count;
    EGLBoolean* synchronous_result;

    static void execute(void* storage) {
        auto* command = static_cast<swap_command*>(storage);
        const EGLBoolean result = command->damage_swap != nullptr
                                      ? command->damage_swap(command->display, command->surface, command->rectangles,
                                                             command->rectangle_count)
                                      : command->full_swap(command->display, command->surface);
        if (command->synchronous_result != nullptr) *command->synchronous_result = result;
        state().recordSwap(result == EGL_TRUE);
    }
    static void destroy(void* storage) {
        auto* command = static_cast<swap_command*>(storage);
        std::free(command->rectangles);
        command->~swap_command();
    }
};

} // namespace

bool active() { return state().ownsForCaller(); }
reservation reserve(command_fn execute, command_fn destroy, size_t payload_size, size_t payload_alignment,
                    command_kind kind, coalesce_key key) {
    return state().reserveCommand(execute, destroy, payload_size, payload_alignment, kind, key);
}
void publish(uint64_t sequence) { state().publishCommand(sequence); }
void wait(uint64_t sequence) { state().waitFor(sequence); }
void flush_pending() { state().flushPending(); }

bool adopt_context(EGLDisplay display, EGLSurface draw, EGLSurface read, EGLContext context,
                   egl_bind_api_fn bind_api, egl_make_current_fn make_current, egl_release_thread_fn release_thread) {
    return state().adopt({display, draw, read, context, bind_api, make_current, release_thread});
}

bool release_context() { return state().release(); }
void shutdown() { state().stopWorker(); }
bool owns_context_for_caller() { return state().ownsForCaller(); }

bool submit_swap(EGLDisplay display, EGLSurface surface, egl_swap_buffers_fn full_swap,
                 egl_swap_damage_fn damage_swap, const EGLint* rects, EGLint rect_count,
                 bool synchronous, EGLBoolean* result) {
    if (!state().ownsForCaller() || full_swap == nullptr || rect_count < 0 ||
        (damage_swap != nullptr && rect_count > 0 && rects == nullptr)) {
        return false;
    }

    const uint64_t previous = state().previousSwapSequence();
    if (previous != 0) {
        const bool record_telemetry = mg_pz_census_active;
        const auto started = record_telemetry ? std::chrono::steady_clock::now()
                                              : std::chrono::steady_clock::time_point{};
        state().waitFor(previous, false);
        if (record_telemetry) state().recordFrameThrottle(started);
    }

    EGLint* copied_rects = nullptr;
    if (damage_swap != nullptr && rect_count > 0) {
        const size_t rectangles = static_cast<size_t>(rect_count);
        if (rectangles > std::numeric_limits<size_t>::max() / (4 * sizeof(EGLint))) return false;
        const size_t count = rectangles * 4;
        copied_rects = static_cast<EGLint*>(std::malloc(count * sizeof(EGLint)));
        if (copied_rects == nullptr) return false;
        std::memcpy(copied_rects, rects, count * sizeof(EGLint));
    }

    EGLBoolean synchronous_value = EGL_FALSE;
    const reservation slot = state().reserveCommand(&swap_command::execute, &swap_command::destroy,
                                                     sizeof(swap_command), alignof(swap_command),
                                                     command_kind::barrier, {});
    if (slot.storage == nullptr) {
        std::free(copied_rects);
        return false;
    }
    new (slot.storage) swap_command{display, surface, full_swap, damage_swap, copied_rects, rect_count,
                                    synchronous ? &synchronous_value : nullptr};
    state().publishCommand(slot.sequence);
    state().setLastSwapSequence(slot.sequence);
    const uint64_t frame = state().nextSwapNumber();
    if (synchronous) state().waitFor(slot.sequence);
    if (result != nullptr) *result = synchronous ? synchronous_value : EGL_TRUE;
    state().report(frame);
    return true;
}

} // namespace mg_ts

#endif // ZOMDROID_EXPERIMENTAL
