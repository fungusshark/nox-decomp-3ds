cmake_minimum_required(VERSION 3.13)
project(nox-3ds C CXX)

get_filename_component(NOX_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
set(NOX_SRC_DIR "${NOX_ROOT}/src")

if (NOT NINTENDO_3DS)
    message(FATAL_ERROR "port/3ds must be configured with -DCMAKE_TOOLCHAIN_FILE=/opt/devkitpro/cmake/3DS.cmake")
endif()

# Mandatory ARM11 / Nox decomp diagnostics and ABI flags
add_compile_options(
    -g
    -O2
    -Wcast-align
    -fshort-wchar
    -fno-strict-aliasing
    -fno-strict-overflow
    -fcommon
    -Wno-error=int-conversion
    -Wno-error=incompatible-pointer-types
)

# ABI wrapper library for ARM32 AAPCS-VFP pointer-in-float slots
add_library(abi_wrappers STATIC "${NOX_ROOT}/abi_wrappers.c")
set_target_properties(abi_wrappers PROPERTIES C_STANDARD 99)

add_executable(nox
    "${NOX_SRC_DIR}/GAME1.c"
    "${NOX_SRC_DIR}/GAME2.c"
    "${NOX_SRC_DIR}/GAME3.c"
    "${NOX_SRC_DIR}/GAME4.c"
    "${NOX_SRC_DIR}/GAME5.c"
    "${NOX_SRC_DIR}/GAME_ABI.c"
    "${NOX_SRC_DIR}/GAME_data.c"
    "${NOX_SRC_DIR}/cdrom.c"
    "${NOX_SRC_DIR}/compat.c"
    "${NOX_SRC_DIR}/compat_mss.c"
    "${NOX_SRC_DIR}/draw.c"
    "${NOX_SRC_DIR}/gamepad.c"
    "${NOX_SRC_DIR}/imm.c"
    "${NOX_SRC_DIR}/input.c"
    "${NOX_SRC_DIR}/main.cpp"
    "${NOX_SRC_DIR}/sm.c"
    "${NOX_SRC_DIR}/string.c"
    "${NOX_SRC_DIR}/win.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/platform_3ds.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/video_3ds.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/input_3ds.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/stubs.c"
)

target_include_directories(nox PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
    "${NOX_SRC_DIR}/external/include"
)

# Link order matters for static archives: citro2d -> citro3d -> ctru -> zlib
target_link_libraries(nox PRIVATE
    citro2d
    citro3d
    ctru
    z
    abi_wrappers
    m
)

# Package nox.elf -> nox.3dsx (target name created: nox_3dsx)
ctr_create_3dsx(nox)

# Post-build copy to /mnt/c/3ds-dev/nox.3dsx for emulator / SD staging
add_custom_target(copy-nox-3dsx ALL
    COMMAND ${CMAKE_COMMAND} -E make_directory /mnt/c/3ds-dev
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_BINARY_DIR}/nox.3dsx"
        "/mnt/c/3ds-dev/nox.3dsx"
    DEPENDS nox_3dsx
    COMMENT "Copying nox.3dsx to /mnt/c/3ds-dev/nox.3dsx"
)

# Wireless hardware push via 3dslink
find_program(CTR_3DSLINK_EXE NAMES 3dslink HINTS "${DEVKITPRO}/tools/bin")
if (CTR_3DSLINK_EXE)
    add_custom_target(run-3ds
        COMMAND "${CTR_3DSLINK_EXE}" "build-3ds/nox.3dsx"
        DEPENDS nox_3dsx
        WORKING_DIRECTORY "${NOX_ROOT}"
        COMMENT "Sending build-3ds/nox.3dsx to 3DS via 3dslink"
    )
else()
    add_custom_target(run-3ds
        COMMAND ${CMAKE_COMMAND} -E echo "Error: 3dslink not found in ${DEVKITPRO}/tools/bin"
        COMMAND ${CMAKE_COMMAND} -E false
    )
endif()
