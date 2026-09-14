cmake_minimum_required(VERSION 3.22.1)
project("mobileglues")
enable_language(CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

option(ZOMDROID_EXPERIMENTAL "Build the isolated ZomDroid ARM64 renderer" OFF)

set(ENABLE_RTTI                ON  CACHE BOOL "Enables RTTI (will be took by glslang)" FORCE)
set(ENABLE_HLSL                OFF CACHE BOOL "Disable HLSL input for glslang" FORCE)
set(BUILD_EXTERNAL             OFF CACHE BOOL "Disable build external dependencies for glslang" FORCE)
set(ENABLE_GLSLANG_BINARIES    OFF CACHE BOOL "Disable glslangValidator/spirv-remap" FORCE)
set(ENABLE_OPT                 OFF CACHE BOOL "Disable SPIRV-Tools opt usage in glslang" FORCE)
set(BUILD_EXTERNAL             OFF CACHE BOOL "Don't build external deps in External/" FORCE)

set(SPIRV_CROSS_ENABLE_C_API   ON  CACHE BOOL "Enable C API" FORCE)
set(SPIRV_CROSS_ENABLE_GLSL    ON  CACHE BOOL "Enable GLSL backend" FORCE)
set(SPIRV_CROSS_ENABLE_REFLECT OFF CACHE BOOL "Disable reflect" FORCE)
set(SPIRV_CROSS_ENABLE_HLSL    OFF CACHE BOOL "Disable HLSL backend" FORCE)
set(SPIRV_CROSS_ENABLE_MSL     OFF CACHE BOOL "Disable MSL backend" FORCE)
set(SPIRV_CROSS_ENABLE_CPP     OFF CACHE BOOL "Disable C++ API target" FORCE)
set(SPIRV_CROSS_CLI            OFF CACHE BOOL "Disable CLI binary" FORCE)
set(SPIRV_CROSS_STATIC         ON  CACHE BOOL "Prefer static libs" FORCE)
set(SPIRV_CROSS_ENABLE_UTIL    OFF CACHE BOOL "Disable util" FORCE)

add_subdirectory(3rdparty/glslang)
add_subdirectory(3rdparty/SPIRV-Cross)

if(MACOS)
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -w -fvisibility=default -funwind-tables -g -D_THREAD_SAFE -fPIC -stdlib=libc++")
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -w -g -std=gnu99 -funwind-tables -fvisibility=hidden")
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -static-libstdc++")
else()
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -w -fvisibility=hidden -funwind-tables -g -D_THREAD_SAFE -fPIC")
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -w -g -std=gnu99 -funwind-tables -fvisibility=hidden")
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -static-libstdc++")
    set(CMAKE_ANDROID_STL_TYPE c++_static)
endif()

if (NOT CMAKE_BUILD_TYPE STREQUAL "Debug")
    include(CheckIPOSupported)
    include(CheckCCompilerFlag)
    include(CheckCXXCompilerFlag)

    check_ipo_supported(RESULT LTOSupported OUTPUT LTOError)

    check_c_compiler_flag("-flto" HAS_LTO_C)
    check_cxx_compiler_flag("-flto" HAS_LTO_CXX)

    if (LTOSupported OR (HAS_LTO_C AND HAS_LTO_CXX))
    check_c_compiler_flag("-flto=thin" HAS_THINLTO_C)
    check_cxx_compiler_flag("-flto=thin" HAS_THINLTO_CXX)
    if (HAS_THINLTO_C AND HAS_THINLTO_CXX)
        message(STATUS "ThinLTO supported, using -flto=thin")

        add_compile_options(-flto=thin)
        add_link_options(-flto=thin)
    else()
        message(STATUS "ThinLTO not available, fallback to CMAKE IPO")
        set(CMAKE_INTERPROCEDURAL_OPTIMIZATION TRUE)
    endif()
    else()
    message(STATUS "IPO not supported: ${LTOError}")
    endif()
endif()

if (CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang" AND NOT MATCHES "AppleClang")
    add_compile_options(-O3 -ffunction-sections -fdata-sections)
    add_link_options(-Wl,--gc-sections)
elseif (CMAKE_CXX_COMPILER_ID MATCHES "MSVC")
else ()
    add_compile_options(-O2)
endif()

if (CMAKE_CXX_COMPILER_ID MATCHES "MSVC")
    add_compile_options(/Zc:preprocessor)
    add_compile_options(/Zc:__cplusplus)
endif()

set(PROFILING OFF)

add_library(${CMAKE_PROJECT_NAME} SHARED
    init.cpp
    main.cpp
    gl/gl_stub.cpp
    gl/gl_native.cpp
    gl/gl.cpp
    gl/envvars.cpp
    gl/log.cpp
    gl/program.cpp
    gl/pz_census.cpp
    gl/threaded_submission.cpp
    gl/shader.cpp
    gl/framebuffer.cpp
    gl/texture.cpp
    gl/drawing.cpp
    gl/multidraw.cpp
    bench/multidraw_bench.cpp
    gl/mg.cpp
    gl/buffer.cpp
    gl/getter.cpp
    gl/enable.cpp
    gl/server_attrib.cpp
    gl/restart.cpp
    egl/context.cpp
    gl/pixel.cpp
    gl/transfer.cpp
    gl/random_string_gen.cpp
    gl/ExtWrappers/DSAWrapper.cpp
    gl/ExtWrappers/MultiBindWrapper.cpp
    gl/glsl/glsl_for_es.cpp
    gl/glsl/cache.cpp
    gl/FSR1/FSR1.cpp

    gl/vertexattrib.cpp
    glx/lookup.cpp
    egl/egl.cpp
    egl/loader.cpp
    gles/loader.cpp
    config/cJSON.c
    config/config.cpp
    config/gpu_utils.cpp
    config/settings.cpp
    config/stats.cpp
)

if (ZOMDROID_EXPERIMENTAL)
    target_sources(${CMAKE_PROJECT_NAME} PRIVATE
        gl/pz_etc2.c
        gl/pz_etc2_runtime.cpp
        gl/pz_texture_memory.cpp
        gl/pz_static_sequence_census.cpp
        gl/pz_static_sequence_capture.cpp
        gl/pz_static_sequence_frontend.cpp
        gl/pz_static_sequence_egl.cpp
    )

    # V5 leaves the validated drawing/EGL source files byte-for-byte unchanged.
    # Only this diagnostic branch source-renames the public entry points so the
    # wrappers above can observe full draw arguments and frame boundaries before
    # forwarding to the exact original implementation.
    set_property(SOURCE gl/drawing.cpp APPEND PROPERTY COMPILE_DEFINITIONS
        glDrawArrays=mg_pz_original_glDrawArrays
        glDrawArraysInstanced=mg_pz_original_glDrawArraysInstanced
        glDrawElements=mg_pz_original_glDrawElements
        glDrawElementsInstanced=mg_pz_original_glDrawElementsInstanced
        glDrawElementsBaseVertex=mg_pz_original_glDrawElementsBaseVertex
        glDrawRangeElements=mg_pz_original_glDrawRangeElements
        glDrawRangeElementsBaseVertex=mg_pz_original_glDrawRangeElementsBaseVertex
        glDrawElementsInstancedBaseVertex=mg_pz_original_glDrawElementsInstancedBaseVertex
        glDrawArraysInstancedBaseInstance=mg_pz_original_glDrawArraysInstancedBaseInstance
        glDrawElementsInstancedBaseInstance=mg_pz_original_glDrawElementsInstancedBaseInstance
        glDrawElementsInstancedBaseVertexBaseInstance=mg_pz_original_glDrawElementsInstancedBaseVertexBaseInstance
    )
    set_property(SOURCE egl/egl.cpp APPEND PROPERTY COMPILE_DEFINITIONS
        eglMakeCurrent=mg_pz_original_eglMakeCurrent
        eglSwapBuffers=mg_pz_original_eglSwapBuffers
        eglSwapBuffersWithDamageKHR=mg_pz_original_eglSwapBuffersWithDamageKHR
        eglSwapBuffersWithDamageEXT=mg_pz_original_eglSwapBuffersWithDamageEXT
        eglGetProcAddress=mg_pz_original_eglGetProcAddress
    )

    # The encoder uses C11 atomics for its persistent worker pool. The project
    # retains gnu99 for its other C sources.
    set_source_files_properties(gl/pz_etc2.c PROPERTIES COMPILE_OPTIONS "-std=gnu11")
    target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE
        ZOMDROID_EXPERIMENTAL=1
        ZOMDROID_GL_BREADCRUMBS=1
    )
    set_target_properties(${CMAKE_PROJECT_NAME} PROPERTIES
        OUTPUT_NAME "MobileGluesZomDroid"
    )
endif()

target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE
    ./includes
    ./3rdparty/xxhash)
