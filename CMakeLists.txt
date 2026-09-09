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

add_library(koengine STATIC ${ENGINE_SOURCES} ${STUB_SOURCES})

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
