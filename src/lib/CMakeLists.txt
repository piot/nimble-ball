cmake_minimum_required(VERSION 3.17)
project(nimble_ball C)

set(CMAKE_C_STANDARD 99)

if (CMAKE_BUILD_TYPE STREQUAL "Debug")
    set(isDebug TRUE)
else ()
    message("no debug")
    set(isDebug FALSE)
endif ()


if (isDebug)
    message("Debug build detected")
    add_compile_definitions(CONFIGURATION_DEBUG TYRAN_CONFIGURATION_DEBUG)
    set(CONFIGURATION_DEBUG 1)
endif ()


set(DEPS ../../deps/)


file(GLOB_RECURSE deps_src FOLLOW_SYMLINKS
        "${DEPS}*/*/src/lib/*.c"
        "${DEPS}*/*/src/platform/posix/*.c"
        )

file(GLOB src FOLLOW_SYMLINKS
        "*.c"
        )

message("something ${example_src}")

add_executable(nimble_ball
        ${src}
        ${deps_src}
        )

find_package(SDL2 REQUIRED COMPONENTS SDL2)
find_package(SDL2_image REQUIRED COMPONENTS SDL2_image>=2.0.0)
find_package(SDL2_ttf REQUIRED COMPONENTS SDL2_ttf>=2.20.0)
target_link_libraries(nimble_ball PUBLIC SDL2::SDL2 SDL2_image::SDL2_image SDL2_ttf::SDL2_ttf)

if (WIN32)
else ()
    target_link_libraries(nimble_ball PUBLIC m)
endif (WIN32)

target_compile_options(nimble_ball PRIVATE -Wall -Wextra -Wshadow -Wstrict-aliasing -ansi -pedantic -Wno-declaration-after-statement -Wno-extra-semi-stmt -Wno-undef -Wno-unused-variable -Wno-unused-parameter -Wno-padded -Werror=implicit-function-declaration -Werror=incompatible-pointer-types -Werror=missing-prototypes -Werror=int-conversion -Werror=return-type -Werror=incompatible-function-pointer-types) # -Werror=incompatible-pointer-types-discards-qualifiers
if (COMPILER_CLANG)
    target_compile_options(nimble_ball PRIVATE -Wmost -Weverything -Werror=missing-variable-declarations)
endif ()

target_include_directories(nimble_ball PRIVATE ${DEPS}piot/clog/src/include)
target_include_directories(nimble_ball PRIVATE ${DEPS}piot/tiny-libc/src/include)
target_include_directories(nimble_ball PRIVATE ${DEPS}piot/transmute-c/src/include)
target_include_directories(nimble_ball PRIVATE ${DEPS}piot/sdl-render/src/include)
target_include_directories(nimble_ball PRIVATE ${DEPS}piot/rectify-c/src/include)
target_include_directories(nimble_ball PRIVATE ${DEPS}piot/imprint/src/include)
target_include_directories(nimble_ball PRIVATE ${DEPS}piot/nimble-steps-serialize-c/src/include)
target_include_directories(nimble_ball PRIVATE ${DEPS}piot/nimble-steps-c/src/include)
target_include_directories(nimble_ball PRIVATE ${DEPS}piot/assent-c/src/include)
target_include_directories(nimble_ball PRIVATE ${DEPS}piot/seer-c/src/include)
target_include_directories(nimble_ball PRIVATE ${DEPS}piot/flood-c/src/include)
target_include_directories(nimble_ball PRIVATE ${DEPS}piot/discoid-c/src/include)
target_include_directories(nimble_ball PRIVATE ${DEPS}piot/udp-server-c/src/include)
target_include_directories(nimble_ball PRIVATE ${DEPS}piot/udp-client-c/src/include)
target_include_directories(nimble_ball PRIVATE ${DEPS}piot/hazy-c/src/include)
target_include_directories(nimble_ball PRIVATE ${DEPS}piot/basal-c/src/include)
target_include_directories(nimble_ball PRIVATE ${DEPS}piot/monotonic-time-c/src/include)
target_include_directories(nimble_ball PRIVATE ${DEPS}piot/udp-transport/src/include)
target_include_directories(nimble_ball PRIVATE ${DEPS}piot/nimble-ball-simulation/src/include)
target_include_directories(nimble_ball PRIVATE ${DEPS}piot/nimble-ball-presentation/src/include)
target_include_directories(nimble_ball PRIVATE ${DEPS}piot/nimble-serialize-c/src/include)
target_include_directories(nimble_ball PRIVATE ${DEPS}piot/ordered-datagram-c/src/include)
target_include_directories(nimble_ball PRIVATE ${DEPS}piot/blob-stream/src/include)
target_include_directories(nimble_ball PRIVATE ${DEPS}piot/stats-c/src/include)
target_include_directories(nimble_ball PRIVATE ${DEPS}piot/bit-array/src/include)
target_include_directories(nimble_ball PRIVATE ${DEPS}piot/nimble-server-lib/src/include)
target_include_directories(nimble_ball PRIVATE ${DEPS}piot/nimble-client-c/src/include)

target_include_directories(nimble_ball PRIVATE ../include)
