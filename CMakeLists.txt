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

# ---------------------------------------------------------------------------
# Reference ("oracle") engine build.
#
# -DKO_ENGINE_ROOT=<dir> compiles the engine from a DIFFERENT source root than
# vendor-lib, so the pinned upstream checkout can be RUN against the port on the
# same book and the two outputs diffed — a source diff only tells you the text
# differs, not whether the behaviour does.
#
# The root must have vendor-lib's layout (Epub/, GfxRenderer/, EpdFont/, ...),
# which is exactly what the reference repo's lib/ directory is. There is no
# copying and no per-file patching: the reference files are compiled as they are,
# and vendor-lib is appended to the include path only so the port's extra faces
# (ridibatang, which upstream does not ship) still resolve. Every
# reference file that exists therefore wins over its vendored copy — a
# substitution is not possible by accident.
#
# KO_ORACLE_BUILD also switches the driver off the port's text-once capture path
# (ko_engine_driver.h), which exists only in the port's GfxRenderer; the reference
# does the plain three-pass render. That is the one place the harness adapts, and
# it is a driver-side choice, not an engine one.
# ---------------------------------------------------------------------------
set(KO_ENGINE_ROOT "" CACHE PATH "engine source root to compile instead of vendor-lib (default: none)")
if(KO_ENGINE_ROOT)
  if(NOT IS_DIRECTORY "${KO_ENGINE_ROOT}")
    message(FATAL_ERROR "KO_ENGINE_ROOT is not a directory: ${KO_ENGINE_ROOT}")
  endif()
  set(VL "${KO_ENGINE_ROOT}")
  add_compile_definitions(KO_ORACLE_BUILD=1)
  message(STATUS "KO_ENGINE_ROOT set → compiling the engine from ${VL} (KO_ORACLE_BUILD=1)")
endif()

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
  ${CMAKE_CURRENT_SOURCE_DIR}/third_party/lz4
  ${CMAKE_CURRENT_SOURCE_DIR}/third_party/JPEGDEC
  ${STUB_DIR}
  ${CMAKE_CURRENT_SOURCE_DIR}/src
)

# Port-only sources and headers, always from the port even in a reference build:
# the extra Korean faces (upstream ships neither) and every ${VL} entry that the
# reference tree lacks. Appended AFTER the engine root so it can only fill gaps,
# never shadow a reference file. The oracle build prints the list so a gap is
# visible in the build log instead of being discovered as a rendering difference.
set(PORT_ONLY_DIRS
  ${CMAKE_CURRENT_SOURCE_DIR}/vendor-lib/EpdFont
  ${CMAKE_CURRENT_SOURCE_DIR}/vendor-lib/Epub
  ${CMAKE_CURRENT_SOURCE_DIR}/vendor-lib/Epub/Epub
  ${CMAKE_CURRENT_SOURCE_DIR}/vendor-lib/GfxRenderer
  ${CMAKE_CURRENT_SOURCE_DIR}/vendor-lib
)
if(KO_ENGINE_ROOT)
  include_directories(${PORT_ONLY_DIRS})
  foreach(_f builtinFonts/ridibatang_14_regular.h builtinFonts/pretendard_10_regular.h)
    if(NOT EXISTS "${VL}/EpdFont/${_f}")
      message(STATUS "  port-only header fallback: EpdFont/${_f}")
    endif()
  endforeach()
endif()

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

