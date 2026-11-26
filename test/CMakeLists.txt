cmake_minimum_required(VERSION 3.25)

set(MF_MIN_VERSION "1.0.0")
set(MF_NEEDS_LILA OFF)
project(mf_kinect_test LANGUAGES C CXX)

enable_testing()

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
endif()

add_compile_definitions(MAYAFLUX_PROJECT)

set(MAYAFLUX_SEARCH_PATHS "")

if(DEFINED ENV{MAYAFLUX_ROOT})
    list(APPEND MAYAFLUX_SEARCH_PATHS "$ENV{MAYAFLUX_ROOT}/lib/cmake/MayaFlux")
    list(APPEND MAYAFLUX_SEARCH_PATHS "$ENV{MAYAFLUX_ROOT}/lib64/cmake/MayaFlux")
elseif(APPLE)
    list(APPEND MAYAFLUX_SEARCH_PATHS "/opt/homebrew/lib/cmake/MayaFlux")
    list(APPEND MAYAFLUX_SEARCH_PATHS "/usr/local/lib/cmake/MayaFlux")
elseif(UNIX)
    list(APPEND MAYAFLUX_SEARCH_PATHS "/usr/lib/cmake/MayaFlux")
    list(APPEND MAYAFLUX_SEARCH_PATHS "/usr/lib64/cmake/MayaFlux")
    list(APPEND MAYAFLUX_SEARCH_PATHS "/usr/local/lib/cmake/MayaFlux")
elseif(WIN32)
    list(APPEND MAYAFLUX_SEARCH_PATHS "C:/MayaFlux/lib/cmake/MayaFlux")
endif()

find_package(MayaFlux REQUIRED PATHS ${MAYAFLUX_SEARCH_PATHS} NO_DEFAULT_PATH)

if(NOT EXISTS "${MayaFlux_PCH_FILE}")
    message(FATAL_ERROR
        "MayaFlux runtime PCH not found at ${MayaFlux_PCH_FILE}\n"
        "Ensure MayaFlux is correctly installed."
    )
endif()

find_package(PkgConfig REQUIRED)
pkg_check_modules(FREENECT REQUIRED libfreenect)

# The test plays back a recording through libfakenect instead of a real Kinect.
find_library(FAKENECT_LIBRARY fakenect PATH_SUFFIXES fakenect REQUIRED)

file(GLOB_RECURSE _mf_kinect_sources CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/../src/*.cpp"
)
if(NOT _mf_kinect_sources)
    set(_mf_kinect_empty_source "${CMAKE_CURRENT_BINARY_DIR}/empty_module.cpp")
    file(WRITE "${_mf_kinect_empty_source}" "")
    list(APPEND _mf_kinect_sources "${_mf_kinect_empty_source}")
endif()

add_library(mf_kinect OBJECT ${_mf_kinect_sources})

set_target_properties(mf_kinect PROPERTIES
    CXX_STANDARD 23
    CXX_STANDARD_REQUIRED ON
)

target_include_directories(mf_kinect PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/../src" ${FREENECT_INCLUDE_DIRS})

target_precompile_headers(mf_kinect PUBLIC "${MayaFlux_PCH_FILE}")
target_link_libraries(mf_kinect PUBLIC MayaFlux::MayaFluxLib)

if(MF_NEEDS_LILA)
    target_link_libraries(mf_kinect PUBLIC MayaFlux::MayaFluxHost)
endif()

# Prefer a GoogleTest installation already available on this machine.
find_package(GTest QUIET)
if(NOT GTest_FOUND)
    include(FetchContent)
    FetchContent_Declare(
        googletest
        URL https://github.com/google/googletest/releases/download/v1.18.0/googletest-1.18.0.tar.gz
        URL_HASH SHA256=6e3191c1455468b3fc35a417fb565c1c5071aee1b7e7f85e30cf48a98d37d8b5
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    )
    set(BUILD_GMOCK OFF)
    if(MSVC)
        # MayaFlux projects use the DLL CRT; keep fetched GoogleTest compatible.
        set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
    endif()
    FetchContent_MakeAvailable(googletest)
endif()

add_executable(test_mf_kinect
    test_mf_kinect.cpp
    $<TARGET_OBJECTS:mf_kinect>
)

target_precompile_headers(test_mf_kinect REUSE_FROM mf_kinect)
target_link_libraries(test_mf_kinect PRIVATE mf_kinect MayaFlux::MayaFluxLib ${FAKENECT_LIBRARY} GTest::gtest)

if(WIN32)
    # MayaFlux's imported targets expose their DLL locations to this expression.
    # Place them beside the executable before CTest runs or discovers tests.
    add_custom_command(TARGET test_mf_kinect POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy
            $<TARGET_RUNTIME_DLLS:test_mf_kinect>
            $<TARGET_FILE_DIR:test_mf_kinect>
        COMMAND_EXPAND_LISTS
        VERBATIM
    )
endif()

include(GoogleTest)
gtest_discover_tests(test_mf_kinect DISCOVERY_MODE PRE_TEST)

if(UNIX)
    execute_process(
        COMMAND ${CMAKE_COMMAND} -E create_symlink
            ${CMAKE_BINARY_DIR}/compile_commands.json
            ${CMAKE_CURRENT_SOURCE_DIR}/../compile_commands.json
    )
endif()
