// Exercises the real sticky primitive-restart state machine without a GLES driver.
#include <cstdio>

int mg_test_sticky_restart_transition(unsigned long long context_id, bool desired);

static int failures = 0;

static void expect(bool condition, const char* message) {
    if (condition) return;
    std::printf("FAIL %s\n", message);
    ++failures;
}

int main() {
    expect(mg_test_sticky_restart_transition(1, false) == -1,
           "first draw establishes the disabled driver state");
    expect(mg_test_sticky_restart_transition(1, false) == 0, "repeated disabled state emits no call");
    expect(mg_test_sticky_restart_transition(1, true) == 1, "a required restart enables once");
    expect(mg_test_sticky_restart_transition(1, true) == 0, "consecutive restart draws stay enabled");
    expect(mg_test_sticky_restart_transition(1, false) == -1, "leaving a restart run disables once");
    expect(mg_test_sticky_restart_transition(2, false) == -1, "a new context establishes its own state");
    expect(mg_test_sticky_restart_transition(1, true) == 1, "returning to another context re-establishes state");

    std::printf("%s (%d failures)\n", failures ? "FAILED" : "sticky restart checks passed", failures);
    return failures != 0;
}
