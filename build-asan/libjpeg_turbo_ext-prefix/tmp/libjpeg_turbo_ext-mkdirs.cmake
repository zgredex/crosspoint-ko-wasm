# Distributed under the OSI-approved BSD 3-Clause License.  See accompanying
# file LICENSE.rst or https://cmake.org/licensing for details.

cmake_minimum_required(VERSION ${CMAKE_VERSION}) # this file comes with cmake

# If CMAKE_DISABLE_SOURCE_CHANGES is set to true and the source directory is an
# existing directory in our source tree, calling file(MAKE_DIRECTORY) on it
# would cause a fatal error, even though it would be a no-op.
if(NOT EXISTS "/Users/patryk/krxtc/ko-wasm/third_party/libjpeg-turbo")
  file(MAKE_DIRECTORY "/Users/patryk/krxtc/ko-wasm/third_party/libjpeg-turbo")
endif()
file(MAKE_DIRECTORY
  "/Users/patryk/krxtc/ko-wasm/build-asan/libjpeg-turbo-build"
  "/Users/patryk/krxtc/ko-wasm/build-asan/libjpeg-install"
  "/Users/patryk/krxtc/ko-wasm/build-asan/libjpeg_turbo_ext-prefix/tmp"
  "/Users/patryk/krxtc/ko-wasm/build-asan/libjpeg_turbo_ext-prefix/src/libjpeg_turbo_ext-stamp"
  "/Users/patryk/krxtc/ko-wasm/build-asan/libjpeg_turbo_ext-prefix/src"
  "/Users/patryk/krxtc/ko-wasm/build-asan/libjpeg_turbo_ext-prefix/src/libjpeg_turbo_ext-stamp"
)

set(configSubDirs )
foreach(subDir IN LISTS configSubDirs)
    file(MAKE_DIRECTORY "/Users/patryk/krxtc/ko-wasm/build-asan/libjpeg_turbo_ext-prefix/src/libjpeg_turbo_ext-stamp/${subDir}")
endforeach()
if(cfgdir)
  file(MAKE_DIRECTORY "/Users/patryk/krxtc/ko-wasm/build-asan/libjpeg_turbo_ext-prefix/src/libjpeg_turbo_ext-stamp${cfgdir}") # cfgdir has leading slash
endif()
