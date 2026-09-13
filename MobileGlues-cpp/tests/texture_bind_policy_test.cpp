#include <cstdio>

bool mg_texture_bind_elision_allowed(bool shadow_redundant);

int main() {
    int failures = 0;
    if (mg_texture_bind_elision_allowed(false)) {
        std::printf("FAIL non-redundant texture state must reach GLES\n");
        ++failures;
    }
    if (mg_texture_bind_elision_allowed(true)) {
        std::printf("FAIL ZomDroid must not elide texture state from an unverified shadow\n");
        ++failures;
    }
    std::printf("%s (%d failures)\n", failures ? "FAILED" : "texture bind policy checks passed", failures);
    return failures != 0;
}
