cmake_minimum_required(VERSION 3.20)
project(ConkerRecomp LANGUAGES C)
add_subdirectory(src/launcher)

set(CONKER_GENERATED_DIR "" CACHE PATH "Locally prepared generated/recomp directory")
if(CONKER_GENERATED_DIR)
    if(NOT WIN32 OR NOT MSVC)
        message(FATAL_ERROR "The current runtime requires Windows and MSVC.")
    endif()
    if(NOT EXISTS "${CONKER_GENERATED_DIR}/recomp_dispatch.c")
        message(FATAL_ERROR "Prepare your verified ISO first; generated C must stay local.")
    endif()
    set(CMAKE_C_STANDARD 11)
    set(CMAKE_C_STANDARD_REQUIRED ON)
    string(REPLACE "/RTC1" "" CMAKE_C_FLAGS_DEBUG "${CMAKE_C_FLAGS_DEBUG}")
    include_directories(runtime/xbox src/xbox src/xbox/kernel src/xbox/input src/conker "${CONKER_GENERATED_DIR}")
    add_compile_definitions(WIN32_LEAN_AND_MEAN NOMINMAX)
    file(GLOB LOCAL_RECOMP CONFIGURE_DEPENDS "${CONKER_GENERATED_DIR}/recomp_*.c")
    add_executable(conker_recomp
        src/xbox/d3d/d3d8_combiners.c
        src/xbox/d3d/d3d8_compile.c
        src/xbox/d3d/d3d8_device.c
        src/xbox/d3d/d3d8_nv2a_gpu.c
        src/xbox/d3d/d3d8_resources.c
        src/xbox/d3d/d3d8_shaders.c
        src/xbox/d3d/d3d8_states.c
        src/xbox/d3d/d3d8_vsh.c
        src/xbox/input/conker_input.c
        src/xbox/input/input_queue.c
        src/xbox/input/xinput_device.c
        src/xbox/kernel/kernel_bridge.c
        src/xbox/kernel/kernel_crypto.c
        src/xbox/kernel/kernel_file.c
        src/xbox/kernel/kernel_guest_clock.c
        src/xbox/kernel/kernel_guest_timer.c
        src/xbox/kernel/kernel_hal.c
        src/xbox/kernel/kernel_io.c
        src/xbox/kernel/kernel_memory.c
        src/xbox/kernel/kernel_ob.c
        src/xbox/kernel/kernel_path.c
        src/xbox/kernel/kernel_pool.c
        src/xbox/kernel/kernel_rtl.c
        src/xbox/kernel/kernel_sync.c
        src/xbox/kernel/kernel_thread.c
        src/xbox/kernel/kernel_thunks.c
        src/xbox/kernel/kernel_xbox.c
        src/xbox/kernel/recomp_esp_check.c
        src/xbox/kernel/recomp_globals.c
        src/xbox/kernel/xbox_memory.c
        src/xbox/kernel/xbox_memory_layout.c
        src/conker/main.c
        src/xbox/nv2a/nv2a_pgraph_d3d11.c
        src/xbox/nv2a/nv2a_pushbuffer.c
        src/xbox/nv2a/nv2a_vsh.c
        src/xbox/nv2a/nv2a_vsh_hlsl.c
        src/conker/runtime_config.c
        src/conker/integration.c
        ${LOCAL_RECOMP})
    set_source_files_properties(${LOCAL_RECOMP} PROPERTIES COMPILE_OPTIONS "/w" COMPILE_DEFINITIONS RECOMP_GENERATED_CODE)
    target_compile_options(conker_recomp PRIVATE /utf-8)
    file(GLOB GPU_RUNTIME src/xbox/nv2a/nv2a_vsh*.c src/xbox/nv2a/nv2a_pgraph_d3d11.c src/xbox/nv2a/nv2a_pushbuffer.c src/xbox/d3d/*.c)
    set_source_files_properties(${GPU_RUNTIME} PROPERTIES COMPILE_OPTIONS "/O2;/Ob2;/fp:precise")
    add_subdirectory(src/xbox/nv2a)
    add_subdirectory(src/xbox/apu)
    target_link_libraries(conker_recomp PRIVATE xbox_nv2a xbox_apu kernel32 user32 ole32 d3d11 dxgi dxguid d3dcompiler bcrypt xinput)
endif()
