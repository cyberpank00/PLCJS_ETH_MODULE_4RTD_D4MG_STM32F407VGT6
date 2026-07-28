cmake_minimum_required(VERSION 3.22)

#
# This file is generated only once,
# and is not re-generated if converter is called multiple times.
#
# User is free to modify the file as much as necessary
#

# Setup compiler settings
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)


# Define the build type
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "Debug")
endif()

# Set the project name
set(CMAKE_PROJECT_NAME PLCJS_ETH_MODULE_4RTD_D4MG_STM32F407VGT6)

# Enable compile command to ease indexing with e.g. clangd
set(CMAKE_EXPORT_COMPILE_COMMANDS TRUE)

# Core project settings
project(${CMAKE_PROJECT_NAME})
message("Build type: " ${CMAKE_BUILD_TYPE})

# Enable CMake support for ASM and C languages
enable_language(C ASM)

# Create an executable object type
add_executable(${CMAKE_PROJECT_NAME})

# Add STM32CubeMX generated sources
add_subdirectory(cmake/stm32cubemx)

# Link directories setup
target_link_directories(${CMAKE_PROJECT_NAME} PRIVATE
    # Add user defined library search paths
)

# ---------- Module identity (overridable per module variant) ---------------
# Pass -DPRODUCT_ID=0x... / -DHW_REVISION=0xMMmm / -DFW_VERSION=0xMMmm on the
# cmake command line to customise the firmware header for each module variant.
#
# HW_REVISION encoding: (major << 8) | minor  →  e.g. hw:01.02 = 0x0102
# FW_VERSION  encoding: (major << 8) | minor  →  e.g. fw:01.02 = 0x0102
# OTA acceptance: product_id exact match + hw_revision major byte match.
set(PRODUCT_ID    "0x504C0403" CACHE STRING "Product ID embedded in fw_header_t (0x504C=PL, 4ch, RTD)" FORCE)
set(HW_REVISION   "0x0101"     CACHE STRING "HW revision (major<<8|minor), e.g. hw:01.01 = 0x0101" FORCE)
set(FW_VERSION    "0x0100"     CACHE STRING "FW version  (major<<8|minor), e.g. fw:01.00 = 0x0100" FORCE)

# Add sources to executable
target_sources(${CMAKE_PROJECT_NAME} PRIVATE
    # Application sources
    Application/app/app.c
    Application/led/led_module.c
    Application/temp/temp_module.c
    Application/button/button_module.c
    Application/ksz8863/ksz8863.c
    Application/net_id/net_id.c
    Application/settings/settings.c
    Application/calstore/calstore.c
    Application/modbus/modbus_app.c
    Application/modbus/modbus_tcp_server.c
    Application/fw_header/fw_header.c
    # RTD analog front-end
    Application/spi/spi_bus.c
    Application/max31865/max31865.c
    Application/rtd/rtd_scales.c
    Application/rtd/rtd_module.c
    # Third-party
    Application/third_party/nanomodbus/nanomodbus.c
)

# Add include paths
target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE
    Application/app
    Application/led
    Application/temp
    Application/button
    Application/ksz8863
    Application/net_id
    Application/settings
    Application/calstore
    Application/modbus
    Application/fw_header
    Application/spi
    Application/max31865
    Application/rtd
    Application/third_party/nanomodbus
)

# Disable nanoMODBUS client side - we only need the slave/server.
target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE
    NMBS_CLIENT_DISABLED=1
)

# Add project symbols (macros) -- firmware header identity fields.
target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE
    FW_PRODUCT_ID=${PRODUCT_ID}
    FW_HW_REVISION=${HW_REVISION}
    FW_VERSION_VALUE=${FW_VERSION}
)

# Remove wrong libob.a library dependency when using cpp files
list(REMOVE_ITEM CMAKE_C_IMPLICIT_LINK_LIBRARIES ob)

# Add linked libraries
target_link_libraries(${CMAKE_PROJECT_NAME}
    stm32cubemx

    # Add user defined libraries
)

# ---------- Post-build: generate .hex and .bin images ---------------------
add_custom_command(TARGET ${CMAKE_PROJECT_NAME} POST_BUILD
    COMMAND ${CMAKE_SIZE} $<TARGET_FILE:${CMAKE_PROJECT_NAME}>
    COMMAND ${CMAKE_OBJCOPY} -O ihex
            $<TARGET_FILE:${CMAKE_PROJECT_NAME}>
            ${CMAKE_BINARY_DIR}/${CMAKE_PROJECT_NAME}.hex
    COMMAND ${CMAKE_OBJCOPY} -O binary
            $<TARGET_FILE:${CMAKE_PROJECT_NAME}>
            ${CMAKE_BINARY_DIR}/${CMAKE_PROJECT_NAME}.bin
    COMMENT "Generating ${CMAKE_PROJECT_NAME}.hex and ${CMAKE_PROJECT_NAME}.bin"
)
