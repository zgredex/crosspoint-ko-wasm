// wasm_api.cpp — CrossPoint-KO engine as a WASM module.
//
// Exposes the full Korean-typography knob surface so callers can experiment:
//   - ReaderRenderSpec knobs: lineCompression, paragraph indent, character
//     wrap, alignment, extra paragraph spacing, hyphenation, embedded style,
//     image rendering policy, focus reading
//   - margins + viewport
//   - page-by-page plane capture (BW + LSB/MSB gray = the three passes the
//     device display pipeline uses; XTH value semantics documented)
//   - one-shot "convert whole book" → in-memory XTCH container bytes
//
// Memory model: module-lifetime HalDisplay + GfxRenderer + EngineDriver. EPUB
// injected via ko_load_epub(bytes,len). Planes are 48000 bytes (physical
// 800x480); JS reads them from HEAPU8. The encoder in JS (or ko_render_xtch)
// transposes to logical 480x800 + XTH packing.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include "converters/ImagePerf.h"
#include <cstring>
#include <algorithm>
#include <memory>
#include <string>
#include <vector>

#ifdef __EMSCRIPTEN__
#include <emscripten/emscripten.h>
#define KO_EXPORT EMSCRIPTEN_KEEPALIVE
#else
#define KO_EXPORT
#endif
#include <SdFontFamily.h>
#include "ko_engine_driver.h"
#include "external_font_loader.h"   // §6: lossless EPD2 built-in font blobs
#include "xtch_writer.h"
#include "xtch_chapters.h"

// Global instances (module-lifetime)
static HalDisplay* g_display = nullptr;
static GfxRenderer* g_renderer = nullptr;
static ko::EngineDriver* g_driver = nullptr;
// §6 of the KoPub groundwork: an externally loaded built-in font. The bundle owns the arrays, the
// EpdFont/EpdFontFamily own nothing and point into the bundle's EpdFontData, so all three must outlive
// any renderer reference and are torn down in reverse order in ko_xtch_release().
// An externalized built-in face: the parsed bundle plus the wrappers built on its storage. Kept
// PER FONT ID because the renderer's registry is keyed by id — a RIDIBatang blob has to replace
// RIDIBatang, and unloading one face must not disturb the other. The single-slot version of this
// registered every blob under KOPUB_14_FONT_ID, which was invisible only because KoPub was the only
// face that could be externalized at the time.
struct ExternalFace {
  int fontId = 0;
  std::unique_ptr<ko::ExternalBuiltinFont> bundle;
  std::unique_ptr<EpdFont> font;
  std::unique_ptr<EpdFontFamily> family;
};

// One slot per id the EPD2 loader accepts (see the switch in ko_load_external_builtin_font).
static ExternalFace g_externalFaces[2];

static ExternalFace* externalFaceFor(int fontId) {
  for (auto& face : g_externalFaces) {
    if (face.fontId == fontId) return &face;
  }
  for (auto& face : g_externalFaces) {
    if (face.fontId == 0) {
      face.fontId = fontId;
      return &face;
    }
  }
  return nullptr;
}

static EpdFont* g_kopub = nullptr;
static EpdFontFamily* g_kopubFamily = nullptr;
static EpdFont* g_ridibatang = nullptr;
static EpdFontFamily* g_ridibatangFamily = nullptr;
static ko::Spec g_spec;
static ko::RenderedPage g_page;
static std::vector<uint8_t> g_error;

// XTCH accumulation (used by ko_render_xtch one-shot)
static ko::XtchWriter* g_xtch = nullptr;
static std::vector<ko::XtchChapter> g_chapters;
static std::vector<ko::XtchChapter> g_spineFallback;   // per-spine names when no TOC
static std::vector<ko::ChapterCandidate> g_chapterCandidates;  // TOC entries during export
static int g_currentSpine = -1;
static int g_spinePageStart = 0;
static int g_totalPages = 0;
static int g_xtchFullReady = 0;
static std::vector<uint8_t> g_xtchOut;      // finished container bytes (ko_xtch_ptr)
static std::vector<uint8_t> g_coverData;   // cover BMP bytes (ko_generate_cover)
static std::string g_coverPath;
static size_t g_coverSize = 0;

static void setError(const std::string& msg) {
  g_error.assign(msg.begin(), msg.end());
  g_error.push_back('\0');
}