# Declared substitutions for a reference build — one, and it is forced.
#
# The reference's PNG path compiles against PNGdec, a bitbank2/ESP32 library that
# does not exist anywhere in this tree (the port decodes PNG with libpng), so the
# port's PngToFramebufferConverter stands in: without it nothing that touches a PNG
# links, and a conformance harness that cannot build the thing it certifies is
# worthless. Its JPEG counterparts are NOT substituted: JPEGDEC is vendored under
# third_party/JPEGDEC, so the reference's own JPEG decoders compile and run here.
#
# Consequences for the gate, stated instead of glossed:
#   * text is compared byte-for-byte (planes included);
#   * LAYOUT (page count, line breaks, word x, image rects, visible-text offsets) is compared
#     for every page on every fixture;
#   * PIXELS on pages that carry an image are exempt, for two measured reasons: the dither
#     implementation differs (upstream applyBayerDither4Level vs the port's models —
#     docs/ko-image-dither-models.md) and the JPEG decode differs (JPEGDEC vs libjpeg-turbo).
#     demo-images.epub separates them: one page has equal ink in a different pattern, another
#     has a different ink count. 0 refusals there, so device policy explains none of it.
#   * the exemption is NARROW: a page with no image that differs fails the gate, on every
#     fixture, whether or not the reference refused anything.
if(KO_ENGINE_ROOT)
  set(_port_vl ${CMAKE_CURRENT_SOURCE_DIR}/vendor-lib)
  set(_substitutions
    Epub/Epub/converters/PngToFramebufferConverter.cpp)
  foreach(_rel ${_substitutions})
    if(NOT EXISTS "${_port_vl}/${_rel}")
      message(FATAL_ERROR "declared substitution does not exist in the port: ${_rel}")
    endif()
    message(STATUS "  declared substitution (no host implementation in the reference): ${_rel}")
  endforeach()
  set(_mapped "")
  foreach(_src ${ENGINE_SOURCES})
    string(REPLACE "${VL}/" "" _rel "${_src}")
    if(_rel IN_LIST _substitutions)
      list(APPEND _mapped "${_port_vl}/${_rel}")
    else()
      list(APPEND _mapped "${_src}")
    endif()
  endforeach()
  list(LENGTH ENGINE_SOURCES _n_total)
  list(LENGTH _substitutions _n_sub)
  message(STATUS "  engine TUs: ${_n_total} (${_n_sub} substituted, ${_n_total} minus ${_n_sub} from the reference)")
  set(ENGINE_SOURCES "${_mapped}")
  # The reference's JPEG decoders need their implementation linked.
  list(APPEND ENGINE_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/third_party/JPEGDEC/JPEGDEC.cpp)
endif()

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
# KO_JPEG_WASM_SIMD: let Clang autovectorize libjpeg-turbo's GENERIC C for wasm SIMD, while leaving
# WITH_SIMD=OFF. Those are different things: WITH_SIMD=ON is libjpeg-turbo's own architecture-specific
# handwritten assembly, which has no wasm implementation in this tree (simd/ ships arm, i386, mips,
# mips64, nasm, powerpc, x86_64 — verified). -msimd128 keeps the identical generic C algorithms and only
# permits the compiler to vectorize them, so it is worth an experiment: keep it only if the container
# stays byte-identical AND the decode is measurably faster.
# Host builds are deliberately excluded: the host is the byte-exact verification harness, and giving the
# two toolchains different code generation is how verification becomes ambiguous.
option(KO_JPEG_WASM_SIMD "Autovectorize libjpeg for wasm SIMD (-msimd128, WITH_SIMD stays OFF)" OFF)

set(JPEG_TURBO_ARGS
    -DCMAKE_BUILD_TYPE=Release
    -DENABLE_SHARED=OFF
    -DENABLE_STATIC=ON
    -DWITH_SIMD=OFF
    -DWITH_TURBOJPEG=OFF
    -DWITH_12BIT=OFF
    -DWITH_JAVA=OFF
    -DCMAKE_INSTALL_PREFIX=${JPEG_TURBO_INSTALL})
# Passed EXPLICITLY in both cases. libjpeg's own build tree caches CMAKE_C_FLAGS_RELEASE, so omitting the
# argument when the option is OFF would leave a previous ON value in place — and ExternalProject does not
# re-run its configure step just because the argument list changed, so toggling this option needs the
# stamps cleared too:
#   rm -f build-wasm/libjpeg_turbo_ext-prefix/src/libjpeg_turbo_ext-stamp/libjpeg_turbo_ext-{configure,build,install,done}
# Without that, flipping KO_JPEG_WASM_SIMD and rebuilding silently keeps the previous libjpeg.
if(KO_JPEG_WASM_SIMD AND EMSCRIPTEN)
  list(APPEND JPEG_TURBO_ARGS "-DCMAKE_C_FLAGS_RELEASE=-O3 -DNDEBUG -msimd128")
  message(STATUS "libjpeg-turbo: wasm SIMD autovectorization ON (WITH_SIMD stays OFF)")
else()
  list(APPEND JPEG_TURBO_ARGS "-DCMAKE_C_FLAGS_RELEASE=-O3 -DNDEBUG")
  message(STATUS "libjpeg-turbo: wasm SIMD autovectorization OFF")
endif()
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

# §5 font-contribution measurement. Temporary measurement switches, not production architecture:
# flip one off, rebuild, and compare artifact size to see what that font costs the payload.
option(KO_EMBED_RIDI "Embed RIDIBatang (optional face; KoPub is the reference default)" ON)
option(KO_EMBED_KOPUB "Embed KoPub Batang" ON)
add_compile_definitions(
  KO_EMBED_RIDI=$<BOOL:${KO_EMBED_RIDI}>
  KO_EMBED_KOPUB=$<BOOL:${KO_EMBED_KOPUB}>
)

add_library(koengine STATIC ${ENGINE_SOURCES} ${STUB_SOURCES})
target_link_libraries(koengine PUBLIC jpeg-static)

