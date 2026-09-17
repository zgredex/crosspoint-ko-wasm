# KO-fork renderer → host + WASM builds from one source tree.
# Host:  cmake -S . -B build && cmake --build build          → ko_xtch_host
# WASM:  emcmake cmake -S . -B build-wasm && cmake --build build-wasm → ko_xtch_wasm.{js,wasm}
cmake_minimum_required(VERSION 3.16)
project(ko_xtch C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

if(EMSCRIPTEN)
  message(STATUS "Building for WASM (emscripten ${EMSCRIPTEN_VERSION})")
  # engine has no exceptions/RTTI; match device flags
  add_compile_options(-fno-exceptions -fno-rtti -Wno-unused-variable -Wno-unused-parameter
                      -Wno-sign-compare -Wno-missing-field-initializers)
else()
  add_compile_options(-Wall -Wno-unused-variable -Wno-unused-parameter -Wno-sign-compare
                      -Wno-missing-field-initializers -fno-exceptions)
endif()
add_compile_definitions(XML_GE=0 XML_CONTEXT_BYTES=1024)

set(STUB_DIR ${CMAKE_CURRENT_SOURCE_DIR}/stubs)
set(VL ${CMAKE_CURRENT_SOURCE_DIR}/vendor-lib)

include_directories(
  ${STUB_DIR}                                  # our shims first (Arduino/Hal*/Logging)
  ${VL}/Epub
  ${VL}/Epub/Epub
  ${VL}/GfxRenderer
  ${VL}/EpdFont
  ${VL}/MiniBidi
  ${VL}/Utf8
  ${VL}/ZipFile
  ${VL}/InflateReader
  ${VL}/Serialization
  ${VL}/FsHelpers
  ${VL}/Memory
  ${VL}/hal
  ${VL}/JpegToBmpConverter
  ${VL}/PngToBmpConverter
  ${VL}/miniz/src
  ${VL}/miniz/third_party
  ${VL}/uzlib/src
  ${VL}/expat
  ${VL}/XmlParserUtils
  ${CMAKE_CURRENT_SOURCE_DIR}/third_party/JPEGDEC
  ${CMAKE_CURRENT_SOURCE_DIR}/third_party/PNGdec
  ${CMAKE_CURRENT_SOURCE_DIR}/third_party/lz4
  ${STUB_DIR}
  ${CMAKE_CURRENT_SOURCE_DIR}/src
)

set(ENGINE_SOURCES
  ${VL}/Epub/Epub.cpp
  ${VL}/Epub/Epub/BookMetadataCache.cpp
  ${VL}/Epub/Epub/Section.cpp
  ${VL}/Epub/Epub/Page.cpp
  ${VL}/Epub/Epub/ParsedText.cpp
  ${VL}/Epub/Epub/htmlEntities.cpp
  ${VL}/Epub/Epub/css/CssParser.cpp
  ${VL}/Epub/Epub/blocks/TextBlock.cpp
  ${VL}/Epub/Epub/blocks/ImageBlock.cpp
  ${VL}/Epub/Epub/parsers/ContainerParser.cpp
  ${VL}/Epub/Epub/parsers/ContentOpfParser.cpp
  ${VL}/Epub/Epub/parsers/TocNavParser.cpp
  ${VL}/Epub/Epub/parsers/TocNcxParser.cpp
  ${VL}/Epub/Epub/parsers/ChapterHtmlSlimParser.cpp
  ${VL}/Epub/Epub/hyphenation/Hyphenator.cpp
  ${VL}/Epub/Epub/hyphenation/LanguageRegistry.cpp
  ${VL}/Epub/Epub/hyphenation/HyphenationCommon.cpp
  ${VL}/Epub/Epub/hyphenation/LiangHyphenation.cpp
  ${VL}/Epub/Epub/converters/ImageToFramebufferDecoder.cpp
  ${VL}/Epub/Epub/converters/ImageDimsProbe.cpp
  ${VL}/Epub/Epub/converters/ImageDecoderFactory.cpp
  ${VL}/Epub/Epub/converters/JpegToFramebufferConverter.cpp
  ${VL}/Epub/Epub/converters/PngToFramebufferConverter.cpp
  ${VL}/JpegToBmpConverter/JpegToBmpConverter.cpp
  ${VL}/PngToBmpConverter/PngToBmpConverter.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/third_party/JPEGDEC/JPEGDEC.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/third_party/PNGdec/PNGdec.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/third_party/PNGdec/adler32.c
  ${CMAKE_CURRENT_SOURCE_DIR}/third_party/PNGdec/crc32.c
  ${CMAKE_CURRENT_SOURCE_DIR}/third_party/PNGdec/inffast.c
  ${CMAKE_CURRENT_SOURCE_DIR}/third_party/PNGdec/inflate.c
  ${CMAKE_CURRENT_SOURCE_DIR}/third_party/PNGdec/inftrees.c
  ${CMAKE_CURRENT_SOURCE_DIR}/third_party/PNGdec/zutil.c
  ${VL}/GfxRenderer/GfxRenderer.cpp
  ${VL}/GfxRenderer/Bitmap.cpp
  ${VL}/GfxRenderer/BitmapHelpers.cpp
  ${VL}/GfxRenderer/FontCacheManager.cpp
  ${VL}/EpdFont/EpdFont.cpp
  ${VL}/EpdFont/EpdFontFamily.cpp
  ${VL}/EpdFont/FontDecompressor.cpp
  ${VL}/EpdFont/SdFont.cpp
  ${VL}/EpdFont/SdFontFamily.cpp
  ${VL}/MiniBidi/BidiUtils.cpp
  ${VL}/MiniBidi/minibidi.c
  ${VL}/Utf8/Utf8.cpp
  ${VL}/ZipFile/ZipFile.cpp
  ${VL}/FsHelpers/FsHelpers.cpp
  ${VL}/Memory/BuildScratch.cpp
  ${VL}/InflateReader/InflateReader.cpp
  ${VL}/miniz/src/InflateStream.cpp
  ${VL}/miniz/src/miniz_impl.c
  ${VL}/uzlib/src/tinflate.c
  ${VL}/expat/xmlparse.c
  ${VL}/expat/xmlrole.c
  ${VL}/expat/xmltok.c
  ${CMAKE_CURRENT_SOURCE_DIR}/third_party/lz4/lz4.c
)

set(STUB_SOURCES
  ${STUB_DIR}/HalStorage.cpp
  ${STUB_DIR}/esp_shim.cpp
  ${STUB_DIR}/uzlib_checksums.c
)

# ---------------------------------------------------------------------------
# libjpeg-turbo, vendored so host and wasm share ONE decoder implementation.
#
# Upstream refuses add_subdirectory() outright ("cannot be integrated into another
# build system using add_subdirectory()"), so it is built as an ExternalProject and
# imported. Its own generated jconfig.h comes from its build system either way,
# which is the other reason not to hand-roll it.
#
# SIMD is disabled on purpose. The SIMD IDCT/colour paths are not guaranteed
# bit-identical to the C ISLOW path, and the host build is the byte-exact
# verification harness — divergence between toolchains would make every
# verification ambiguous. With SIMD off, both toolchains compile the same C and
# our decoder sets dct_method = JDCT_ISLOW, so decoded pixels match everywhere.
#
# TurboJPEG (the C++ wrapper), 12-bit samples and shared libraries are unused.
# ---------------------------------------------------------------------------
include(ExternalProject)

set(JPEG_TURBO_INSTALL ${CMAKE_BINARY_DIR}/libjpeg-install)
set(JPEG_TURBO_ARGS
    -DCMAKE_BUILD_TYPE=Release
    -DENABLE_SHARED=OFF
    -DENABLE_STATIC=ON
    -DWITH_SIMD=OFF
    -DWITH_TURBOJPEG=OFF
    -DWITH_12BIT=OFF
    -DWITH_JAVA=OFF
    -DCMAKE_INSTALL_PREFIX=${JPEG_TURBO_INSTALL})
if(CMAKE_TOOLCHAIN_FILE)
  list(APPEND JPEG_TURBO_ARGS -DCMAKE_TOOLCHAIN_FILE=${CMAKE_TOOLCHAIN_FILE})
endif()

ExternalProject_Add(libjpeg_turbo_ext
  SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/libjpeg-turbo
  BINARY_DIR ${CMAKE_BINARY_DIR}/libjpeg-turbo-build
  INSTALL_DIR ${JPEG_TURBO_INSTALL}
  CMAKE_ARGS ${JPEG_TURBO_ARGS}
  BUILD_BYPRODUCTS ${JPEG_TURBO_INSTALL}/lib/libjpeg.a
  UPDATE_COMMAND ""
  TEST_COMMAND ""
  LOG_BUILD ON)

add_library(jpeg-static STATIC IMPORTED GLOBAL)
set_target_properties(jpeg-static PROPERTIES
  IMPORTED_LOCATION ${JPEG_TURBO_INSTALL}/lib/libjpeg.a
  INTERFACE_INCLUDE_DIRECTORIES ${JPEG_TURBO_INSTALL}/include)
add_dependencies(jpeg-static libjpeg_turbo_ext)
# The imported archive does not exist until the ExternalProject has built, so make
# the dependency explicit for the targets that link it.
file(MAKE_DIRECTORY ${JPEG_TURBO_INSTALL}/include)

add_library(koengine STATIC ${ENGINE_SOURCES} ${STUB_SOURCES})
target_link_libraries(koengine PUBLIC jpeg-static)

if(EMSCRIPTEN)
  add_executable(ko_xtch_wasm src/wasm_api.cpp)
  target_link_libraries(ko_xtch_wasm PRIVATE koengine)
  set_target_properties(ko_xtch_wasm PROPERTIES
    LINK_FLAGS "--no-entry -sALLOW_MEMORY_GROWTH=1 -sMAXIMUM_MEMORY=2048MB \
-sEXPORTED_FUNCTIONS=_malloc,_free \
-sEXPORTED_RUNTIME_METHODS=HEAPU8,UTF8ToString,stringToUTF8 \
-sMODULARIZE=1 -sEXPORT_NAME=createKoEngine -sSTACK_SIZE=1MB -sINITIAL_MEMORY=64MB \
-O3"
  )
else()
  add_executable(ko_xtch_host src/host_main.cpp)
  target_link_libraries(ko_xtch_host PRIVATE koengine)
endif()
