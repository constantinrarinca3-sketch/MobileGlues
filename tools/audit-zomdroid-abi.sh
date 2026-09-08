#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 1 ]]; then
    echo "usage: $0 /path/to/libMobileGluesZomDroid.so" >&2
    exit 2
fi

library=$1
tool_prefix=${ANDROID_NDK_HOME:-${ANDROID_NDK_ROOT:-}}/toolchains/llvm/prebuilt/linux-x86_64/bin
readelf_tool=${READELF:-$tool_prefix/llvm-readelf}
nm_tool=${NM:-$tool_prefix/llvm-nm}

[[ -f "$library" ]] || { echo "missing library: $library" >&2; exit 1; }
[[ -x "$readelf_tool" ]] || { echo "llvm-readelf not found: $readelf_tool" >&2; exit 1; }
[[ -x "$nm_tool" ]] || { echo "llvm-nm not found: $nm_tool" >&2; exit 1; }

header=$($readelf_tool -h "$library")
grep -q 'Class:.*ELF64' <<<"$header"
grep -q 'Machine:.*AArch64' <<<"$header"

while read -r alignment; do
    if (( alignment < 0x4000 )); then
        echo "LOAD segment is not Android 16 KB compatible: alignment=$alignment" >&2
        exit 1
    fi
done < <($readelf_tool -lW "$library" | awk '$1 == "LOAD" { print $NF }')

symbols=$($nm_tool -D --defined-only "$library")
required=(
    mg_zomdroid_build_id
    eglGetConfigAttrib eglGetConfigs eglGetDisplay eglGetError eglInitialize eglTerminate
    eglBindAPI eglCreateContext eglDestroySurface eglDestroyContext eglCreateWindowSurface
    eglCreatePbufferSurface eglMakeCurrent eglSwapBuffers eglSwapInterval eglQueryString
    eglGetProcAddress glXGetProcAddress glXGetProcAddressARB
    glGetString glGetIntegerv glGetError glClear glClearColor glViewport
    glGenTextures glBindTexture glTexImage2D glTexSubImage2D glDeleteTextures
    glGenBuffers glBindBuffer glBufferData glBufferSubData glDeleteBuffers
    glCreateShader glShaderSource glCompileShader glCreateProgram glAttachShader glLinkProgram
    glUseProgram glDrawArrays glDrawElements glGenFramebuffers glBindFramebuffer
    glFramebufferTexture2D glCheckFramebufferStatus
)

for symbol in "${required[@]}"; do
    grep -Eq "[[:space:]]${symbol}$" <<<"$symbols" || {
        echo "missing required ZomDroid symbol: $symbol" >&2
        exit 1
    }
done

if $readelf_tool -d "$library" | grep -Eq 'NEEDED.*(libGL\.so|libEGL\.so|libGLESv[0-9]*\.so)'; then
    echo "renderer must load EGL/GLES dynamically, not carry a hard graphics dependency" >&2
    exit 1
fi

echo "ZOMDROID_ABI_AUDIT_OK symbols=${#required[@]}"
