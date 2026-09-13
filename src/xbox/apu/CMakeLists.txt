# MCPX APU Emulation Library
# Extracted from xemu (https://github.com/xemu-project/xemu)
#
# Voice Processor (VP): ADPCM/PCM decode, sample mixing, envelopes, filters
# Global/Encode Processors: DSP56300 execution, scratch DMA, FIFO output

add_subdirectory(dsp)

# Scope upstream build options to this dependency; no network fetch at build.
function(add_apu_samplerate)
    set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
    set(BUILD_TESTING OFF)
    set(BUILD_SHARED_LIBS OFF)
    set(LIBSAMPLERATE_EXAMPLES OFF)
    set(LIBSAMPLERATE_INSTALL OFF)
    set(CMAKE_POLICY_DEFAULT_CMP0077 NEW)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
    add_subdirectory(${CMAKE_SOURCE_DIR}/third_party/libsamplerate
                     ${CMAKE_BINARY_DIR}/libsamplerate EXCLUDE_FROM_ALL)
endfunction()
add_apu_samplerate()

add_library(xbox_apu STATIC
    apu_core.c
    apu_vp.c
    apu_dsp.c
    dsp/dsp.c
    dsp/dsp_jit.c
    dsp/dsp_dma.c
    apu_mmio_hook.c
    apu_xaudio2.c
)

target_include_directories(xbox_apu PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(xbox_apu PRIVATE dsp56300_engine)
target_link_libraries(xbox_apu PUBLIC SampleRate::samplerate)

# qemu_shim.h lives in nv2a/ - shared QEMU type abstraction
target_include_directories(xbox_apu PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../nv2a
    ${CMAKE_CURRENT_SOURCE_DIR}/dsp
)

if(MSVC)
    # Audio processing must keep pace with 1500 VP/GP frames per second.
    set_source_files_properties(dsp/dsp.c dsp/dsp_jit.c dsp/dsp_dma.c PROPERTIES COMPILE_OPTIONS "/O2;/Ob2;/fp:precise")
    target_compile_options(samplerate PRIVATE /O2 /Ob2 /fp:precise)
    target_compile_options(xbox_apu PRIVATE
        /W3
        /wd4244  # conversion warnings (float/double/int)
        /wd4146  # unary minus on unsigned
        /wd4267  # size_t to int conversion
    )
    target_compile_definitions(xbox_apu PRIVATE
        _CRT_SECURE_NO_WARNINGS
        _USE_MATH_DEFINES
    )
endif()

# The root build compiles src/platform straight into conker_recomp rather than
# building the `platform` library target, so link nothing here and just take
# the include path -- the same arrangement src/nv2a uses.
target_include_directories(xbox_apu PRIVATE
    ${CMAKE_SOURCE_DIR}/src/xbox
    ${CMAKE_SOURCE_DIR}/runtime/xbox
)

if(WIN32)
    target_link_libraries(xbox_apu PRIVATE kernel32 winmm)
else()
    # Linux: SDL2 audio + libm
    find_package(SDL2 REQUIRED)
    target_link_libraries(xbox_apu PRIVATE SDL2::SDL2 m)
endif()