target_include_directories(${CMAKE_PROJECT_NAME} PUBLIC ${CMAKE_SOURCE_DIR}/include)

target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE
    glslang::glslang
    spirv-cross-c
)

if (NOT MSVC)
    target_link_options(${CMAKE_PROJECT_NAME} PRIVATE -Wl,-Bsymbolic-functions)
endif()

if(MACOS)
    target_link_libraries(${CMAKE_PROJECT_NAME} PUBLIC
        "-F${CMAKE_SOURCE_DIR}/external"
        "-framework libEGL"
        "-framework libGLESv2"
        "-framework Metal"
        "-framework Foundation"
        "-framework QuartzCore"
    )
elseif(ANDROID)
    target_link_libraries(${CMAKE_PROJECT_NAME} PUBLIC
        android
        log
    )
    else()
    target_link_libraries(${CMAKE_PROJECT_NAME} PUBLIC
        ${CMAKE_DL_LIBS}
    )
endif()

if (PROFILING)
    find_package(Threads)
    include_directories(${CMAKE_SOURCE_DIR}/3rdparty/perfetto/sdk)
    add_library(perfetto STATIC ${CMAKE_SOURCE_DIR}/3rdparty/perfetto/sdk/perfetto.cc)
    target_link_libraries(${CMAKE_PROJECT_NAME} perfetto ${CMAKE_THREAD_LIBS_INIT})
    target_compile_definitions(${CMAKE_PROJECT_NAME} PUBLIC PROFILING=1)
endif()