extern "C" {

// forward decl (ko_init calls it before its definition)
KO_EXPORT void ko_set_margins(int top, int right, int bottom, int left);

// ---- lifecycle -------------------------------------------------------------

KO_EXPORT const char* ko_version() { return "crosspoint-ko 1.5.0-ko.3 wasm 0.1"; }

KO_EXPORT int ko_init(int viewportWidth, int viewportHeight) {
  if (!g_display) {
    g_display = new HalDisplay();
    g_renderer = new GfxRenderer(*g_display);
    g_renderer->begin();
    g_driver = new ko::EngineDriver(*g_renderer, *g_display);

#if KO_EMBED_KOPUB
    g_kopub = new EpdFont(&kopub_14_regular);
    g_kopubFamily = new EpdFontFamily(g_kopub);
#endif
#if KO_EMBED_RIDI
    g_ridibatang = new EpdFont(&ridibatang_14_regular);
    g_ridibatangFamily = new EpdFontFamily(g_ridibatang);
#endif
    // Registration must be guarded exactly like construction. It was not: with KO_EMBED_KOPUB=OFF the
    // family pointer was never assigned, yet the (unconditional) insertFont registered a null family —
    // so every measurement build except the baseline registered nulls for the disabled faces.
#if KO_EMBED_KOPUB
    g_renderer->insertFont(KOPUB_14_FONT_ID, g_kopubFamily);
#endif
#if KO_EMBED_RIDI
    g_renderer->insertFont(RIDIBATANG_14_FONT_ID, g_ridibatangFamily);
#endif
    g_xtch = new ko::XtchWriter();
  }
  g_spec = ko::Spec();
  // The width/height arguments are ADVISORY and were, until now, silently discarded: the geometry
  // is derived from the spec's margins (viewport = screen − margins), so ko_init(464, 778) still
  // produced a 464x764 layout. Five verification scripts were initialised with the old 778 and
  // printed nothing about it, which made their own headers wrong about the geometry they measured.
  // Saying it out loud costs one line and removes that whole class of quiet disagreement.
  ko_set_margins(g_spec.marginTop, g_spec.marginRight, g_spec.marginBottom, g_spec.marginLeft);
  if (viewportWidth != g_spec.viewportWidth || viewportHeight != g_spec.viewportHeight) {
    std::fprintf(stderr,
                 "[ko] ko_init(%d,%d) ignored: geometry follows the margins, giving %ux%u "
                 "(margins %d/%d/%d/%d). Pass the derived viewport to silence this.\n",
                 viewportWidth, viewportHeight, g_spec.viewportWidth, g_spec.viewportHeight,
                 g_spec.marginTop, g_spec.marginRight, g_spec.marginBottom, g_spec.marginLeft);
  }
  return 0;
}

KO_EXPORT void ko_close() {
  delete g_driver; g_driver = nullptr;
  delete g_renderer; g_renderer = nullptr;
  delete g_display; g_display = nullptr;
#if KO_EMBED_KOPUB
  delete g_kopubFamily; g_kopubFamily = nullptr;
  delete g_kopub; g_kopub = nullptr;
#endif
#if KO_EMBED_RIDI
  delete g_ridibatangFamily; g_ridibatangFamily = nullptr;
  delete g_ridibatang; g_ridibatang = nullptr;
#endif
  // Reverse construction order per slot: family, then font, then the bundle they point into.
  for (auto& face : g_externalFaces) {
    face.family.reset();
    face.font.reset();
    face.bundle.reset();
    face.fontId = 0;
  }
  delete g_xtch; g_xtch = nullptr;
}

KO_EXPORT const char* ko_error() {
  return g_error.empty() ? "" : reinterpret_cast<const char*>(g_error.data());
}

// ---- Korean typography knobs ------------------------------------------------

KO_EXPORT void ko_set_line_compression(float v) { g_spec.lineCompression = v; }
KO_EXPORT void ko_set_extra_paragraph_spacing(int v) { g_spec.extraParagraphSpacing = v; }
KO_EXPORT void ko_set_paragraph_indent(int v) { g_spec.paragraphIndent = v; }
KO_EXPORT void ko_set_character_wrap(int v) { g_spec.characterWrap = v; }
// alignment: 0 JUSTIFIED 1 LEFT 2 CENTER 3 RIGHT 4 BOOK_STYLE
KO_EXPORT void ko_set_paragraph_alignment(int v) { g_spec.paragraphAlignment = v; }
KO_EXPORT void ko_set_hyphenation(int v) { g_spec.hyphenationEnabled = v; }
KO_EXPORT void ko_set_embedded_style(int v) { g_spec.embeddedStyle = v; }
KO_EXPORT void ko_set_image_rendering(int v) { g_spec.imageRendering = v; }
// Text anti-aliasing (device Text AA toggle). Off = text renders 1-bit in the
// BW pass only; images still get their grayscale passes.
KO_EXPORT void ko_set_text_aa(int v) { g_spec.textAntiAliasing = v ? 1 : 0; }
// Image dither model (ko::DitherMode code) and the tone depth the export needs
// (4 for a 2-bit page, 2 for a 1-bit page). Re-render after changing either.
KO_EXPORT void ko_set_image_dither(int v) { g_spec.imageDither = v; }
KO_EXPORT void ko_set_image_tone_depth(int v) { g_spec.imageToneDepth = (v == 2) ? 2 : 4; }
KO_EXPORT void ko_set_focus_reading(int v) { (void)v; g_spec.focusReadingEnabled = 0; }  // EN-only; hardcoded off in KO

// Reader font: KOPUB_14_FONT_ID (reference default) / RIDIBATANG_14_FONT_ID (XTCKO
// extra) / CUSTOM_FONT_ID.
// §2/§3 of the KoPub-externalization groundwork. ko_set_font() accepted KOPUB_14_FONT_ID by id alone,
// so with the face compiled out (or before a lazy fetch has landed) the JS believed the layout was
// KoPub while the renderer drew whatever it actually had. A face that is not registered must fail
// visibly instead.
// Load a lossless external built-in font (EPD2) and register it under `fontId`.
// Returns 0 on success, -1 for a bad request, -2 for a blob that failed to parse (the reason is in the
// error string). Nothing is partially registered on failure: parse first, insert second.
KO_EXPORT int ko_load_external_builtin_font(int fontId, uintptr_t ptr, size_t len) {
  if (!g_renderer) {
    setError("engine not initialised");
    return -1;
  }
  // The EPD2 container is generic; this list is the set of BUILT-IN faces a blob may stand in for.
  // It is deliberately a positive list: the format carries its own metrics, but the engine still has
  // to know which family to replace, and a typo'd or unsupported id must fail loudly rather than
  // register a face under a name nothing will ever ask for. RIDIBatang is here because it is the
  // optional face a default KoPub build does not need to carry — see docs/ko-font-payload-
  // measurement.md for the sizes that decide whether it ships embedded or fetched.
  switch (fontId) {
    case KOPUB_14_FONT_ID:
    case RIDIBATANG_14_FONT_ID:
      break;
    default:
      setError("unsupported external built-in font");
      return -1;
  }

  ExternalFace* slot = externalFaceFor(fontId);
  if (slot == nullptr) {
    setError("no free external font slot");
    return -1;
  }

  std::unique_ptr<ko::ExternalBuiltinFont> parsed;
  std::string err;
  if (!ko::parseExternalFont(reinterpret_cast<const uint8_t*>(ptr), len, parsed, err)) {
    setError(err.c_str());
    return -2;
  }

  auto font = std::make_unique<EpdFont>(&parsed->data);
  auto family = std::make_unique<EpdFontFamily>(font.get());

  // Insert the new family BEFORE releasing anything: the renderer must never hold a destroyed family,
  // and a re-load (a second book, a re-fetch) must replace the previous bundle without leaking it.
  g_renderer->insertFont(fontId, family.get());
  slot->family = std::move(family);
  slot->font = std::move(font);
  slot->bundle = std::move(parsed);

  // A build that still embeds this face now has both; the external one is registered, so drop the
  // embedded wrapper rather than keeping two copies of the font alive. This is what makes the switch
  // to KO_EMBED_<FACE>=OFF a size change only, with identical rendering either way. Only the face
  // that was just replaced is dropped — the other embedded face is untouched.
  if (fontId == KOPUB_14_FONT_ID) {
    delete g_kopub;
    g_kopub = nullptr;
    delete g_kopubFamily;
    g_kopubFamily = nullptr;
  } else if (fontId == RIDIBATANG_14_FONT_ID) {
    delete g_ridibatang;
    g_ridibatang = nullptr;
    delete g_ridibatangFamily;
    g_ridibatangFamily = nullptr;
  }
  return 0;
}

KO_EXPORT int ko_has_font(int fontId) {
  return (g_renderer && g_renderer->hasFont(fontId)) ? 1 : 0;
}

KO_EXPORT int ko_set_font(int fontId) {
  switch (fontId) {
    case RIDIBATANG_14_FONT_ID:
    case KOPUB_14_FONT_ID:
    case CUSTOM_FONT_ID:
      if (!g_renderer || !g_renderer->hasFont(fontId)) {
        setError("font not loaded");
        return -1;
      }
      g_spec.fontId = fontId;
      return 0;
    default:
      setError("unknown font id");
      return -1;
  }
}

KO_EXPORT int ko_font() { return g_spec.fontId; }

// Runtime custom reader font (.epdfont), mirroring the device's SD-font path:
// bytes are mounted into the in-memory FS, loaded through SdFontFamily and
// registered under CUSTOM_FONT_ID. After a book load (which clears the FS),
// the caller must re-send the font (the web worker caches and re-applies it).
KO_EXPORT int ko_load_epdfont(const uint8_t* data, size_t size, const char* name) {
  if (!g_renderer || !data || size < 64) {
    setError("bad epdfont payload");
    return -1;
  }
  // remove any previous custom font first (mirrors reloadCustomReaderFont)
  if (g_renderer->hasFont(CUSTOM_FONT_ID)) {
    g_renderer->removeFont(CUSTOM_FONT_ID);
  }
  const std::string path = std::string("/.fonts/") + (name && name[0] ? name : "custom") + ".epdfont";
  Storage.mountBlob(path, data, size);
  auto family = new SdFontFamily(path.c_str());
  if (!family || !family->load()) {
    delete family;
    Storage.remove(path.c_str());
    setError("epdfont load failed (bad format?)");
    return -1;
  }
  g_renderer->insertSdFont(CUSTOM_FONT_ID, family);  // takes ownership
  g_spec.fontId = CUSTOM_FONT_ID;
  return 0;
}

KO_EXPORT int ko_clear_custom_font() {
  if (g_renderer && g_renderer->hasFont(CUSTOM_FONT_ID)) {
    g_renderer->removeFont(CUSTOM_FONT_ID);
  }
  if (g_spec.fontId == CUSTOM_FONT_ID) {
    // Back to the reference default face, not to a port-specific one.
    g_spec.fontId = KOPUB_14_FONT_ID;
  }
  return 0;
}

// Active reader font metrics: returns baseline advance (lineHeight) for the
// current font at the default (uncompressed) multiplier — the KO doc's
// advanceY (KoPub Batang 14 → 32 px, RIDIBatang 14 → 38 px).
KO_EXPORT int ko_font_advance_y() {
  if (!g_renderer) return 0;
  return g_renderer->getLineHeight(g_spec.fontId);
}

KO_EXPORT void ko_set_margins(int top, int right, int bottom, int left) {
  g_spec.marginTop = top;
  g_spec.marginRight = right;
  g_spec.marginBottom = bottom;
  g_spec.marginLeft = left;
  if (g_renderer) {
    g_spec.viewportWidth = static_cast<uint16_t>(
        g_renderer->getScreenWidth() - g_spec.marginLeft - g_spec.marginRight);
    g_spec.viewportHeight = static_cast<uint16_t>(
        g_renderer->getScreenHeight() - g_spec.marginTop - g_spec.marginBottom);
  }
}

KO_EXPORT int ko_viewport_width() { return g_spec.viewportWidth; }
KO_EXPORT int ko_viewport_height() { return g_spec.viewportHeight; }
KO_EXPORT int ko_logical_width() {
  return g_renderer ? g_renderer->getScreenWidth() : 0;
}
KO_EXPORT int ko_logical_height() {
  return g_renderer ? g_renderer->getScreenHeight() : 0;
}

// ---- EPUB loading -----------------------------------------------------------

// Shared tail of both load entry points: reset everything that belongs to a previous book. Kept in one
// place so the copying and adopting paths cannot drift in what they leave behind.
static int resetForNewBook() {
  g_xtch->reset();
  g_xtch->setMetadata(g_driver->title(), "unknown", "", "ko");
  g_chapters.clear();
  g_currentSpine = -1;
  g_spinePageStart = 0;
  g_totalPages = 0;
  g_xtchFullReady = 0;
  return g_driver->spineCount();
}

KO_EXPORT int ko_load_epub(const uint8_t* data, size_t size, const char* virtualPath) {
  if (!g_driver || !data || size == 0) return -1;
  // Fresh in-memory FS per book: section caches from a previous load otherwise
  // accumulate in the wasm heap and eventually fail section builds.
  Storage.clearAll();
  const std::string vp = virtualPath && virtualPath[0] ? virtualPath : "/book.epub";
  if (!g_driver->loadEpubFromBlob(data, size, vp)) {
    setError("Epub::load failed");
    return -1;
  }
  return resetForNewBook();
}

// Allocate the buffer the caller will stream an EPUB into. It is NOT a general-purpose allocator: pair
// it with ko_load_epub_owned, which adopts the pointer, and only free it yourself if you never call
// that (or if the call never happened).
KO_EXPORT uint8_t* ko_epub_alloc(size_t size) {
  if (size == 0) return nullptr;
  return static_cast<uint8_t*>(std::malloc(size));
}

// Adopt an EPUB that already lives in this heap, instead of copying it in: the bytes the ZIP reader
// walks are the bytes the page wrote. OWNERSHIP TRANSFERS — the storage frees the pointer when the
// book is replaced, so the caller must not. A 100 MB book used to be copied twice on the way in (JS
// ArrayBuffer -> wasm, then wasm -> storage vector).
KO_EXPORT int ko_load_epub_owned(uint8_t* data, size_t size, const char* virtualPath) {
  // The pointer is consumed by this call whenever it is usable at all, whatever the outcome: a caller
  // must never read or free it afterwards. The only case where nothing is adopted is a null/empty
  // buffer, and then there is nothing to leak either.
  if (!data || size == 0) return -1;
  if (!g_driver) {
    std::free(data);          // consumed, just not by the storage
    setError("no engine");
    return -1;
  }
  Storage.clearAll();
  const std::string vp = virtualPath && virtualPath[0] ? virtualPath : "/book.epub";
  if (!g_driver->loadEpubFromOwnedBlob(data, size, vp)) {
    setError("Epub::load failed");
    return -1;      // the driver already dropped the mounted blob, which freed the buffer
  }
  return resetForNewBook();
}

KO_EXPORT int ko_spine_count() { return g_driver ? g_driver->spineCount() : 0; }

// The spine the REFERENCE reader opens at: EpubReaderActivity::onEnter, when opening a book for the
// first time (currentSpineIndex == 0), navigates to Epub::getSpineIndexForTextReference() if it is not 0
// — i.e. it skips a cover/author page that the EPUB explicitly designates as such. The browser preview
// ignored this and always started at 0, so the port's first page and the device's first page could be
// different spines. Export still contains every spine; only the initial position changes.
KO_EXPORT int ko_text_reference_spine() {
  return g_driver ? g_driver->textReferenceSpine() : 0;
}

KO_EXPORT void ko_get_title(char* buf, int bufLen) {
  if (!buf || bufLen <= 0) return;
  const std::string& t = g_driver->title();
  int n = static_cast<int>(t.size()) < bufLen - 1 ? static_cast<int>(t.size()) : bufLen - 1;
  memcpy(buf, t.data(), n);
  buf[n] = '\0';
}

// Spine href (basename) for the chapter picker; pointer valid until next call.
KO_EXPORT const char* ko_get_spine_href(int spineIndex) {
  if (!g_driver || spineIndex < 0 || spineIndex >= g_driver->spineCount()) return "";
  // By value: spineHref() copies out of the metadata cache, so there is no temporary to outlive here.
  // `last` is the buffer the returned pointer refers to, so the pointer is valid until the next call.
  //
  // Assign DIRECTLY — no `const auto& item = spineHref(...)` in between. That indirection is legal only
  // while the accessor returns by value: the reference binds to a temporary whose lifetime gets extended.
  // It is also exactly how this function read a dead stack frame before, so if the accessor ever goes
  // back to returning a reference, the indirection silently reintroduces the UB instead of failing to
  // compile. `scripts/verify/spine_href_gate.py` asserts this line stays in the direct form.
  static std::string last;
  last = g_driver->spineHref(spineIndex);
  // basename only
  size_t slash = last.find_last_of('/');
  if (slash != std::string::npos) last = last.substr(slash + 1);
  return last.c_str();
}

// Cover/thumbnail generation — mirrors the device library path. Generates the
// prescaled cover BMP via JpegToBmpConverter (handles oversized covers that the
// in-page decoder's RAM cap refuses). Returns bytes via ko_cover_ptr/size.
// kind: 0 = cropped cover (540x800), 1 = thumb (device-height)
KO_EXPORT int ko_generate_cover(int kind) {
  if (!g_driver) return -1;
  auto epub = g_driver->epubShared();
  bool ok;
  if (kind == 0) {
    ok = epub->generateCoverBmp(true);          // cropped
    g_coverPath = epub->getCoverBmpPath(true);
  } else {
    ok = epub->generateThumbBmp(220);           // device thumb height
    g_coverPath = epub->getThumbBmpPath(220);
  }
  if (!ok) {
    setError("cover generation failed");
    return -1;
  }
  HalFile f;
  if (!Storage.openFileForRead("CVR", g_coverPath, f)) {
    setError("cover bmp missing");
    return -1;
  }
  g_coverSize = f.size();
  g_coverData.resize(g_coverSize);
  if (f.read(g_coverData.data(), g_coverSize) != static_cast<int>(g_coverSize)) {
    f.close();
    setError("cover read failed");
    return -1;
  }
  f.close();
  return 0;
}

KO_EXPORT const uint8_t* ko_cover_ptr() { return g_coverData.empty() ? nullptr : g_coverData.data(); }
KO_EXPORT size_t ko_cover_size() { return g_coverSize; }

// ---- pagination + page capture ----------------------------------------------

// Build spine (0-based). Returns page count, or -1 on failure.
KO_EXPORT int ko_build_spine(int spineIndex) {
  if (!g_driver) return -1;
  const int n = g_driver->buildSection(spineIndex, g_spec);
  if (n < 0) {
    setError("section build failed");
    return -1;
  }
  g_currentSpine = spineIndex;
  return n;
}

// Render page p of current spine (all 3 passes). 0 ok / -1 error.
// Then ko_plane_ptr(kind): 0=BW 1=LSB(dark grey) 2=MSB(light+dark).
// ---- progressive section build (open-path latency) ---------------------------
// Lay out enough of a spine for its first page, then extend in chunks. Without this the browser
// paginates an entire chapter before it may show page 1.
KO_EXPORT int ko_start_spine(int spine, int initialPages) {
  if (!g_driver) return -1;
  const int n = g_driver->startSection(spine, g_spec, initialPages < 1 ? 1 : initialPages);
  if (n < 0) {
    setError("progressive section start failed");
    return -1;
  }
  // Same contract as ko_build_spine: a successful section start makes this the current spine, which is
  // what ko_render_page checks before rendering. Without this the render is refused with
  // "page render failed" and nothing says why.
  g_currentSpine = spine;
  return n;
}

KO_EXPORT int ko_build_spine_more(int maxPages) {
  if (!g_driver) return -1;
  return g_driver->buildSectionMore(maxPages);
}

KO_EXPORT int ko_spine_build_complete() {
  return (g_driver && g_driver->sectionBuildComplete()) ? 1 : 0;
}

KO_EXPORT int ko_spine_pages_available() {
  return g_driver ? g_driver->availablePages() : 0;
}

KO_EXPORT int ko_spine_pages_estimated() {
  return g_driver ? g_driver->estimatedPages() : 0;
}

// Per-render image accounting: how many times an image was decoded for this one page, and where the
// image time went. Reset here so the numbers always describe the LAST render — which is how "the cover
// is decoded three times for one preview page" becomes a measurement instead of a claim.
KO_EXPORT void ko_image_perf_reset() { ko::imagePerfReset(); }
KO_EXPORT int ko_image_decodes() { return static_cast<int>(ko::imagePerf().decodes); }

// Gate switch for the browser half of the image-once comparison: render three-pass instead of
// image-once, so the same module can be measured both ways. Not a product setting.
KO_EXPORT void ko_set_three_pass(int on) {
  if (g_driver) g_driver->setThreePass(on != 0);
}
KO_EXPORT int ko_image_count() { return static_cast<int>(ko::imagePerf().images); }
KO_EXPORT double ko_image_perf(int which) {
  const ko::ImagePerf& p = ko::imagePerf();
  switch (which) {
    case 0: return p.readMs;
    case 1: return p.headerMs;
    case 2: return p.decodeMs;
    case 3: return p.drawMs;
    default: return 0.0;
  }
}

// 1-bit consumers (XTC preview, XTC export) only read the BW plane; monoOnly skips the gray passes.
KO_EXPORT int ko_render_page_mode(int pageIndex, int monoOnly) {
  if (!g_driver || g_currentSpine < 0) return -1;
  return g_driver->renderPage(pageIndex, g_spec, g_page, nullptr, g_currentSpine, monoOnly != 0) ? 0 : -1;
}

KO_EXPORT int ko_render_page(int pageIndex) {
  if (!g_driver || g_currentSpine < 0) return -1;
  ko::imagePerfReset();
  if (!g_driver->renderPage(pageIndex, g_spec, g_page)) {
    setError("page render failed");
    return -1;
  }
  return 0;
}

KO_EXPORT const uint8_t* ko_plane_ptr(int kind) {
  switch (kind) {
    case 0: return g_page.bw.data();
    case 1: return g_page.lsb.data();
    case 2: return g_page.msb.data();
    default: return nullptr;
  }
}
KO_EXPORT size_t ko_plane_size(int kind) {
  (void)kind;
  return 48000;
}

// ---- XTCH container (whole-book convert) -----------------------------------
//
// One book = one XTCH file (the site's core promise: the exported .xtc/.xtch
// plays back on the device exactly like the preview). Export is incremental:
// the JS worker calls ko_export_spine() per spine (reports progress, stays well
// under the call timeout), then ko_export_finish() finalizes the container.
// Bytes are read via ko_xtch_ptr()/ko_xtch_size().

// Select container mode before export: 0 = 1-bit XTC ("XTC\0", XTG pages),
// 1 = 2-bit XTCH ("XTCH", XTH pages). Mirrors the device file magics.
// Text-AA switch. Images are dithered to 2 levels in EVERY mode (grey pixels are
// always halftoned by the mono writer); this switch only decides whether the writer
// additionally thins SOLID ink, i.e. whether text gets the AA-on halftone look or
// crisp 1-bit pixels. The same switch drives the mono preview, so preview and file
// stay in agreement by construction.
static bool textAaEnabled() { return g_spec.textAntiAliasing != 0; }

KO_EXPORT void ko_export_set_mode(int mode) {
  if (!g_xtch) return;
  g_xtch->setMode(mode == 0 ? ko::XtcMode::Mono1Bit : ko::XtcMode::Gray2Bit);
  g_xtch->setTextAa(textAaEnabled());
}

KO_EXPORT int ko_export_begin() {
  if (!g_driver || !g_xtch) return -1;
  g_xtch->reset();
  g_xtch->setMetadata(g_driver->title(), "unknown", "", "ko");
  g_chapters.clear();
  g_spineFallback.clear();
  g_chapterCandidates.clear();
  g_totalPages = 0;
  g_spinePageStart = 0;
  g_xtchFullReady = 0;
  g_xtchOut.clear();
  return g_driver->spineCount();
}

// Render one spine's pages into the container. Returns the number of pages
// ACTUALLY added (>=0), or -1 on build failure — the JS side maps spine→page
// ranges from this return, so it must reflect real page count.
KO_EXPORT int ko_export_spine(int spine) {
  if (!g_driver || !g_xtch) return -1;
  // Re-assert the switch here as well: it can be flipped between
  // ko_export_set_mode() and the page loop, and this is the only place that is
  // guaranteed to run for every exported page.
  g_xtch->setTextAa(textAaEnabled());
  const int n = g_driver->buildSection(spine, g_spec);
  if (n < 0) return -1;
  const int before = g_totalPages;
  // Same rule as ko_encode_spine: a 1-bit container never stores the gray planes, so do not render them.
  const bool monoSerial = (g_xtch->mode() == ko::XtcMode::Mono1Bit);
  for (int p = 0; p < n; p++) {
    ko::RenderedPage rp;
    if (!g_driver->renderPage(p, g_spec, rp, nullptr, 0, monoSerial)) continue;
    g_xtch->addPageFromPlanes(rp.bw, rp.lsb, rp.msb);
    g_totalPages++;
  }
  const int added = g_totalPages - before;
  if (added > 0) {
    const int spineStart = g_spinePageStart;
    // fallback chapter name (used only when the book has no usable TOC)
    {
      std::string href = g_driver->spineHref(spine);
      size_t slash = href.find_last_of('/');
      if (slash != std::string::npos) href = href.substr(slash + 1);
      size_t dot = href.find_last_of('.');
      if (dot != std::string::npos) href = href.substr(0, dot);
      ko::XtchChapter ch;
      ch.name = href.empty() ? ("Chapter " + std::to_string(spine + 1)) : href;
      ch.startPage = static_cast<uint16_t>(spineStart);
      ch.endPage = static_cast<uint16_t>(spineStart + added - 1);
      g_spineFallback.push_back(ch);
    }
    // TOC candidates for this spine: each nav entry resolving into this spine
    // becomes a chapter whose start page = spine start + anchor local page
    // (anchors are recorded during the layout we just ran).
    const int tocN = g_driver->tocCount();
    for (int t = 0; t < tocN; t++) {
      if (g_driver->tocSpine(t) != spine) continue;
      const std::string title = g_driver->tocTitle(t);
      const std::string anchor = g_driver->tocAnchor(t);
      int local = anchor.empty() ? -1 : g_driver->anchorLocalPage(anchor);
      int page = (local >= 0 && local < added) ? spineStart + local : spineStart;
      g_chapterCandidates.push_back({title, static_cast<uint32_t>(page)});
    }
    g_spinePageStart += added;
  }
  return added;
}

// Finalize the container into g_xtchOut. Returns total pages or -1.
// Chapters are assembled from the EPUB TOC (device-true chapter list, official
// converter caps at 100 entries, end page = next chapter start − 1). Books
// without a usable TOC fall back to per-spine chapters.

KO_EXPORT int ko_export_finish() {
  if (!g_xtch) return -1;
  g_chapters = buildChapters(g_chapterCandidates, g_spineFallback,
                             static_cast<uint32_t>(g_totalPages));
  g_xtchOut = g_xtch->finish(g_chapters);
  g_xtchFullReady = 1;
  return g_totalPages;
}


// ---- spine pool: per-spine encoded results ----------------------------------
//
// A spine can be laid out, rendered and ENCODED on its own, and its page records appended to the
// container later in spine order by a single assembler. Nothing here re-implements page encoding:
// each spine uses the same ko::XtchWriter the serial path uses, and the assembler appends those
// records verbatim. Rendering is ~85% of export time and it is per-spine work, so this is the unit
// worth parallelising; the container is not, which is why assembly stays centralised.
namespace {
struct SpineTocEntry {
  int tocIndex = 0;
  int localPage = 0;
  std::string title;
};
struct EncodedSpine {
  // The records live in `flat` (what crosses to JS in one transfer) with `offsets`/`lengths` indexing
  // it. There is deliberately no per-page vector here: an earlier version had one that the encode path
  // never filled, which made count() — and so ko_spine_page_count() — report 0 while the real count sat
  // in lengths. Deriving the count from lengths makes the accessor truthful by construction.
  std::vector<uint8_t> flat;
  std::vector<uint32_t> offsets, lengths;
  std::vector<SpineTocEntry> toc;            // anchors resolved while the section was still built
  std::string fallbackName;
  int count() const { return static_cast<int>(lengths.size()); }
  void reset() { flat.clear(); offsets.clear(); lengths.clear(); toc.clear(); fallbackName.clear(); }
};
}  // namespace

static EncodedSpine g_enc;
static std::unique_ptr<ko::XtchWriter> g_asm;          // the assembler's writer
static std::vector<ko::ChapterCandidate> g_asmCandidates;
static std::vector<ko::XtchChapter> g_asmFallback;

// Layout+render+encode one spine into g_enc. Returns the page count, or -1.
KO_EXPORT int ko_encode_spine(int spine) {
  if (!g_driver || !g_xtch) return -1;
  g_enc.reset();
  // A local writer: a worker's spine must not touch any shared writer state.
  ko::XtchWriter w(g_xtch->mode());
  w.setTextAa(textAaEnabled());
  const int n = g_driver->buildSection(spine, g_spec);
  if (n < 0) return -1;
  int rendered = 0;
  for (int p = 0; p < n; p++) {
    ko::RenderedPage rp;
    // A 1-bit container stores no gray planes, so rendering them is dead work — and on an AA-off image
    // page each gray pass is another full image decode. Only ever mono when the writer is Mono1Bit.
    const bool mono = (w.mode() == ko::XtcMode::Mono1Bit);
    if (!g_driver->renderPage(p, g_spec, rp, nullptr, 0, mono)) continue;
    w.addPageFromPlanes(rp.bw, rp.lsb, rp.msb);
    rendered++;
  }
  const int pages = static_cast<int>(w.pageCount());
  if (pages != rendered) {
    // The serial path counts a page for every successful render and trusts the writer to have made
    // the same number of records. If those disagree the container would already be malformed, so
    // fail loudly here rather than assemble a file whose index disagrees with its records.
    return -1;
  }
  // Move the records into g_enc and build the transferable layout.
  size_t total = 0;
  for (size_t i = 0; i < static_cast<size_t>(pages); i++) total += w.page(i).size();
  g_enc.flat.reserve(total);
  for (size_t i = 0; i < static_cast<size_t>(pages); i++) {
    const std::vector<uint8_t>& rec = w.page(i);
    g_enc.offsets.push_back(static_cast<uint32_t>(g_enc.flat.size()));
    g_enc.lengths.push_back(static_cast<uint32_t>(rec.size()));
    g_enc.flat.insert(g_enc.flat.end(), rec.begin(), rec.end());
  }
  // TOC anchors, resolved while this spine's section is still the built one.
  const int tocN = g_driver->tocCount();
  for (int t = 0; t < tocN; t++) {
    if (g_driver->tocSpine(t) != spine) continue;
    const std::string anchor = g_driver->tocAnchor(t);
    int local = anchor.empty() ? -1 : g_driver->anchorLocalPage(anchor);
    if (local < 0 || local >= pages) local = 0;      // out-of-range anchors start the spine, as serial does
    SpineTocEntry e;
    e.tocIndex = t;
    e.localPage = local;
    e.title = g_driver->tocTitle(t);
    g_enc.toc.push_back(e);
  }
  // Fallback chapter name, only used when the book has no usable TOC.
  {
    std::string href = g_driver->spineHref(spine);
    const size_t slash = href.find_last_of('/');
    if (slash != std::string::npos) href = href.substr(slash + 1);
    const size_t dot = href.find_last_of('.');
    if (dot != std::string::npos) href = href.substr(0, dot);
    g_enc.fallbackName = href.empty() ? ("Chapter " + std::to_string(spine + 1)) : href;
  }
  return pages;
}

KO_EXPORT int ko_spine_page_count() { return g_enc.count(); }
KO_EXPORT const uint8_t* ko_spine_data_ptr() { return g_enc.flat.empty() ? nullptr : g_enc.flat.data(); }
KO_EXPORT size_t ko_spine_data_size() { return g_enc.flat.size(); }
KO_EXPORT const uint32_t* ko_spine_page_offsets() { return g_enc.offsets.empty() ? nullptr : g_enc.offsets.data(); }
KO_EXPORT const uint32_t* ko_spine_page_lengths() { return g_enc.lengths.empty() ? nullptr : g_enc.lengths.data(); }
KO_EXPORT int ko_spine_toc_count() { return static_cast<int>(g_enc.toc.size()); }
KO_EXPORT int ko_spine_toc_index(int i) {
  return (i >= 0 && i < static_cast<int>(g_enc.toc.size())) ? g_enc.toc[i].tocIndex : -1;
}
KO_EXPORT int ko_spine_toc_local_page(int i) {
  return (i >= 0 && i < static_cast<int>(g_enc.toc.size())) ? g_enc.toc[i].localPage : -1;
}
KO_EXPORT const char* ko_spine_toc_title(int i) {
  return (i >= 0 && i < static_cast<int>(g_enc.toc.size())) ? g_enc.toc[i].title.c_str() : "";
}
KO_EXPORT const char* ko_spine_fallback_name() { return g_enc.fallbackName.c_str(); }

// ---- spine pool: centralised assembly ---------------------------------------
// Workers hand back page records; ONE assembler appends them in spine order and writes the container.
// Chapter candidates are added in SPINE order, and within a spine in TOC order, because that is the
// order the serial path accumulates them in — the shared buildChapters() then caps and stable-sorts
// exactly as before. Assembly order must not depend on which worker finished first.
KO_EXPORT int ko_assemble_begin(int mode) {
  if (!g_xtch) return -1;
  g_asm.reset(new ko::XtchWriter(mode == 0 ? ko::XtcMode::Mono1Bit : ko::XtcMode::Gray2Bit));
  g_asm->setTextAa(textAaEnabled());
  g_asm->adoptMetadataFrom(*g_xtch);      // the header carries the book's title/author
  g_asmCandidates.clear();
  g_asmFallback.clear();
  return 0;
}

// Append one spine's page records (flat buffer + per-page offsets/lengths).
KO_EXPORT int ko_assemble_add_spine(const uint8_t* data, size_t size, int pageCount,
                                    const uint32_t* offsets, const uint32_t* lengths) {
  if (!g_asm) return -1;
  if (pageCount <= 0) return static_cast<int>(g_asm->pageCount());
  if (data == nullptr || offsets == nullptr || lengths == nullptr) return -1;
  for (int i = 0; i < pageCount; i++) {
    const uint64_t off = offsets[i];
    const uint64_t len = lengths[i];
    if (off + len > size) return -1;              // a truncated record must never be assembled
    if (!g_asm->addRawPage(data + off, static_cast<size_t>(len))) return -1;
  }
  return static_cast<int>(g_asm->pageCount());
}

// One TOC anchor for a spine. spineBase is the spine's first global page.
KO_EXPORT void ko_assemble_add_toc(int spineBase, const char* title, int localPage) {
  ko::ChapterCandidate c;
  c.title = title ? title : "";
  c.page = static_cast<uint32_t>(spineBase + (localPage < 0 ? 0 : localPage));
  g_asmCandidates.push_back(c);
}

// The per-spine fallback chapter, used only when the book has no usable TOC.
KO_EXPORT void ko_assemble_add_fallback(int spineBase, const char* name, int pages) {
  if (pages <= 0) return;
  ko::XtchChapter ch;
  ch.name = name ? name : "";
  ch.startPage = static_cast<uint16_t>(spineBase);
  ch.endPage = static_cast<uint16_t>(spineBase + pages - 1);
  g_asmFallback.push_back(ch);
}

KO_EXPORT int ko_assemble_finish() {
  if (!g_asm) return -1;
  const uint32_t total = static_cast<uint32_t>(g_asm->pageCount());
  g_chapters = buildChapters(g_asmCandidates, g_asmFallback, total);
  g_xtchOut = g_asm->finish(g_chapters);
  g_xtchFullReady = 1;
  g_totalPages = static_cast<int>(total);   // the container is now the assembler's
  return g_totalPages;
}


// ---- spine pool: prefix-only assembly ---------------------------------------
//
// For an uncompressed container the page records do not need to pass through an engine at all. The
// header, metadata, chapter table and page index are functions of the page SIZES and the chapters —
// every index entry is (running offset, size, 480, 800) — so the pool can ask for the prefix alone and
// the browser composes [prefix][records…] as a Blob.
//
// This removes the whole-container copies that the full assembler performs after encoding: 162 MB
// transferred to an assembler, copied into its heap, copied into page vectors, copied again by
// finish(), and copied back out to JS. What crosses now is the page SIZES and ~10 KB of prefix.
// The format still has one implementation: buildPrefix() is the same code finish() uses.
//
// XTCZ (LZ4) deliberately keeps the full assembler: compression consumes the logical byte stream, so it
// needs the bytes. Streaming XTZ4 over Blob parts is a later step, not this one.
static std::vector<uint32_t> g_planSizes;
static std::vector<ko::ChapterCandidate> g_planCandidates;
static std::vector<ko::XtchChapter> g_planFallback;
static std::vector<uint8_t> g_planPrefix;
static int g_planMode = 1;

KO_EXPORT int ko_plan_begin(int mode) {
  g_planSizes.clear();
  g_planCandidates.clear();
  g_planFallback.clear();
  g_planPrefix.clear();
  g_planMode = mode;
  return 0;
}

// One spine's page SIZES, in page order. Add spines in spine order.
KO_EXPORT int ko_plan_add_spine(const uint32_t* lengths, int count) {
  if (count < 0 || (count > 0 && lengths == nullptr)) return -1;
  for (int i = 0; i < count; i++) g_planSizes.push_back(lengths[i]);
  return static_cast<int>(g_planSizes.size());
}

KO_EXPORT void ko_plan_add_toc(int spineBase, const char* title, int localPage) {
  ko::ChapterCandidate c;
  c.title = title ? title : "";
  c.page = static_cast<uint32_t>(spineBase + (localPage < 0 ? 0 : localPage));
  g_planCandidates.push_back(c);
}

KO_EXPORT void ko_plan_add_fallback(int spineBase, const char* name, int pages) {
  if (pages <= 0) return;
  ko::XtchChapter ch;
  ch.name = name ? name : "";
  ch.startPage = static_cast<uint16_t>(spineBase);
  ch.endPage = static_cast<uint16_t>(spineBase + pages - 1);
  g_planFallback.push_back(ch);
}

KO_EXPORT int ko_plan_finish() {
  if (!g_xtch) return -1;
  ko::XtchWriter w(g_planMode == 0 ? ko::XtcMode::Mono1Bit : ko::XtcMode::Gray2Bit);
  w.adoptMetadataFrom(*g_xtch);          // the header carries the book's title/author
  const uint32_t total = static_cast<uint32_t>(g_planSizes.size());
  g_chapters = ko::buildChapters(g_planCandidates, g_planFallback, total);
  g_planPrefix = w.buildPrefix(g_chapters, g_planSizes);
  return static_cast<int>(total);
}

KO_EXPORT const uint8_t* ko_plan_prefix_ptr() { return g_planPrefix.empty() ? nullptr : g_planPrefix.data(); }
KO_EXPORT size_t ko_plan_prefix_size() { return g_planPrefix.size(); }

// One-shot convenience: whole book in a single call (used by host/tests).
KO_EXPORT int ko_render_xtch() {
  const int spines = ko_export_begin();
  if (spines < 0) return -1;
  for (int s = 0; s < spines; s++) {
    if (ko_export_spine(s) < 0) continue;
  }
  return ko_export_finish();
}

// Module-lifetime output cache (declared above with the other globals)

KO_EXPORT const uint8_t* ko_xtch_ptr() {
  if (!g_xtchFullReady) return nullptr;
  if (g_xtchOut.empty()) g_xtchOut = g_xtch->finish(g_chapters);
  return g_xtchOut.data();
}

KO_EXPORT size_t ko_xtch_size() { return g_xtchOut.size(); }

// ---- XTCZ (LZ4) compressed container ----------------------------------------
// Official layout (epub2xtc.xteink.cn compressXtczLz4, verified from minified
// bundle): "XTZ4" magic + u32LE uncompressed size + u32LE block size (4096),
// then per block: u32LE length, bit31 set => raw stored, else LZ4-compressed
// block bytes; terminator u32LE 0. Requires device firmware >= 5.1.6.
#include <lz4.h>
KO_EXPORT void ko_xtcz_wrap() {
  if (!g_xtchFullReady || g_xtchOut.empty()) return;
  const size_t rawLen = g_xtchOut.size();
  constexpr uint32_t kBlock = 4096;
  const uint32_t numBlocks = static_cast<uint32_t>((rawLen + kBlock - 1) / kBlock);

  // worst case: header (12B) + per block 4B len + LZ4_compressBound(4096) (~4200)
  const size_t cap = 12 + numBlocks * (4 + LZ4_compressBound(kBlock)) + 4;
  std::vector<uint8_t> out;
  out.reserve(cap);
  const auto putU32 = [](std::vector<uint8_t>& v, uint32_t x) {
    v.push_back(x & 0xFF); v.push_back((x >> 8) & 0xFF);
    v.push_back((x >> 16) & 0xFF); v.push_back((x >> 24) & 0xFF);
  };
  out.push_back('X'); out.push_back('T'); out.push_back('Z'); out.push_back('4');
  putU32(out, static_cast<uint32_t>(rawLen));
  putU32(out, kBlock);

  // LZ4_compress_default is stateless (creates its own table per call); block
  // size 4096 keeps the per-call cost trivial even for ~46k blocks on a
  // 190 MB book. White-dominant e-ink pages compress well.
  std::vector<uint8_t> comp(LZ4_compressBound(kBlock));
  for (size_t off = 0; off < rawLen; off += kBlock) {
    const uint32_t chunk = static_cast<uint32_t>(std::min<size_t>(kBlock, rawLen - off));
    if (chunk < 13) {  // too small to compress: always store raw
      putU32(out, 0x80000000u | chunk);
      out.insert(out.end(), g_xtchOut.data() + off, g_xtchOut.data() + off + chunk);
      continue;
    }
    const int c = LZ4_compress_default(
        reinterpret_cast<const char*>(g_xtchOut.data() + off),
        reinterpret_cast<char*>(comp.data()),
        static_cast<int>(chunk), static_cast<int>(comp.size()));
    if (c > 0 && static_cast<size_t>(c) < chunk) {
      putU32(out, static_cast<uint32_t>(c));
      out.insert(out.end(), comp.data(), comp.data() + c);
    } else {
      putU32(out, 0x80000000u | chunk);
      out.insert(out.end(), g_xtchOut.data() + off, g_xtchOut.data() + off + chunk);
    }
  }
  putU32(out, 0);  // terminator
  g_xtchOut.swap(out);
}

// Release the module-lifetime container buffer (frees wasm heap; the JS side
// keeps its own copy once exported). Safe to call anytime.
// Live HalStorage accounting (§: measure before changing the preflight). The warm preflight calls
// _ko_build_spine(), which builds a complete RAM-backed section-cache file per spine — so "no page
// buffers allocated" is only true for the XTH/XTG export buffers. This exposes what the store holds.
KO_EXPORT size_t ko_storage_bytes() {
  return Storage.totalBytes();
}

KO_EXPORT void ko_xtch_release() {
  g_xtchOut.clear();
  g_xtchOut.shrink_to_fit();
  g_xtchFullReady = 0;
}

// Abort an export in progress: drop the partially accumulated container AND the page buffers the
// writer is holding, so a cancelled warm/export releases its wasm heap now instead of waiting for the
// next ko_export_begin(). §3 of the 1.2 audit. Safe to call anytime (the next begin() re-inits).
KO_EXPORT void ko_export_abort() {
  if (g_xtch) g_xtch->reset();
  g_chapters.clear();
  g_spineFallback.clear();
  g_chapterCandidates.clear();
  g_totalPages = 0;
  g_spinePageStart = 0;
  g_xtchFullReady = 0;
  ko_xtch_release();
}

// Preview compose: three packed 1-bpp planes -> 480x800 RGBA in ONE call, so the JS side
// never touches a pixel. Byte-verified against the frozen JS implementation over identical
// planes; scripts/preview-compose/compose_rgba.cpp holds the standalone, native-tested copy
// of this exact body. See docs/ko-preview-wasm-compose.md.
//
// Why the 8-bit grouping: for a fixed byte column c, the byte offsets phyX = 8c+0..8c+7 live
// in eight BITS OF THE SAME BYTE, so one strided fetch serves eight logical rows - 8x fewer
// strided reads (3 x 384,000 -> 3 x 48,000) while every output row is still written densely.
// Measured 1.43x faster than the naive per-pixel loop in the same language, which is how we
// know the win is the grouping and not the move into C++.
static std::vector<uint32_t> g_rgbaOut;  // 480*800 packed RGBA words, allocated once

KO_EXPORT uint8_t* ko_rgba_ptr() {
  if (g_rgbaOut.size() != 480u * 800u) g_rgbaOut.assign(480u * 800u, 0);
  return reinterpret_cast<uint8_t*>(g_rgbaOut.data());
}

// mono: 0 = 2-bit page (four shades), 1 = 1-bit page (the ink the XTG file carries:
// grey pixels blue-noise halftoned to 2 levels, solid ink thinned only with text AA on).
KO_EXPORT int ko_compose_rgba(int mono) {
  const uint8_t* bw = ko_plane_ptr(0);
  if (!bw) return -1;
  // A 1-bit page is never a pure function of the BW plane: grey pixels become ink
  // dots. The grey planes are therefore always read for mono; textAa decides only
  // whether solid ink is thinned as well.
  const bool textAa = textAaEnabled();
  const uint8_t* lsb = ko_plane_ptr(1);
  const uint8_t* msb = ko_plane_ptr(2);
  if (g_rgbaOut.size() != 480u * 800u) g_rgbaOut.assign(480u * 800u, 0);
  uint32_t* out = g_rgbaOut.data();

  // Preview palette: the four page levels at the PANEL's own relative reflectances (white 210,
  // light grey 80, dark grey 30, black 15 - the same anchors xtch_writer.h derives the 92%/67%
  // ink densities from), sRGB-encoded so a monitor's LINEAR light reproduces them:
  //   v=0 white      (210-15)/195 = 1.000 -> 255
  //   v=1 dark grey  ( 30-15)/195 = 0.077 ->  78
  //   v=2 light grey ( 80-15)/195 = 0.333 -> 156
  //   v=3 black      ( 15-15)/195 = 0.000 ->   0
  // The extremes are normalised onto the screen's full range deliberately: how dim the panel's
  // paper is, and that its black is a dark grey, are properties of the panel - not of this file -
  // and normalising is what makes the preview legible on a bright monitor while staying
  // tone-exact. A dithered patch then integrates (the eye averages light, not display codes) to
  // the reflectance the device shows. The previous palette {0,128,205,255} lifted dark grey 4.3x
  // and light grey 2.6x, so every grey patch read brighter than the panel, and mid-greys looked
  // washed out; do not restore it. Verify with scripts/verify/preview_fidelity.py.
  static constexpr uint32_t kGray32[4] = {0xFFFFFFFFu, 0xFF4E4E4Eu, 0xFF9C9C9Cu, 0xFF000000u};
  // 1-bit: the panel's two extremes (210 and 15), already faithful under the same normalisation.
  static constexpr uint32_t kMono32[2] = {0xFF000000u, 0xFFFFFFFFu};  // indexed by plane bit
  static constexpr uint8_t kLevelByMask[4] = {3, 2, 1, 1};
  const int colBytes = 100;  // physical row width in bytes
  // Same blue-noise masks the XTG writer inks with (ko::monoNoiseMasks): layer 0 =
  // dark grey v=1, layer 1 = light grey v=2, layer 2 = black v=3. Sharing the table
  // is what makes the 1-bit preview pixel-exact against the exported file.
  const MonoNoiseMasks& nm = monoNoiseMasks();

  for (int c = 0; c < colBytes; ++c) {
    for (int phyY = 0; phyY < 480; ++phyY) {
      const size_t idx = static_cast<size_t>(phyY) * colBytes + c;
      const uint8_t bwByte = bw[idx];
      const uint8_t lsbByte = lsb ? lsb[idx] : 0;
      const uint8_t msbByte = msb ? msb[idx] : 0;
      const int x = 479 - phyY;  // logical column for this physical row
      // Branchless (verified equivalent, 6.8x faster in the native harness): the 4-level
      // decision is a masked 4-entry lookup, and mono is hoisted out of the inner loop.
      // kLevelByMask maps (lsb<<1|msb) to v with lsb winning; (0 - !ink) zeroes it for non-ink.
      // The c * 8 * kW offset is essential: eight consecutive logical rows start at row c*8.
      uint32_t* dst = out + static_cast<size_t>(c) * 8 * 480 + x;
      if (mono) {
        const int k = x >> 3;
        const int jshift = 7 - (x & 7);  // the pixel's bit position inside a mask byte
        for (int b = 0; b < 8; ++b, dst += 480) {
          // Mirror of addMonoPage. NOTE the two different bit layouts: a plane byte
          // holds this pixel at bit (7 - (y & 7)) = (7 - b), while the mask byte holds
          // it at bit (7 - (x & 7)) = jshift, because the writer transposes the plane
          // bytes into the mask's layout before ANDing. So the level's mask bit must be
          // selected per pixel, not ANDed byte-wise.
          int ink = ((bwByte >> (7 - b)) & 1) ^ 1;  // bw bit 0 = ink
          if (ink) {
            const int pshift = 7 - b;
            const uint8_t l = (lsbByte >> pshift) & 1;
            const uint8_t m = (msbByte >> pshift) & 1;
            if (l || m) {
              // Grey pixel: always halftoned (dark = layer 0, light = layer 1).
              const uint8_t yc = static_cast<uint8_t>((c * 8 + b) & 63);
              ink = (nm.m[l ? 0 : 1][yc][k] >> jshift) & 1;
            } else if (textAa) {
              // Solid ink: thinned by the 255 layer only with text AA on.
              const uint8_t yc = static_cast<uint8_t>((c * 8 + b) & 63);
              ink = (nm.m[2][yc][k] >> jshift) & 1;
            }
          }
          dst[0] = ink ? kMono32[0] : kMono32[1];
        }
      } else {
        for (int b = 0; b < 8; ++b, dst += 480) {
          const int shift = 7 - b;
          const int bit = (bwByte >> shift) & 1;
          const int m = (((lsbByte >> shift) & 1) << 1) | ((msbByte >> shift) & 1);
          dst[0] = kGray32[kLevelByMask[m] & (0 - (bit ^ 1))];
        }
      }
    }
  }
  return 0;
}

KO_EXPORT void ko_free(void*) {}

}  // extern "C"
