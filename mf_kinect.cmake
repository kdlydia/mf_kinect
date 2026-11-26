set(MF_MIN_VERSION "1.0.0")
set(MF_NEEDS_LILA OFF)

find_package(PkgConfig REQUIRED)
pkg_check_modules(FREENECT REQUIRED IMPORTED_TARGET libfreenect)

file(GLOB_RECURSE _mf_kinect_sources CONFIGURE_DEPENDS
    "${CMAKE_SOURCE_DIR}/community/mf_kinect/src/*.cpp"
)

add_library(mf_kinect OBJECT
    ${_mf_kinect_sources}
)

target_include_directories(mf_kinect PUBLIC
    "${CMAKE_SOURCE_DIR}/community/mf_kinect/src"
)

set_target_properties(mf_kinect PROPERTIES
    CXX_STANDARD 23
    CXX_STANDARD_REQUIRED ON
)

target_link_libraries(mf_kinect PUBLIC MayaFlux::MayaFluxLib PkgConfig::FREENECT)

if(MF_NEEDS_LILA)
    target_link_libraries(mf_kinect PUBLIC MayaFlux::MayaFluxHost)
endif()
