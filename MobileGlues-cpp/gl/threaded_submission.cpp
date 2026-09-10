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
#include <memory>
#include <mutex>
#include <thread>

namespace mg_ts {
namespace {

constexpr uint64_t kQueueCapacity = 32768;
constexpr uint64_t kQueueMask = kQueueCapacity - 1;
static_assert((kQueueCapacity & kQueueMask) == 0);

struct alignas(std::max_align_t) queue_slot {
    command_fn execute = nullptr;
    command_fn destroy = nullptr;
    alignas(std::max_align_t) unsigned char payload[kCommandPayloadBytes];
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

    reservation reserveSlot(command_fn execute, command_fn destroy) {
        uint64_t head = producer_head_;
        const auto wait_started = std::chrono::steady_clock::now();
        bool blocked = false;
        while (head - tail_.load(std::memory_order_acquire) >= kQueueCapacity) {
            blocked = true;
            const uint64_t wanted = head - kQueueCapacity + 1;
            waiting_for_.store(wanted, std::memory_order_release);
            std::unique_lock<std::mutex> lock(wait_mutex_);
            completion_cv_.wait(lock, [this, wanted] {
                return tail_.load(std::memory_order_acquire) >= wanted || !isActive();
            });
            if (!isActive()) return {nullptr, 0};
        }
        if (blocked) {
            waiting_for_.store(std::numeric_limits<uint64_t>::max(), std::memory_order_release);
            recordProducerWait(wait_started);
        }

        const uint64_t depth = head - tail_.load(std::memory_order_relaxed) + 1;
        uint64_t high = queue_highwater_.load(std::memory_order_relaxed);
        while (depth > high && !queue_highwater_.compare_exchange_weak(high, depth, std::memory_order_relaxed)) {
        }

        queue_slot& slot = queue_[head & kQueueMask];
        slot.execute = execute;
        slot.destroy = destroy;
        return {slot.payload, head + 1};
    }

    void publishSlot(uint64_t sequence) {
        producer_head_ = sequence;
        submitted_.fetch_add(1, std::memory_order_relaxed);
        head_.store(sequence, std::memory_order_release);
        work_cv_.notify_one();
    }

    void waitFor(uint64_t sequence, bool record_wait = true) {
        if (sequence == 0 || tail_.load(std::memory_order_acquire) >= sequence) return;
        if (record_wait) synchronous_waits_.fetch_add(1, std::memory_order_relaxed);
        const auto started = std::chrono::steady_clock::now();
        waiting_for_.store(sequence, std::memory_order_release);
        std::unique_lock<std::mutex> lock(wait_mutex_);
        completion_cv_.wait(lock, [this, sequence] { return tail_.load(std::memory_order_acquire) >= sequence; });
        waiting_for_.store(std::numeric_limits<uint64_t>::max(), std::memory_order_release);
        if (record_wait) recordSynchronousWait(started);
    }

    bool adopt(const context_binding& binding) {
        if (!mg_pz_threaded_submission_active || binding.context == EGL_NO_CONTEXT || binding.bind_api == nullptr ||
            binding.make_current == nullptr) {
            return false;
        }
        if (isActive() && !release()) return false;
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
        work_cv_.notify_one();
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
        LOG_I("ZOMDROID_PZ_THREADED_SUBMISSION active=1 queue_slots=%llu frame_depth=1",
              static_cast<unsigned long long>(kQueueCapacity))
        return true;
    }

    bool release() {
        if (!isActive()) return true;
        const uint64_t target = head_.load(std::memory_order_acquire);
        waitFor(target, false);
        active_.store(false, std::memory_order_release);

        std::unique_lock<std::mutex> lock(control_mutex_);
        control_done_ = false;
        request_ = control_request::release;
        lock.unlock();
        work_cv_.notify_one();
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
        work_cv_.notify_one();
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
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
        frame_wait_us_.fetch_add(static_cast<uint64_t>(ms * 1000.0), std::memory_order_relaxed);
        uint64_t maximum = frame_wait_max_us_.load(std::memory_order_relaxed);
        const uint64_t us = static_cast<uint64_t>(ms * 1000.0);
        while (us > maximum &&
               !frame_wait_max_us_.compare_exchange_weak(maximum, us, std::memory_order_relaxed)) {
        }
    }

    void recordSwap(bool succeeded) {
        swaps_completed_.fetch_add(1, std::memory_order_relaxed);
        if (!succeeded) swap_failures_.fetch_add(1, std::memory_order_relaxed);
    }

    uint64_t nextSwapNumber() { return swaps_submitted_.fetch_add(1, std::memory_order_relaxed) + 1; }

