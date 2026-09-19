// Host-side ordering and lifetime checks for experimental GL command submission.
#include "gl/pz_census.h"
#include "gl/threaded_submission.h"

#include <atomic>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

bool mg_pz_threaded_submission_active = true;
bool mg_pz_zbetterfps_fastpath_active = true;
bool mg_pz_large_uniform_async_active = true;
bool mg_pz_census_active = false;

extern "C" void write_log(const char*, ...) {}
extern "C" void write_log_n(const char*, ...) {}
int __android_log_print(int, const char*, const char*, ...) { return 0; }

namespace {

int failures = 0;
std::mutex mutex;
std::condition_variable entered_cv;
std::condition_variable release_cv;
bool blocker_entered = false;
bool blocker_released = false;
bool flush_executed = false;
std::thread::id worker_id;
std::vector<int> order;
unsigned char uploaded[4] = {};
std::array<unsigned char, 576> fusion_upload{};
GLfloat uniform[4] = {};
std::array<GLfloat, 60 * 16> large_uniform{};
GLsizei large_uniform_count = 0;
GLboolean large_uniform_transpose = GL_FALSE;
std::atomic<uint64_t> counted{0};
bool extended_payload_executed = false;

void expect(bool condition, const char* message) {
    if (condition) return;
    std::printf("FAIL %s\n", message);
    ++failures;
}

EGLBoolean fakeBindAPI(EGLenum api) { return api == EGL_OPENGL_ES_API ? EGL_TRUE : EGL_FALSE; }

EGLBoolean fakeMakeCurrent(EGLDisplay, EGLSurface, EGLSurface, EGLContext context) {
    if (context != EGL_NO_CONTEXT) worker_id = std::this_thread::get_id();
    return EGL_TRUE;
}

EGLBoolean fakeReleaseThread() { return EGL_TRUE; }

void fakeBlock(GLint value) {
    std::unique_lock<std::mutex> lock(mutex);
    order.push_back(value);
    blocker_entered = true;
    entered_cv.notify_one();
    release_cv.wait(lock, [] { return blocker_released; });
}

void fakeNoop(GLint) {}

void fakeCount(GLint) { counted.fetch_add(1, std::memory_order_relaxed); }

void fakeBufferData(GLenum, GLsizeiptr size, const void* data, GLenum) {
    std::lock_guard<std::mutex> lock(mutex);
    order.push_back(2);
    if (size == 4 && data != nullptr) std::memcpy(uploaded, data, 4);
}

void fakeBufferSubData(GLenum target, GLintptr offset, GLsizeiptr size, const void* data) {
    if (target == GL_ARRAY_BUFFER && offset == 0 && size == static_cast<GLsizeiptr>(fusion_upload.size()) &&
        data != nullptr)
        std::memcpy(fusion_upload.data(), data, fusion_upload.size());
}

void fakeUniform4fv(GLint, GLsizei count, const GLfloat* value) {
    std::lock_guard<std::mutex> lock(mutex);
    order.push_back(3);
    if (count == 1 && value != nullptr) std::memcpy(uniform, value, sizeof(uniform));
}

void fakeUniformMatrix4fv(GLint, GLsizei count, GLboolean transpose, const GLfloat* value) {
    large_uniform_count = count;
    large_uniform_transpose = transpose;
    if (count == 60 && value != nullptr)
        std::memcpy(large_uniform.data(), value, large_uniform.size() * sizeof(GLfloat));
}

GLint fakeQuery() {
    std::lock_guard<std::mutex> lock(mutex);
    order.push_back(4);
    return 77;
}

GLint fakeQuietQuery() { return 88; }

void fakeFlush() {
    std::lock_guard<std::mutex> lock(mutex);
    flush_executed = true;
    entered_cv.notify_one();
}

void fakeExtendedPayload(void* storage) {
    const auto* bytes = static_cast<const unsigned char*>(storage);
    extended_payload_executed = bytes[0] == 0x5a && bytes[575] == 0xa5;
}

void destroyExtendedPayload(void*) {}

EGLBoolean fakeSwap(EGLDisplay, EGLSurface) {
    std::lock_guard<std::mutex> lock(mutex);
    order.push_back(5);
    return EGL_TRUE;
}

} // namespace

