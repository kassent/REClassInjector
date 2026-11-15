cmake_minimum_required(VERSION 3.21)
project(REClassInjector LANGUAGES C CXX)
if(NOT WIN32 OR NOT MSVC OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "This project requires Windows x64 and MSVC")
endif()
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
add_compile_definitions(_CRT_SECURE_NO_WARNINGS NOMINMAX)
if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/common/CMakeLists.txt")
    message(FATAL_ERROR "Missing common submodule: run git submodule update --init --recursive")
endif()
find_package(spdlog CONFIG REQUIRED)
find_package(fmt CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(ZLIB REQUIRED)
include(CTest)
add_subdirectory(common)

# Build-only by default. Never write into a game installation automatically.
set(HID_WRAPPER_DEPLOY OFF CACHE BOOL "Explicit opt-in game deployment")
add_subdirectory(plugins/HIDWrapper)
if(BUILD_TESTING)
    add_test(NAME ci.metadata COMMAND ClassInjectorTests)
    add_test(NAME ci.fields COMMAND ClassInjectorFieldLayoutTests)
    add_test(NAME ci.enums COMMAND ClassInjectorEnumMetadataTests)
    add_test(NAME ci.provider-abi COMMAND ClassInjectorProviderAbiTests
        $<TARGET_FILE:ClassInjectorLegacyProvider> $<TARGET_FILE:ClassInjectorMismatchedProvider>
        $<TARGET_FILE:CIExampleProvider>)
    add_test(NAME ci.hid COMMAND HIDWrapperTests $<TARGET_FILE:HIDWrapper>)
    add_test(NAME common.virtual-call COMMAND CommonManagedVirtualCallTests)
    add_test(NAME common.allocation COMMAND CommonManagedAllocationTests)
    add_custom_target(check
        COMMAND ${CMAKE_CTEST_COMMAND} --output-on-failure -C $<CONFIG>
        DEPENDS ClassInjectorTests ClassInjectorFieldLayoutTests ClassInjectorEnumMetadataTests
            ClassInjectorProviderAbiTests HIDWrapperTests HIDWrapper
            CommonManagedVirtualCallTests CommonManagedAllocationTests)
endif()
