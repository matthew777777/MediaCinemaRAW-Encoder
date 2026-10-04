cmake_minimum_required(VERSION 3.16)
project(MediaCinemaRAWEncoder VERSION 0.1.0 LANGUAGES CXX)

add_library(MediaCinemaRAWEncoder src/Encoder.cpp src/ContainerWriter.cpp)
add_library(MediaCinemaRAW::Encoder ALIAS MediaCinemaRAWEncoder)
set_target_properties(MediaCinemaRAWEncoder PROPERTIES EXPORT_NAME Encoder)
target_compile_features(MediaCinemaRAWEncoder PUBLIC cxx_std_17)
target_compile_options(MediaCinemaRAWEncoder PRIVATE -O3)
target_include_directories(MediaCinemaRAWEncoder
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>)
# encode_parallel() uses std::thread; PUBLIC so static-link consumers inherit it.
find_package(Threads REQUIRED)
target_link_libraries(MediaCinemaRAWEncoder PUBLIC Threads::Threads)

include(GNUInstallDirs)
install(TARGETS MediaCinemaRAWEncoder EXPORT MediaCinemaRAWEncoderTargets)
install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(EXPORT MediaCinemaRAWEncoderTargets
    NAMESPACE MediaCinemaRAW::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/MediaCinemaRAWEncoder)

option(MEDIACINEMARAW_BUILD_TESTS "Build codec tests" ON)
if(MEDIACINEMARAW_BUILD_TESTS)
    enable_testing()
    add_executable(MediaCinemaRAWEncoderTests tests/EncoderTests.cpp)
    target_link_libraries(MediaCinemaRAWEncoderTests PRIVATE MediaCinemaRAW::Encoder)
    # The suite is assert()-based, but Release/RelWithDebInfo builds define
    # NDEBUG, which would silently compile every check out. Target options
    # come after the config flags on the command line, so this -U wins and
    # ctest actually tests. PRIVATE: the library itself keeps NDEBUG.
    target_compile_options(MediaCinemaRAWEncoderTests PRIVATE
        $<$<CXX_COMPILER_ID:MSVC>:/UNDEBUG>
        $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-UNDEBUG>)
    add_test(NAME encoder_validation COMMAND MediaCinemaRAWEncoderTests)
endif()