int main() {
    const std::thread::id producer = std::this_thread::get_id();
    const EGLDisplay display = reinterpret_cast<EGLDisplay>(1);
    const EGLSurface surface = reinterpret_cast<EGLSurface>(2);
    const EGLContext context = reinterpret_cast<EGLContext>(3);

    expect(mg_ts::adopt_context(display, surface, surface, context, fakeBindAPI, fakeMakeCurrent,
                                fakeReleaseThread),
           "worker must adopt the context");
    expect(mg_ts::active(), "submission must be active for the producer");
    expect(worker_id != producer, "backend context must live on a different thread");

    // ZBBetterFPS chunk fusion uploads 8 * 18 floats (576 bytes). The packet
    // queue must own that payload directly instead of forcing a heap copy.
    const mg_ts::reservation extended =
        mg_ts::reserve(fakeExtendedPayload, destroyExtendedPayload, 576, alignof(std::max_align_t));
    expect(extended.storage != nullptr, "a 576-byte fusion upload must fit directly in a command packet");
    if (extended.storage != nullptr) {
        std::memset(extended.storage, 0, 576);
        static_cast<unsigned char*>(extended.storage)[0] = 0x5a;
        static_cast<unsigned char*>(extended.storage)[575] = 0xa5;
        mg_ts::publish(extended.sequence);
        mg_ts::wait(extended.sequence);
    }
    expect(extended_payload_executed, "the worker must execute the packet-owned fusion payload");

    bool second_adopted = true;
    std::thread second_context([&] {
        second_adopted = mg_ts::adopt_context(display, surface, surface, reinterpret_cast<EGLContext>(4),
                                              fakeBindAPI, fakeMakeCurrent, fakeReleaseThread);
    });
    second_context.join();
    expect(!second_adopted, "a second context thread must stay on direct submission");
    expect(mg_ts::active(), "a second context thread must not steal the producer queue");

    mg_ts_dispatch_slot<void (*)(GLint)> block{"glClear"};
    mg_ts_dispatch_slot<void (*)(GLint)> no_op{"glClear"};
    mg_ts_dispatch_slot<void (*)(GLint)> count{"glClear"};
    mg_ts_dispatch_slot<void (*)(GLenum, GLsizeiptr, const void*, GLenum)> buffer_data{"glBufferData"};
    mg_ts_dispatch_slot<void (*)(GLenum, GLintptr, GLsizeiptr, const void*)> buffer_sub_data{"glBufferSubData"};
    mg_ts_dispatch_slot<void (*)(GLint, GLsizei, const GLfloat*)> uniform4fv{"glUniform4fv"};
    mg_ts_dispatch_slot<GLint (*)()> query{"glGetError"};
    mg_ts_dispatch_slot<GLint (*)()> quiet_query{"glGetError"};
    mg_ts_dispatch_slot<void (*)()> flush{"glFlush"};
    block = fakeBlock;
    no_op = fakeNoop;
    count = fakeCount;
    buffer_data = fakeBufferData;
    buffer_sub_data = fakeBufferSubData;
    uniform4fv = fakeUniform4fv;
    query = fakeQuery;
    quiet_query = fakeQuietQuery;
    flush = fakeFlush;

    block(1);
    // Filling one packet must publish all 32 calls with a single queue wake.
    for (int i = 1; i < 32; ++i) no_op(i);
    {
        std::unique_lock<std::mutex> lock(mutex);
        entered_cv.wait(lock, [] { return blocker_entered; });
    }

    unsigned char source[4] = {1, 2, 3, 4};
    GLfloat source_uniform[4] = {1.0f, 2.0f, 3.0f, 4.0f};
    std::array<unsigned char, 576> fusion_source{};
    std::array<GLfloat, 60 * 16> large_uniform_source{};
    fusion_source.front() = 0x31;
    fusion_source.back() = 0x79;
    for (size_t i = 0; i < large_uniform_source.size(); ++i)
        large_uniform_source[i] = static_cast<GLfloat>(i) + 0.25f;
    buffer_data(GL_ARRAY_BUFFER, 4, source, GL_STREAM_DRAW);
    buffer_sub_data(GL_ARRAY_BUFFER, 0, fusion_source.size(), fusion_source.data());
    uniform4fv(9, 1, source_uniform);
    expect(mg_ts::tryUniformCopy(fakeUniformMatrix4fv, 16, 11, 60, static_cast<GLboolean>(GL_TRUE),
                                 static_cast<const GLfloat*>(large_uniform_source.data())),
           "a 3.75 KiB matrix palette must be copied into the command packet asynchronously");
    std::memset(source, 9, sizeof(source));
    fusion_source.fill(0xff);
    for (float& value : source_uniform) value = 9.0f;
    large_uniform_source.fill(9.0f);

    {
        std::lock_guard<std::mutex> lock(mutex);
        blocker_released = true;
    }
    release_cv.notify_one();

    expect(query() == 77, "a value-returning command must wait for preceding work");
    expect(uploaded[0] == 1 && uploaded[1] == 2 && uploaded[2] == 3 && uploaded[3] == 4,
           "buffer bytes must be copied before returning to the caller");
    expect(fusion_upload.front() == 0x31 && fusion_upload.back() == 0x79,
           "a 576-byte fusion upload must remain packet-owned until backend execution");
    expect(uniform[0] == 1.0f && uniform[1] == 2.0f && uniform[2] == 3.0f && uniform[3] == 4.0f,
           "uniform bytes must be copied before returning to the caller");
    expect(large_uniform_count == 60 && large_uniform_transpose == GL_TRUE && large_uniform.front() == 0.25f &&
               large_uniform.back() == 959.25f,
           "large uniform bytes must remain packet-owned until backend execution");

    // Cross the packet-ring boundary and make a partial final packet visible
    // through the following synchronous query.
    constexpr uint64_t stress_commands = 40000;
    for (uint64_t i = 0; i < stress_commands; ++i) count(static_cast<GLint>(i));
    expect(quiet_query() == 88, "a synchronous query must flush a partial packet");
    expect(counted.load(std::memory_order_relaxed) == stress_commands,
           "packet queue must preserve every command across ring reuse");

    flush();
    {
        std::unique_lock<std::mutex> lock(mutex);
        expect(entered_cv.wait_for(lock, std::chrono::seconds(2), [] { return flush_executed; }),
               "glFlush must publish a partial packet without waiting for a full packet");
    }

    EGLBoolean swap_result = EGL_FALSE;
    expect(mg_ts::submit_swap(display, surface, fakeSwap, nullptr, nullptr, 0, true, &swap_result),
           "swap must enter the worker queue");
    expect(swap_result == EGL_TRUE, "a synchronous swap must return the backend result");
    expect(mg_ts::release_context(), "release must drain work and unbind the worker context");
    mg_ts::shutdown();

    expect(order == std::vector<int>({1, 2, 3, 4, 5}), "backend calls must preserve producer order");
    expect(!mg_ts::active(), "submission must be inactive after release");

    std::printf("%s (%d failures)\n", failures ? "FAILED" : "threaded submission checks passed", failures);
    return failures != 0;
}