    void report(uint64_t frames) {
        if (frames != 60 && frames % 300 != 0) return;
        const uint64_t sync_waits = synchronous_waits_.load(std::memory_order_relaxed);
        const uint64_t frame_wait_us = frame_wait_us_.load(std::memory_order_relaxed);
        LOG_I("ZOMDROID_PZ_THREADED_SUBMISSION frames=%llu submitted=%llu executed=%llu sync_waits=%llu "
              "sync_avg_ms=%.3f sync_max_ms=%.3f queue_waits=%llu queue_wait_avg_ms=%.3f "
              "queue_wait_max_ms=%.3f frame_wait_avg_ms=%.3f frame_wait_max_ms=%.3f "
              "queue_highwater=%llu swap_done=%llu swap_fail=%llu",
              static_cast<unsigned long long>(frames),
              static_cast<unsigned long long>(submitted_.load(std::memory_order_relaxed)),
              static_cast<unsigned long long>(executed_.load(std::memory_order_relaxed)),
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
              static_cast<unsigned long long>(queue_highwater_.load(std::memory_order_relaxed)),
              static_cast<unsigned long long>(swaps_completed_.load(std::memory_order_relaxed)),
              static_cast<unsigned long long>(swap_failures_.load(std::memory_order_relaxed)))
    }

  private:
    bool ensureWorker() {
        std::lock_guard<std::mutex> lock(control_mutex_);
        if (disabled_) return false;
        if (worker_.joinable()) return true;
        try {
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
            const uint64_t available = head_.load(std::memory_order_acquire);
            if (consumer_tail < available) {
                queue_slot& slot = queue_[consumer_tail & kQueueMask];
                slot.execute(slot.payload);
                slot.destroy(slot.payload);
                ++consumer_tail;
                executed_.fetch_add(1, std::memory_order_relaxed);
                tail_.store(consumer_tail, std::memory_order_release);
                if (consumer_tail >= waiting_for_.load(std::memory_order_acquire)) completion_cv_.notify_all();
                continue;
            }

            control_request request = control_request::none;
            context_binding binding;
            {
                std::unique_lock<std::mutex> lock(control_mutex_);
                work_cv_.wait(lock, [this, consumer_tail] {
                    return head_.load(std::memory_order_acquire) > consumer_tail ||
                           request_ != control_request::none;
                });
                if (head_.load(std::memory_order_acquire) > consumer_tail) continue;
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

    std::unique_ptr<queue_slot[]> queue_{new queue_slot[kQueueCapacity]};
    std::atomic<uint64_t> head_{0};
    std::atomic<uint64_t> tail_{0};
    uint64_t producer_head_ = 0;
    std::atomic<uint64_t> waiting_for_{std::numeric_limits<uint64_t>::max()};
    std::atomic<bool> active_{false};
    std::thread::id owner_;

    std::mutex wait_mutex_;
    std::condition_variable work_cv_;
    std::condition_variable completion_cv_;
    std::mutex control_mutex_;
    std::condition_variable control_cv_;
    std::thread worker_;
    context_binding binding_;
    control_request request_ = control_request::none;
    bool control_done_ = false;
    bool control_result_ = false;
    bool disabled_ = false;
    uint64_t last_swap_sequence_ = 0;

    std::atomic<uint64_t> submitted_{0};
    std::atomic<uint64_t> executed_{0};
    std::atomic<uint64_t> synchronous_waits_{0};
    std::atomic<uint64_t> synchronous_wait_us_{0};
    std::atomic<uint64_t> synchronous_wait_max_us_{0};
    std::atomic<uint64_t> producer_waits_{0};
    std::atomic<uint64_t> producer_wait_us_{0};
    std::atomic<uint64_t> producer_wait_max_us_{0};
    std::atomic<uint64_t> frame_wait_us_{0};
    std::atomic<uint64_t> frame_wait_max_us_{0};
    std::atomic<uint64_t> queue_highwater_{0};
    std::atomic<uint64_t> swaps_submitted_{0};
    std::atomic<uint64_t> swaps_completed_{0};
    std::atomic<uint64_t> swap_failures_{0};
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
reservation reserve(command_fn execute, command_fn destroy) { return state().reserveSlot(execute, destroy); }
void publish(uint64_t sequence) { state().publishSlot(sequence); }
void wait(uint64_t sequence) { state().waitFor(sequence); }

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
        const auto started = std::chrono::steady_clock::now();
        state().waitFor(previous, false);
        state().recordFrameThrottle(started);
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
    const reservation slot = state().reserveSlot(&swap_command::execute, &swap_command::destroy);
    if (slot.storage == nullptr) {
        std::free(copied_rects);
        return false;
    }
    new (slot.storage) swap_command{display, surface, full_swap, damage_swap, copied_rects, rect_count,
                                    synchronous ? &synchronous_value : nullptr};
    state().publishSlot(slot.sequence);
    state().setLastSwapSequence(slot.sequence);
    const uint64_t frame = state().nextSwapNumber();
    if (synchronous) state().waitFor(slot.sequence);
    if (result != nullptr) *result = synchronous ? synchronous_value : EGL_TRUE;
    state().report(frame);
    return true;
}

} // namespace mg_ts

#endif // ZOMDROID_EXPERIMENTAL