# libpng for the PNG paths (framebuffer + covers). Unlike JPEG, where implementations
# legitimately differ in IDCT rounding, PNG decoding is exact integer arithmetic and
# inflate is deterministic - so the toolchain-native libpng produces bit-identical
# pixels and vendoring buys nothing here.
if(EMSCRIPTEN)
  target_link_options(koengine PUBLIC "-sUSE_LIBPNG=1" "-sUSE_ZLIB=1")

  # WASM-only multipliers (no device-era reason to withhold them). -msimd128 enables
  # auto-vectorisation and SIMD intrinsics for the hot loops (plane/bit expansion, dither,
  # writer packing); -flto lets the whole engine optimise across translation units. Both
  # preserve integer semantics - no -ffast-math - so output stays byte-identical.
  if(EMSCRIPTEN)
    target_compile_options(koengine PUBLIC "-msimd128" "-flto")
    target_link_options(koengine PUBLIC "-msimd128" "-flto")
  endif()
else()
  list(APPEND CMAKE_PREFIX_PATH /opt/homebrew)
  find_package(PNG REQUIRED)
  target_link_libraries(koengine PUBLIC PNG::PNG)
endif()

if(EMSCRIPTEN)
  # The build declares which faces it carries. The worker needs this BEFORE the module is ready, so
  # that a face the module does not carry can be fetched in PARALLEL with the module's own
  # fetch/compile instead of after it — which is the whole reason externalizing a face can win on
  # cold boot at all. Generating it here keeps the declaration honest: it is derived from the same
  # switches that decide what is compiled in, not maintained by hand next to them.
  set(_embedded_faces "")
  if(KO_EMBED_KOPUB)
    list(APPEND _embedded_faces "kopub")
  endif()
  if(KO_EMBED_RIDI)
    list(APPEND _embedded_faces "ridibatang")
  endif()
  # An empty list must emit [] — the first version interpolated the empty string into a one-element
  # array, so a build with no embedded face reported embedded: [""], which reads as "a face named
  # empty string is compiled in" and is exactly the kind of thing a report should never say.
  if(_embedded_faces)
    string(REPLACE ";" "\",\"" _embedded_faces_js "${_embedded_faces}")
    set(_embedded_faces_json "[\"${_embedded_faces_js}\"]")
  else()
    set(_embedded_faces_json "[]")
  endif()
  file(WRITE "${CMAKE_BINARY_DIR}/ko_build_info.js"
       "// generated by CMake — which built-in faces this module carries, for the worker to read\n"
       "self.KO_EMBEDDED_FACES = ${_embedded_faces_json};\n")
  # The same fact in a machine-readable form, so packaging does not have to parse JavaScript or
  # duplicate CMake's knowledge: CMake says what the engine contains, and everything downstream
  # derives from that one authority.
  file(WRITE "${CMAKE_BINARY_DIR}/ko_build_info.json"
       "{\n  \"embedded\": ${_embedded_faces_json}\n}\n")

  add_executable(ko_xtch_wasm src/wasm_api.cpp)
  add_custom_command(TARGET ko_xtch_wasm POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy "${CMAKE_BINARY_DIR}/ko_build_info.js"
            "$<TARGET_FILE_DIR:ko_xtch_wasm>/ko_build_info.js"
    COMMENT "staging ko_build_info.js (embedded faces) next to the module")
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
  # The gray-plane negative control (Driver::renderPageDroppingGrayForTest) is compiled ONLY here. It
  # exists so mono_planes_gate.py can prove the writer rejects incomplete 1-bit input; the shipped module
  # must not be able to select that state, so the macro is deliberately not global.
  target_compile_definitions(ko_xtch_host PRIVATE KO_TEST_NEGATIVE_CONTROLS)

  # Verification tools as real targets. They used to be built by hand from a clang++ line in a
  # comment, which meant they could sit in build/ for hours while their sources changed — and did:
  # a stale verify_external_font reported "IDENTICAL" for a RIDIBatang blob it was silently
  # comparing against KoPub, because it predated the face argument. A gate that can be an old
  # binary is not a gate, so nothing here is hand-compiled any more.
  foreach(tool export_external_font verify_external_font)
    add_executable(${tool} tools/${tool}.cpp)
    target_link_libraries(${tool} PRIVATE koengine)
  endforeach()
  if(NOT KO_ENGINE_ROOT)
    enable_testing()
    add_executable(cache_integrity_test tests/cache_integrity_test.cpp)
    target_link_libraries(cache_integrity_test PRIVATE koengine)
    add_test(NAME cache_integrity COMMAND cache_integrity_test)
  endif()
endif()
