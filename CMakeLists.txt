cmake_minimum_required(VERSION 3.22)

# Configure vcpkg toolchain before project() call
# This must be set before project() to allow vcpkg to set up the build system
if (NOT DEFINED CMAKE_TOOLCHAIN_FILE)
    set(CMAKE_TOOLCHAIN_FILE "${CMAKE_CURRENT_SOURCE_DIR}/external/vcpkg/scripts/buildsystems/vcpkg.cmake" CACHE STRING "")
endif()

# Set vcpkg overlay triplets to use ravennakit's custom triplets
# This must be set before project() for vcpkg to use it
set(VCPKG_OVERLAY_TRIPLETS "${CMAKE_CURRENT_SOURCE_DIR}/external/ravennakit/triplets" CACHE STRING "" FORCE)

# Determine vcpkg triplet based on platform
# Note: This must be set before project() for vcpkg to use it
if (APPLE)
    # Check the actual system architecture, not CMAKE_OSX_ARCHITECTURES which may not be set yet
    execute_process(
        COMMAND uname -m
        OUTPUT_VARIABLE APPLE_ARCH
        OUTPUT_STRIP_TRAILING_WHITESPACE
    )
    if (APPLE_ARCH STREQUAL "arm64")
        set(VCPKG_TARGET_TRIPLET "macos-arm64" CACHE STRING "")
    else()
        set(VCPKG_TARGET_TRIPLET "macos-x86-64" CACHE STRING "")
    endif()
elseif (WIN32)
    set(VCPKG_TARGET_TRIPLET "windows-x64" CACHE STRING "")
elseif (UNIX)
    set(VCPKG_TARGET_TRIPLET "linux-x64" CACHE STRING "")
endif()

project(JUCECMakeRepo)

#First, we'll add the CMake folder, incase we'll need to find_package later:
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/CMake")

#Compile commands, useful for some IDEs like VS-Code
set(CMAKE_EXPORT_COMPILE_COMMANDS TRUE)

#Minimum MacOS target, set globally
if (${CMAKE_SYSTEM_NAME} STREQUAL "iOS")
    set(CMAKE_OSX_DEPLOYMENT_TARGET 11.0 CACHE STRING "Minimum OS X deployment version" FORCE)

    #code signing to run on an iOS device:
    #    set(CMAKE_XCODE_ATTRIBUTE_CODE_SIGN_IDENTITY "iPhone Developer" CACHE STRING "" FORCE)
    #    set(CMAKE_XCODE_ATTRIBUTE_DEVELOPMENT_TEAM "XXXXXXXXXX" CACHE STRING "" FORCE)
else ()
    # ravennakit requires macOS 10.15+
    set(CMAKE_OSX_DEPLOYMENT_TARGET "10.15" CACHE STRING "Minimum OS X deployment version" FORCE)
endif ()

option(UniversalBinary "Build universal binary for mac" OFF)

if (UniversalBinary)
    set(CMAKE_OSX_ARCHITECTURES "x86_64;arm64" CACHE INTERNAL "")
endif ()

#static linking on Windows
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

#We 'require' that we need juce. If JUCE isn't found, it will revert to what's in
#CMake/Findjuce.cmake, where you can see how JUCE is brought in/configured
find_package(juce REQUIRED)

# Add ravennakit library
# This must be added after JUCE setup as ravennakit may have its own dependencies
add_subdirectory(external/ravennakit)

#By default, we are building the provided plugin/app examples:
option(BUILD_JUCE_PROTOTYPE_EXAMPLES "Build JUCE prototype examples" ON)

#adding any custom modules you might have:
add_subdirectory(Modules)

if (${BUILD_JUCE_PROTOTYPE_EXAMPLES})
    #adding project folders:
    add_subdirectory(TestAES67Sender)
endif ()

#optionally, we're also adding the unit tests:
option(BUILD_UNIT_TESTS "Build JUCE prototype examples" OFF)

if (BUILD_UNIT_TESTS)
    enable_testing()
    add_subdirectory(Tests)
endif ()
