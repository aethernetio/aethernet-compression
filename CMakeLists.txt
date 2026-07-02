cmake_minimum_required(VERSION 3.20)

project(
  aethernet-compression
  VERSION 0.1.0
  DESCRIPTION "Small-message dictionary modeling for constrained devices"
  HOMEPAGE_URL "https://github.com/aethernetio/aethernet-compression"
  LANGUAGES CXX)

include(GNUInstallDirs)

add_library(ae-compression INTERFACE)
add_library(ae-compression::ae-compression ALIAS ae-compression)

target_compile_features(ae-compression INTERFACE cxx_std_20)
target_include_directories(
  ae-compression
  INTERFACE $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
            $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)

option(AE_COMPRESSION_BUILD_TESTS "Build ae-compression tests" ${PROJECT_IS_TOP_LEVEL})
option(AE_COMPRESSION_BUILD_EXAMPLES "Build ae-compression examples" ${PROJECT_IS_TOP_LEVEL})
option(AE_COMPRESSION_BUILD_ZLIB_BENCHMARK "Build the zlib comparison benchmark" ${PROJECT_IS_TOP_LEVEL})

if(AE_COMPRESSION_BUILD_TESTS)
  enable_testing()
  add_executable(ae-compression-tests tests/compression_tests.cpp)
  target_link_libraries(ae-compression-tests PRIVATE ae-compression::ae-compression)
  add_test(NAME ae-compression-tests COMMAND ae-compression-tests)
endif()

if(AE_COMPRESSION_BUILD_EXAMPLES)
  add_executable(ae-compression-pack examples/pack_file.cpp)
  target_link_libraries(ae-compression-pack PRIVATE ae-compression::ae-compression)
endif()

if(AE_COMPRESSION_BUILD_ZLIB_BENCHMARK)
  find_package(ZLIB QUIET)
  if(ZLIB_FOUND)
    add_executable(ae-compression-repeating-benchmark examples/repeating_text_benchmark.cpp)
    target_link_libraries(
      ae-compression-repeating-benchmark
      PRIVATE ae-compression::ae-compression ZLIB::ZLIB)
  else()
    message(STATUS "ZLIB not found; ae-compression-repeating-benchmark will not be built")
  endif()
endif()

install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(
  TARGETS ae-compression
  EXPORT ae-compression-targets
  INCLUDES
  DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(
  EXPORT ae-compression-targets
  NAMESPACE ae-compression::
  FILE ae-compression-targets.cmake
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/ae-compression)
