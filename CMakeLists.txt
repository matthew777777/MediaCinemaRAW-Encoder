cmake_minimum_required(VERSION 3.16)
project(MediaCinemaRAWEncoder VERSION 0.1.0 LANGUAGES CXX)

# Static library, no dependencies beyond the C++ standard library (+ threads
# for the opt-in encode_parallel() entry point).
add_library(MediaCinemaRAWEncoder
    src/ContainerWriter.cpp
    src/Encoder.cpp)
add_library(MediaCinemaRAW::Encoder ALIAS MediaCinemaRAWEncoder)
set_target_properties(MediaCinemaRAWEncoder PROPERTIES
    EXPORT_NAME Encoder
    POSITION_INDEPENDENT_CODE ON)
target_compile_features(MediaCinemaRAWEncoder PUBLIC cxx_std_17)
target_include_directories(MediaCinemaRAWEncoder
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>)
if(MSVC)
    target_compile_options(MediaCinemaRAWEncoder PRIVATE /W4)
else()
    target_compile_options(MediaCinemaRAWEncoder PRIVATE -Wall -Wextra -Wpedantic)
endif()

# encode_parallel() uses std::thread; PUBLIC so static-link consumers inherit it.
find_package(Threads REQUIRED)
target_link_libraries(MediaCinemaRAWEncoder PUBLIC Threads::Threads)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

install(TARGETS MediaCinemaRAWEncoder EXPORT MediaCinemaRAWEncoderTargets)
install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(EXPORT MediaCinemaRAWEncoderTargets
    NAMESPACE MediaCinemaRAW::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/MediaCinemaRAWEncoder)

configure_package_config_file(
    cmake/MediaCinemaRAWEncoderConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/MediaCinemaRAWEncoderConfig.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/MediaCinemaRAWEncoder)
write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/MediaCinemaRAWEncoderConfigVersion.cmake
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion)
install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/MediaCinemaRAWEncoderConfig.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/MediaCinemaRAWEncoderConfigVersion.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/MediaCinemaRAWEncoder)

option(MEDIACINEMARAW_BUILD_TESTS "Build codec tests" ON)
# Optional path to a motioncam-decoder checkout; enables the upstream
# round-trip and gyro interop suites (equivalent to tools/verify_*.py).
set(MEDIACINEMARAW_MOTIONCAM_DECODER_DIR ""
    CACHE PATH "motioncam-decoder checkout for interop tests")

if(MEDIACINEMARAW_BUILD_TESTS)
    enable_testing()
    add_executable(MediaCinemaRAWEncoderTests tests/EncoderTests.cpp)
    target_link_libraries(MediaCinemaRAWEncoderTests PRIVATE MediaCinemaRAW::Encoder)
    if(MSVC)
        target_compile_options(MediaCinemaRAWEncoderTests PRIVATE /W4 /UNDEBUG)
    else()
        target_compile_options(MediaCinemaRAWEncoderTests PRIVATE -Wall -Wextra -Wpedantic)
        # The suite is assert()-based, but Release/RelWithDebInfo builds
        # define NDEBUG, which would silently compile every check out.
        # Target options come after the config flags on the command line,
        # so this -U wins and ctest actually tests. PRIVATE: the library
        # itself keeps NDEBUG.
        target_compile_options(MediaCinemaRAWEncoderTests PRIVATE -UNDEBUG)
    endif()
    add_test(NAME encoder_validation COMMAND MediaCinemaRAWEncoderTests)

    if(MEDIACINEMARAW_MOTIONCAM_DECODER_DIR)
        set(upstream ${MEDIACINEMARAW_MOTIONCAM_DECODER_DIR})
        foreach(source Decoder.cpp RawData.cpp RawData_Legacy.cpp)
            if(NOT EXISTS ${upstream}/lib/${source})
                message(FATAL_ERROR "missing upstream source: ${upstream}/lib/${source}")
            endif()
        endforeach()
        if(NOT MSVC)
            # Upstream intentionally uses unaligned access; keep the
            # alignment sanitizer off those objects if the user enabled
            # sanitizers. A no-op flag otherwise.
            set_source_files_properties(
                ${upstream}/lib/Decoder.cpp
                ${upstream}/lib/RawData.cpp
                ${upstream}/lib/RawData_Legacy.cpp
                PROPERTIES COMPILE_OPTIONS "-fno-sanitize=alignment")
        endif()
        foreach(target CompatibilityTests ContainerGyroInterop)
            add_executable(${target}
                tests/${target}.cpp
                ${upstream}/lib/Decoder.cpp
                ${upstream}/lib/RawData.cpp
                ${upstream}/lib/RawData_Legacy.cpp)
            target_include_directories(${target} PRIVATE
                ${upstream}/lib/include
                ${upstream}/thirdparty)
            target_link_libraries(${target} PRIVATE MediaCinemaRAW::Encoder)
            add_test(NAME ${target} COMMAND ${target})
        endforeach()
    endif()
endif()
