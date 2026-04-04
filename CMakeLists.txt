# Generated Cmake Pico project file

cmake_minimum_required(VERSION 3.13)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# RelWithDebInfo keeps debug symbols while enabling -O2 optimisation.
# This is important for audio: -O0 is too slow for the per-sample ISR.
set(CMAKE_BUILD_TYPE RelWithDebInfo)

# Initialise pico_sdk from installed location
# (note this can come from environment, CMake cache etc)

# == DO NOT EDIT THE FOLLOWING LINES for the Raspberry Pi Pico VS Code Extension to work ==
if(WIN32)
    set(USERHOME $ENV{USERPROFILE})
else()
    set(USERHOME $ENV{HOME})
endif()
set(sdkVersion 2.1.1)
set(toolchainVersion 14_2_Rel1)
set(picotoolVersion 2.1.1)
set(picoVscode ${USERHOME}/.pico-sdk/cmake/pico-vscode.cmake)
if (EXISTS ${picoVscode})
    include(${picoVscode})
endif()
# ====================================================================================
set(PICO_BOARD pico CACHE STRING "Board type")

# Pull in Raspberry Pi Pico SDK (must be before project)
include(pico_sdk_import.cmake)

project(test C CXX ASM)

# Initialise the Raspberry Pi Pico SDK
pico_sdk_init()

add_subdirectory(lib/FatFs_SPI build)

# Add executable. Default name is the project name, version 0.1

file(GLOB SRC_FILES
    "${CMAKE_CURRENT_LIST_DIR}/*.c",
    "${CMAKE_CURRENT_LIST_DIR}/*.cpp"
)


add_executable(
    test
    ${SRC_FILES}
)

pico_set_program_name(test "test")
pico_set_program_version(test "0.1")

# Modify the below lines to enable/disable output over UART/USB
pico_enable_stdio_uart(test 1)
pico_enable_stdio_usb(test 0)

# Add the standard library to the build
target_link_libraries(
    test
    pico_stdlib
    hardware_pwm
    hardware_adc
    hardware_dma
    pico_multicore
    FatFs_SPI)

# Add the standard include files to the build
target_include_directories(test PRIVATE
        ${CMAKE_CURRENT_LIST_DIR}
)

pico_add_extra_outputs(test)