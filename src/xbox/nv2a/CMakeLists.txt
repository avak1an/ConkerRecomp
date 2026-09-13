# NV2A GPU Emulation Library
# Extracted from xemu (https://github.com/xemu-project/xemu)
#
# Register handlers: PMC, PBUS, PTIMER, PFB, PCRTC, PRAMDAC, PFIFO, PGRAPH
# MMIO interception via VEH (Vectored Exception Handling)
# Push buffer parsing and command dispatch

# Only the register model and the MMIO interception belong in the shipped
# binary.  Deliberately excluded:
#
#   nv2a_pgraph_d3d11.c - already compiled directly by the root CMakeLists for
#                         the push-buffer path; listing it here too would define
#                         every symbol twice.
#   nv2a_pb_replay.c    - replays push-buffer data captured from xemu.  The
#                         project forbids captured bytes as runtime input, so it
#                         must never reach a shipped build.
#   nv2a_pb_test.c      - a synthetic command generator for bringing the parser
#                         up; not needed once real guest push buffers arrive.
add_library(xbox_nv2a STATIC
    nv2a_core.c
    nv2a_mmio_hook.c
    nv2a_user_dma.c
)

target_include_directories(xbox_nv2a PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

if(MSVC)
    target_compile_options(xbox_nv2a PRIVATE
        /W3
        /wd4244
        /wd4146
        /wd4267
    )
    target_compile_definitions(xbox_nv2a PRIVATE
        _CRT_SECURE_NO_WARNINGS
    )
else()
    target_compile_options(xbox_nv2a PRIVATE -Wall -Wextra -Wno-unused-parameter -O2)
endif()

# The root build compiles src/platform straight into conker_recomp rather than
# building the `platform` library target, so link nothing here and just take the
# include path -- nv2a only needs platform/xbox_winnt.h for PCONTEXT.
target_include_directories(xbox_nv2a PRIVATE
    ${CMAKE_SOURCE_DIR}/src/xbox
    ${CMAKE_SOURCE_DIR}/runtime/xbox
)
if(WIN32)
    target_link_libraries(xbox_nv2a PRIVATE kernel32)
endif()
