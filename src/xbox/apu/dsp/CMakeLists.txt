# Pinned, open-source DSP implementation. Guest microcode is never bundled.
find_program(DSP_CARGO_EXECUTABLE cargo REQUIRED)
set(DSP_ENGINE_SOURCE "${CMAKE_SOURCE_DIR}/third_party/dsp56300")
set(DSP_ENGINE_TARGET "${CMAKE_BINARY_DIR}/dsp56300/target" CACHE PATH "DSP Cargo build directory")
if(WIN32)
    set(DSP_ENGINE_LIBRARY "${DSP_ENGINE_TARGET}/release/dsp56300_emu_ffi.lib")
else()
    set(DSP_ENGINE_LIBRARY "${DSP_ENGINE_TARGET}/release/libdsp56300_emu_ffi.a")
endif()
file(GLOB_RECURSE DSP_ENGINE_INPUTS CONFIGURE_DEPENDS
    "${DSP_ENGINE_SOURCE}/crates/*.rs"
    "${DSP_ENGINE_SOURCE}/crates/*/Cargo.toml")
add_custom_command(OUTPUT "${DSP_ENGINE_LIBRARY}"
    COMMAND "${DSP_CARGO_EXECUTABLE}" build --release --locked
        --manifest-path "${DSP_ENGINE_SOURCE}/Cargo.toml"
        --target-dir "${DSP_ENGINE_TARGET}" -p dsp56300-emu-ffi
    WORKING_DIRECTORY "${CMAKE_BINARY_DIR}"
    DEPENDS ${DSP_ENGINE_INPUTS} "${DSP_ENGINE_SOURCE}/Cargo.lock"
        "${DSP_ENGINE_SOURCE}/Cargo.toml"
        "${DSP_ENGINE_SOURCE}/crates/emu-ffi/cbindgen.toml"
        "${DSP_ENGINE_SOURCE}/crates/emu-ffi/examples/quickstart.c"
    COMMENT "Building the DSP56300 audio processor"
    VERBATIM)
add_custom_target(dsp56300_build DEPENDS "${DSP_ENGINE_LIBRARY}")
add_library(dsp56300_engine STATIC IMPORTED GLOBAL)
set_target_properties(dsp56300_engine PROPERTIES
    IMPORTED_LOCATION "${DSP_ENGINE_LIBRARY}"
    INTERFACE_INCLUDE_DIRECTORIES "${DSP_ENGINE_SOURCE}/crates/emu-ffi/include")
add_dependencies(dsp56300_engine dsp56300_build)
if(WIN32)
    set_property(TARGET dsp56300_engine PROPERTY INTERFACE_LINK_LIBRARIES
        "ws2_32;userenv;ntdll;bcrypt")
else()
    set_property(TARGET dsp56300_engine PROPERTY INTERFACE_LINK_LIBRARIES
        "pthread;dl;m")
endif()
