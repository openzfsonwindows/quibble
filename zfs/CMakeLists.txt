cmake_minimum_required(VERSION 3.14)

project(zfs-uefi C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

if(NOT (${CMAKE_C_COMPILER_ID} STREQUAL "GNU" OR ${CMAKE_C_COMPILER_ID} STREQUAL "Clang"))
    message(FATAL_ERROR "zfs-uefi can only be built with GCC or Clang")
endif()

include_directories(/usr/include/efi)
if (${CMAKE_SYSTEM_PROCESSOR} STREQUAL "AMD64" OR ${CMAKE_SYSTEM_PROCESSOR} STREQUAL "x86_64")
    include_directories(/usr/include/efi/x86_64)
elseif (${CMAKE_SYSTEM_PROCESSOR} STREQUAL "X86")
    include_directories(/usr/include/efi/ia32)
endif()

# -----------------------------------

# The part that talks to EFI.
set(EFI_SRC_FILES src/zfs.c
    src/misc.c
    src/runtime.c)

# FreeBSD's boot-time ZFS reader, and what goes with it. This is built against
# its own set of headers (src/compat and src/engine), which stand in for the
# BSD libsa environment it was written for, and which clash with gnu-efi's, so
# none of it sees the EFI headers and the EFI part never sees these.
set(ENGINE_SRC_FILES src/zfsengine.c
    src/engine/nvlist.c
    src/engine/lz4.c
    src/engine/skein.c
    src/engine/skein_block.c
    src/engine/list.c
    src/engine/blake3_stub.c)

set_source_files_properties(${ENGINE_SRC_FILES} PROPERTIES COMPILE_FLAGS
    "-include ${CMAKE_CURRENT_SOURCE_DIR}/src/compat/prefix.h -I${CMAKE_CURRENT_SOURCE_DIR}/src/compat -I${CMAKE_CURRENT_SOURCE_DIR}/src/engine -w")

add_executable(zfs ${EFI_SRC_FILES} ${ENGINE_SRC_FILES})

set_target_properties(zfs PROPERTIES SUFFIX ".efi")

target_include_directories(zfs PRIVATE src)

target_compile_options(zfs PRIVATE "-ffreestanding")
target_compile_options(zfs PRIVATE "-fno-stack-protector")
target_compile_options(zfs PRIVATE "-fno-stack-check")
target_compile_options(zfs PRIVATE "-mno-stack-arg-probe")
target_compile_options(zfs PRIVATE "-Werror-implicit-function-declaration")

# memcpy and friends are ours, so don't let the compiler turn a loop in one of
# them into a call to itself
target_compile_options(zfs PRIVATE "-fno-tree-loop-distribute-patterns")

target_link_options(zfs PRIVATE "-nostartfiles")
target_link_options(zfs PRIVATE "-shared")

if(${CMAKE_C_COMPILER_ID} STREQUAL "Clang")
    target_link_options(zfs PRIVATE "-Wl,--subsystem,efi_boot_service_driver")
else()
    target_link_options(zfs PRIVATE "-Wl,--subsystem,11")
endif()

if (${CMAKE_SYSTEM_PROCESSOR} STREQUAL "X86")
    target_link_options(zfs PRIVATE "-e_efi_main")
else()
    target_link_options(zfs PRIVATE "-eefi_main")
endif()
