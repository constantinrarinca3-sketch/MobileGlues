// Host-side ordering and lifetime checks for experimental GL command submission.
#include "gl/pz_census.h"
#include "gl/threaded_submission.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

bool mg_pz_threaded_submission_active = true;
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
GLfloat uniform[4] = {};
std::atomic<uint64_t> counted{0};
std::atomic<uint64_t> state_calls{0};
std::atomic<int> state_value{-1};
std::atomic<uint64_t> uniform_calls{0};
std::atomic<int> uniform_value{-1};
std::atomic<uint64_t> uniform_vector_calls{0};
std::atomic<int> uniform_vector_value{-1};

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

void fakeEnable(GLenum) {
    state_value.store(1, std::memory_order_relaxed);
    state_calls.fetch_add(1, std::memory_order_relaxed);
}

void fakeDisable(GLenum) {
    state_value.store(0, std::memory_order_relaxed);
    state_calls.fetch_add(1, std::memory_order_relaxed);
}

void fakeUniform1i(GLint, GLint value) {
    uniform_value.store(value, std::memory_order_relaxed);
    uniform_calls.fetch_add(1, std::memory_order_relaxed);
}

void fakeUniform2iv(GLint, GLsizei count, const GLint* value) {
    if (count == 1 && value != nullptr) uniform_vector_value.store(value[1], std::memory_order_relaxed);
    uniform_vector_calls.fetch_add(1, std::memory_order_relaxed);
}

void fakeUseProgram(GLuint) {}

void fakeBufferData(GLenum, GLsizeiptr size, const void* data, GLenum) {
    std::lock_guard<std::mutex> lock(mutex);
    order.push_back(2);
    if (size == 4 && data != nullptr) std::memcpy(uploaded, data, 4);
}

void fakeUniform4fv(GLint, GLsizei count, const GLfloat* value) {
    std::lock_guard<std::mutex> lock(mutex);
    order.push_back(3);
    if (count == 1 && value != nullptr) std::memcpy(uniform, value, sizeof(uniform));
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

EGLBoolean fakeSwap(EGLDisplay, EGLSurface) {
    std::lock_guard<std::mutex> lock(mutex);
    order.push_back(5);
    return EGL_TRUE;
}

} // namespace

int main() {
    expect(mg_ts::classifyCommand("glUseProgram") == mg_ts::command_kind::program_state &&
               mg_ts::endsRendererSegment(mg_ts::command_kind::program_state),
           "program changes must terminate uniform compiler segments");
    expect(mg_ts::classifyCommand("glUniform4fv") == mg_ts::command_kind::uniform,
           "uniform writes stay inside a renderer segment");
    expect(mg_ts::classifyCommand("glBufferSubData") == mg_ts::command_kind::resource_write,
           "resource writes terminate renderer segments");
    expect(mg_ts::classifyCommand("glDrawElements") == mg_ts::command_kind::draw,
           "draws terminate renderer segments");
    expect(mg_ts::classifyCommand("glReadPixels") == mg_ts::command_kind::barrier,
           "unknown and synchronous operations are barriers");
    expect(!mg_ts::endsRendererSegment(mg_ts::command_kind::state) &&
               mg_ts::endsRendererSegment(mg_ts::command_kind::draw),
           "only consumers and barriers close a renderer segment");
    expect(mg_ts::coalesceDomainForName("glEnable") == mg_ts::coalesce_domain::enable &&
               mg_ts::coalesceDomainForName("glDisable") == mg_ts::coalesce_domain::enable,
           "opposite writes to one enable state must share a compiler domain");
    expect(mg_ts::coalesceDomainForName("glBindTexture") == mg_ts::coalesce_domain::none,
           "binding commands must not be coalesced without dependency tracking");

    const std::thread::id producer = std::this_thread::get_id();
    const EGLDisplay display = reinterpret_cast<EGLDisplay>(1);
    const EGLSurface surface = reinterpret_cast<EGLSurface>(2);
    const EGLContext context = reinterpret_cast<EGLContext>(3);

    expect(mg_ts::adopt_context(display, surface, surface, context, fakeBindAPI, fakeMakeCurrent,
                                fakeReleaseThread),
           "worker must adopt the context");
    expect(mg_ts::active(), "submission must be active for the producer");
    expect(worker_id != producer, "backend context must live on a different thread");

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
    mg_ts_dispatch_slot<void (*)(GLenum)> enable{"glEnable"};
    mg_ts_dispatch_slot<void (*)(GLenum)> disable{"glDisable"};
    mg_ts_dispatch_slot<void (*)(GLint, GLint)> uniform1i{"glUniform1i"};
    mg_ts_dispatch_slot<void (*)(GLint, GLsizei, const GLint*)> uniform2iv{"glUniform2iv"};
    mg_ts_dispatch_slot<void (*)(GLuint)> use_program{"glUseProgram"};
    mg_ts_dispatch_slot<void (*)(GLenum, GLsizeiptr, const void*, GLenum)> buffer_data{"glBufferData"};
    mg_ts_dispatch_slot<void (*)(GLint, GLsizei, const GLfloat*)> uniform4fv{"glUniform4fv"};
    mg_ts_dispatch_slot<GLint (*)()> query{"glGetError"};
    mg_ts_dispatch_slot<GLint (*)()> quiet_query{"glGetError"};
    mg_ts_dispatch_slot<void (*)()> flush{"glFlush"};
    block = fakeBlock;
    no_op = fakeNoop;
    count = fakeCount;
    enable = fakeEnable;
    disable = fakeDisable;
    uniform1i = fakeUniform1i;
    uniform2iv = fakeUniform2iv;
    use_program = fakeUseProgram;
    buffer_data = fakeBufferData;
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
    buffer_data(GL_ARRAY_BUFFER, 4, source, GL_STREAM_DRAW);
    uniform4fv(9, 1, source_uniform);
    std::memset(source, 9, sizeof(source));
    for (float& value : source_uniform) value = 9.0f;

    {
        std::lock_guard<std::mutex> lock(mutex);
        blocker_released = true;
    }
    release_cv.notify_one();

    expect(query() == 77, "a value-returning command must wait for preceding work");
    expect(uploaded[0] == 1 && uploaded[1] == 2 && uploaded[2] == 3 && uploaded[3] == 4,
           "buffer bytes must be copied before returning to the caller");
    expect(uniform[0] == 1.0f && uniform[1] == 2.0f && uniform[2] == 3.0f && uniform[3] == 4.0f,
           "uniform bytes must be copied before returning to the caller");

    // Cross the packet-ring boundary and make a partial final packet visible
    // through the following synchronous query.
    constexpr uint64_t stress_commands = 40000;
    for (uint64_t i = 0; i < stress_commands; ++i) count(static_cast<GLint>(i));
    expect(quiet_query() == 88, "a synchronous query must flush a partial packet");
    expect(counted.load(std::memory_order_relaxed) == stress_commands,
           "packet queue must preserve every command across ring reuse");

    // Within one safe segment only the final write to the same state reaches
    // the backend. A synchronous query closes the segment and makes it visible.
    enable(GL_BLEND);
    disable(GL_BLEND);
    expect(quiet_query() == 88, "a query must flush the compiled state segment");
    expect(state_calls.load(std::memory_order_relaxed) == 1 && state_value.load(std::memory_order_relaxed) == 0,
           "the compiler must execute only the final state write in a segment");

    enable(GL_BLEND);
    expect(quiet_query() == 88, "the first compiler boundary must complete");
    disable(GL_BLEND);
    expect(quiet_query() == 88, "the second compiler boundary must complete");
    expect(state_calls.load(std::memory_order_relaxed) == 3,
           "state writes separated by barriers must both reach the backend");

    enable(GL_BLEND);
    disable(GL_DEPTH_TEST);
    expect(quiet_query() == 88, "different state selectors must complete");
    expect(state_calls.load(std::memory_order_relaxed) == 5,
           "different enable capabilities must not overwrite one another");

    uniform1i(7, 10);
    uniform1i(7, 20);
    expect(quiet_query() == 88, "a query must flush the compiled uniform segment");
    expect(uniform_calls.load(std::memory_order_relaxed) == 1 &&
               uniform_value.load(std::memory_order_relaxed) == 20,
           "the compiler must execute only the final uniform write in a segment");

    GLint first_vector[2] = {1, 2};
    GLint final_vector[2] = {3, 4};
    uniform2iv(9, 1, first_vector);
    uniform2iv(9, 1, final_vector);
    expect(quiet_query() == 88, "a query must flush copied uniform vectors");
    expect(uniform_vector_calls.load(std::memory_order_relaxed) == 1 &&
               uniform_vector_value.load(std::memory_order_relaxed) == 4,
           "copied uniform vectors must also keep only the final write");

    uniform1i(7, 30);
    use_program(4);
    uniform1i(7, 40);
    expect(quiet_query() == 88, "uniforms around a program change must complete");
    expect(uniform_calls.load(std::memory_order_relaxed) == 3 &&
               uniform_value.load(std::memory_order_relaxed) == 40,
           "uniform writes must never coalesce across a program change");

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
