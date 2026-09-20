// ko_engine_driver.h — shared headless driver logic used by both the host
// converter (host_main.cpp) and the WASM module (wasm_api.cpp). Encapsulates:
//   epub load → font registration → per-spine Section build → page render in
//   the device's 3-pass (BW / LSB / MSB) grayscale pattern → plane capture.
//
// The renderer/display instances live in the embedding binary (host_main /
// wasm_api) because they need static/global storage; this class only holds
// engine pointers and spec state.
#pragma once

#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include <Epub.h>
#include "converters/ImagePerf.h"   // per-render image accounting (reset in renderPage)
#include <GfxRenderer.h>
#include <HalDisplay.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>
#include <ReaderRenderSpec.h>
#include <Section.h>
#include <Epub/Page.h>
#include <Epub/blocks/ImageBlock.h>
#include <converters/ImageDither.h>
#include <EpdFont.h>
#include <EpdFontFamily.h>
#include <FontCacheManager.h>
#include <FontDecompressor.h>
#include "layout_manifest.h"
#include "device_profile.h"
// §5 font-contribution measurement toggles. Default 1 so every other build (native host, any TU
// that misses the definition) keeps the fonts embedded and behaves exactly as before.
#ifndef KO_EMBED_KOPUB
#define KO_EMBED_KOPUB 1
#endif
#ifndef KO_EMBED_RIDI
#define KO_EMBED_RIDI 1
#endif
#if KO_EMBED_KOPUB
#include <builtinFonts/kopub_14_regular.h>
#endif
#if KO_EMBED_RIDI
#include <builtinFonts/ridibatang_14_regular.h>
#endif
#include "fontIds.h"

namespace ko {

// CrossPointSettings::ORIENTATION values. Keep the numeric values identical to
// the Korean fork: they are part of the settings/API contract used by the web
// worker and the host verification binary.
enum ReaderOrientation : uint8_t {
  PORTRAIT = 0,
  LANDSCAPE_CW = 1,
  PORTRAIT_INVERTED = 2,
  LANDSCAPE_CCW = 3,
};

inline bool isReaderOrientation(int orientation) {
  return orientation >= PORTRAIT && orientation <= LANDSCAPE_CCW;
}

inline void applyReaderOrientation(GfxRenderer& renderer, int orientation) {
  switch (orientation) {
    case LANDSCAPE_CW:
      renderer.setOrientation(GfxRenderer::LandscapeClockwise);
      break;
    case PORTRAIT_INVERTED:
      renderer.setOrientation(GfxRenderer::PortraitInverted);
      break;
    case LANDSCAPE_CCW:
      renderer.setOrientation(GfxRenderer::LandscapeCounterClockwise);
      break;
    case PORTRAIT:
    default:
      renderer.setOrientation(GfxRenderer::Portrait);
      break;
  }
}

// Page-plane capture after the 3-pass render, in the selected profile's
// PHYSICAL panel layout (X4 800x480, X3 792x528). The encoder maps that panel
// capture into the same profile's portrait XTC record (480x800 or 528x792);
// landscape content is therefore pre-rotated for XtcReaderActivity.
struct RenderedPage {
  std::vector<uint8_t> bw;    // profile plane bytes: BW pass (black=0)
  std::vector<uint8_t> lsb;   // profile plane bytes: LSB gray plane (dark-grey mask)
  std::vector<uint8_t> msb;   // profile plane bytes: MSB gray plane (light+dark mask)
};

// Page geometry as the REFERENCE reader computes it.
//
// CrossPoint-KO @ release/korean 84a39194, EpubReaderActivity::render():
//
//   renderer.getOrientedViewableTRBL(&t,&r,&b,&l);
//   t += SETTINGS.screenMargin;  l += SETTINGS.screenMargin;  r += SETTINGS.screenMargin;
//   b += std::max(SETTINGS.screenMargin, UITheme::getStatusBarHeight());
//
// getStatusBarHeight() at shipped defaults (CrossPointSettings.h):
//   statusBarChapterPageCount=1, statusBarBookProgressPercentage=1,
//   statusBarTitle=CHAPTER_TITLE, statusBarBattery=1  →  textLaneVisible(true) == true
//   progressBarMode=HIDE_PROGRESS                     →  no progress bar term
//   → UITheme::getStatusBarHeight() == metrics.statusBarVerticalMargin == 19
//     (BaseTheme.h:138 / LyraTheme.h:37 / RoundedRaffTheme.h:37 — all shipped themes)
//
// The physical safe area is 9/3/3/3 in portrait and rotates with the selected
// reader orientation. At screenMargin=5 the reference viewports are therefore:
//   portrait             margins 14/8/22/8  -> 464x764
//   landscape clockwise  margins  8/14/22/8 -> 778x450
//   landscape CCW        margins  8/8/22/14 -> 778x450
// The status lane is added at the logical bottom after the hardware-safe
// margins are rotated, exactly like EpubReaderActivity::render().
//
// The lane is a RESERVATION, not content: nothing of the status bar is ever
// written into an exported page (the device composites its own chrome at read
// time, and XtcReaderActivity's default xtcMode is XTC_STATUS_BAR_HIDE). It
// exists here only so the lines land on the same y the reader would put them on.
namespace geom {

constexpr int kViewableTop = 9;     // GfxRenderer::VIEWABLE_MARGIN_TOP
constexpr int kViewableRight = 3;   // GfxRenderer::VIEWABLE_MARGIN_RIGHT
constexpr int kViewableBottom = 3;  // GfxRenderer::VIEWABLE_MARGIN_BOTTOM
constexpr int kViewableLeft = 3;    // GfxRenderer::VIEWABLE_MARGIN_LEFT

constexpr int kScreenMarginDefault = 5;   // CrossPointSettings::SCREEN_MARGIN_MIN
constexpr int kScreenMarginMin = 5;       // SCREEN_MARGIN_MIN
constexpr int kScreenMarginMax = 40;      // SCREEN_MARGIN_MAX
constexpr int kScreenMarginStep = 5;      // SCREEN_MARGIN_STEP

// UITheme::getStatusBarHeight() on the shipped defaults. Fixed: the reference
// reader has no way to be in a state where the lane height differs without the
// user changing status-bar settings, and those are not part of a book.
constexpr int kReferenceStatusLane = 19;

struct Margins {
  int top;
  int right;
  int bottom;
  int left;
};

constexpr Margins orientedViewableMargins(int orientation) {
  return orientation == LANDSCAPE_CW
             ? Margins{kViewableLeft, kViewableTop, kViewableRight, kViewableBottom}
         : orientation == PORTRAIT_INVERTED
             ? Margins{kViewableBottom, kViewableLeft, kViewableTop, kViewableRight}
         : orientation == LANDSCAPE_CCW
             ? Margins{kViewableRight, kViewableBottom, kViewableLeft, kViewableTop}
             : Margins{kViewableTop, kViewableRight, kViewableBottom, kViewableLeft};
}

constexpr Margins referenceMargins(int screenMargin, int orientation = PORTRAIT) {
  const Margins safe = orientedViewableMargins(orientation);
  return Margins{safe.top + screenMargin, safe.right + screenMargin,
                 safe.bottom + (screenMargin > kReferenceStatusLane ? screenMargin : kReferenceStatusLane),
                 safe.left + screenMargin};
}

constexpr bool isScreenMarginAllowed(int screenMargin) {
  return screenMargin >= kScreenMarginMin && screenMargin <= kScreenMarginMax &&
         (screenMargin - kScreenMarginMin) % kScreenMarginStep == 0;
}

}  // namespace geom

// Mirror of CrossPointSettings knobs (values identical to device enums).
struct Spec {
  // --- Korean typography / layout knobs ---
  float lineCompression = 1.20f;      // NORMAL; 1.00 TIGHT, 1.20 NORMAL, 1.40 WIDE
  int extraParagraphSpacing = 1;      // 0/1
  int paragraphIndent = 0;            // 0/1 first-line indent (device default: off)
  int characterWrap = 1;              // 0/1 (KO default on: break at any char)
  int paragraphAlignment = 0;         // 0 JUSTIFIED 1 LEFT 2 CENTER 3 RIGHT 4 BOOK_STYLE
  int hyphenationEnabled = 0;         // only meaningful when characterWrap=0
  int embeddedStyle = 1;              // honor book's embedded CSS
  int imageRendering = 0;             // 0 DISPLAY 1 PLACEHOLDER 2 SUPPRESS
  int textAntiAliasing = 1;           // 0/1 (device default on: 2-bit glyph gray)
  int focusReadingEnabled = 0;
  // --- image dithering (ko-wasm extension; the device has no such knob) ---
  // Model codes are ko::DitherMode: 0 NONE, 1 BAYER, 2 BLUE_NOISE, 3 FS, 4 ATK, 5 JJN,
  // 6 STUCKI, 7 BURKES, 8 KO_HASH, 9 ZHOU_FANG. Applied at the export's tone depth.
  int imageDither = 2;                // BLUE_NOISE (the previous fixed behaviour)
  int imageToneDepth = 4;             // 4 = 2-bit XTCH, 2 = 1-bit XTC
  // --- page geometry ---
  DeviceProfile deviceProfile = DeviceProfile::X4;
  int orientation = PORTRAIT;         // CrossPointSettings::ORIENTATION (0..3)
  int screenMargin = geom::kScreenMarginDefault;
  // Reference-reader geometry at the default screenMargin: 14 / 8 / 22 / 8 →
  // viewport 464x764. See ko::geom above for the derivation from
  // EpubReaderActivity::render(). applyScreenMargin() recomputes all four from a
  // screenMargin value; setting the margins directly is the raw override the
  // measurement flags use.
  uint16_t viewportWidth = 0;         // set by driver from margins
  uint16_t viewportHeight = 0;
  int marginTop = geom::referenceMargins(geom::kScreenMarginDefault, PORTRAIT).top;
  int marginRight = geom::referenceMargins(geom::kScreenMarginDefault, PORTRAIT).right;
  int marginBottom = geom::referenceMargins(geom::kScreenMarginDefault, PORTRAIT).bottom;
  int marginLeft = geom::referenceMargins(geom::kScreenMarginDefault, PORTRAIT).left;
  // --- reader face ---
  // CrossPoint-KO's reader face. CrossPointSettings::getReaderFontId() returns
  // hasCustomFont() ? CUSTOM_FONT_ID : KOPUB_14_FONT_ID — RIDIBatang does not
  // exist upstream, so it is an XTCKO extra, never the default.
  int fontId = KOPUB_14_FONT_ID;

  // Recompute all four margins the way the reference does. Call before the
  // first buildSection(); the viewport is derived from the margins, never set
  // independently.
  void applyScreenMargin(int screenMargin) {
    this->screenMargin = screenMargin;
    const geom::Margins m = geom::referenceMargins(screenMargin, orientation);
    marginTop = m.top;
    marginRight = m.right;
    marginBottom = m.bottom;
    marginLeft = m.left;
  }

  void applyOrientation(int orientation) {
    this->orientation = isReaderOrientation(orientation) ? orientation : PORTRAIT;
    applyScreenMargin(screenMargin);
  }
};

class EngineDriver {
 public:
  EngineDriver(GfxRenderer& renderer, HalDisplay& display)
      : renderer_(renderer), display_(display) {}

  bool loadEpubFromBlob(const uint8_t* data, size_t size, const std::string& virtualPath) {
    Storage.mountBlob(virtualPath, data, size);
    return openEpub(virtualPath);
  }

  // Same, but ADOPTS a caller-owned allocation instead of copying it. The browser streams the EPUB into
  // the wasm heap and hands that pointer over; the ZIP reader then walks exactly those bytes. The
  // ownership transfer is the contract: the storage's Blob frees the pointer, and the caller must not.
  // CONTRACT: for non-null, non-empty input this call CONSUMES `data` whether parsing succeeds or fails.
  // The owned Blob's deleter frees the pointer, and a failed openEpub() removes the mount — which drops
  // that Blob. A caller must therefore never free `data` itself, and never read it again. (The host CLI
  // used to free it on failure: a double free on malformed input.)
  bool loadEpubFromOwnedBlob(uint8_t* data, size_t size, const std::string& virtualPath) {
    if (!data || size == 0) return false;
    Storage.mountOwnedBlob(virtualPath, data, size);
    return openEpub(virtualPath);
  }

  // Drop every trace of the mounted book, WITHOUT touching the storage. Called at the START of a book
  // replacement, before the new bytes are parsed, so that a parse failure leaves the engine describing
  // "no book" — not the previous book, whose backing files have already been cleared.
  void resetBook() {
    ImageBlock::setExtractor(nullptr, nullptr);
    section_.reset();
    epub_.reset();
    epubPath_.clear();
    pageCount_ = 0;
  }

  // A runtime device-profile change keeps the mounted EPUB but invalidates
  // pagination made for the previous screen geometry.
  void invalidateSection() {
    section_.reset();
    pageCount_ = 0;
  }

  // Range-backed mount: the EPUB is never made resident. `readFn` serves aligned windows on demand from
  // wherever the bytes actually live (the page's File, a real file on the host). Nothing is copied and
  // nothing is adopted, so there is no ownership question — the source outlives the mount by contract.
  bool loadEpubFromExternal(size_t size, const std::string& virtualPath,
                            int (*readFn)(void* ctx, size_t offset, uint8_t* dst, size_t len),
                            void* ctx) {
    if (!readFn || size == 0) return false;
    Storage.mountExternalBlob(virtualPath, size, readFn, ctx);
    return openEpub(virtualPath);
  }

  // Shared tail of both entry points; kept in one place so the two cannot drift apart in what they set
  // up (image extractor hook, Epub construction, load arguments).
  bool openEpub(const std::string& virtualPath) {
    epubPath_ = virtualPath;
    epub_.reset(new Epub(virtualPath, "/.crosspoint"));
    if (!epub_->load(true, false)) {
      // Destroy the failed Epub FIRST (while its backing mount still exists, so nothing it holds dangles),
      // then drop the mount — for an owned mount that releases the adopted buffer.
      //
      // This used to leave epub_ non-null after a failed parse, which made hasBook() report true and let
      // every requireBook() guard pass against a half-initialised Epub. The guards looked right while
      // their predicate was wrong; spineCount() would even report a stale count.
      resetBook();
      Storage.remove(virtualPath.c_str());
      return false;
    }
    // Lazy image extraction: section builds only header-probe images; the first
    // render of an image page pulls the file out of the EPUB through this hook
    // (mirrors EpubReaderActivity::onEnter).
    ImageBlock::setExtractor(epub_.get(), [](void* ctx, const char* src, const char* dest) {
      return static_cast<Epub*>(ctx)->extractItemToFile(src, dest);
    });
    return true;
  }

  // Direct access for cover/thumb generation (device library path)
  std::shared_ptr<Epub> epubShared() { return epub_; }

  int spineCount() const { return epub_ ? epub_->getSpineItemsCount() : 0; }
  // First spine worth reading: skips cover/author pages (device open behavior)
  int textReferenceSpine() const {
    if (!epub_) return 0;
    int t = epub_->getSpineIndexForTextReference();
    return t >= 0 && t < spineCount() ? t : 0;
  }
  // Fail-closed book replacement made "driver alive, no book mounted" a legitimate state (see
  // beginBookReplacement). These accessors used to assume the two were equivalent and dereferenced the
  // EPUB unconditionally, so a title read after a failed replacement was a null dereference.
  bool hasBook() const { return static_cast<bool>(epub_); }

  const std::string& title() const {
    static const std::string empty;
    return epub_ ? epub_->getTitle() : empty;
  }
  // RETURNS BY VALUE. This used to return `const std::string&` bound to
  // `epub_->getSpineItem(i).href`, but getSpineItem() returns BookMetadataCache::SpineEntry BY VALUE
  // (Epub.h:67), so the reference outlived the temporary that owned the string. Short hrefs live in the
  // string's SSO buffer, i.e. inside that dead temporary, which is where the nondeterministic labels
  // ("䏆", U+070F U+0006) came from. Copying here removes the UB at the source instead of reducing how
  // often it is observed.
  //
  // Note for whoever tries to gate this: the host build does NOT reproduce it — a Release x86_64 build
  // happens to leave the temporary's bytes intact at the point the copy reads them (measured: the
  // --spine-hrefs sweep passes with the bug reinstated). It showed up in the wasm build. So the argument
  // here is the language rule plus the browser behaviour, not a host regression test.
  std::string spineHref(int i) const {
    if (!epub_ || i < 0 || i >= spineCount()) return {};
    return epub_->getSpineItem(i).href;
  }

  // Build a section (spine) and return page count; -1 on failure.
  int buildSection(int spineIndex, const Spec& spec) {
    if (!epub_) return -1;
    applyReaderOrientation(renderer_, spec.orientation);
    section_.reset();
    ReaderRenderSpec rs = toReaderSpec(spec);
    section_.reset(new Section(epub_, spineIndex, renderer_));
    if (!section_->createSectionFile(rs)) return -1;
    pageCount_ = section_->pageCount;
    return pageCount_;
  }

  // Progressive section build: lay out enough of a spine for the first page, render it, then keep
  // laying out the rest in chunks. A long first chapter otherwise means "paginate the whole chapter
  // before page 1 exists" — measured at 48.8 ms of a 68.2 ms first page on a 326-page single spine.
  // The Section API is unchanged; this only stops calling the build-to-completion wrapper.
  int startSection(int spineIndex, const Spec& spec, int initialPages) {
    if (!epub_) return -1;
    applyReaderOrientation(renderer_, spec.orientation);
    section_.reset();
    ReaderRenderSpec rs = toReaderSpec(spec);
    section_.reset(new Section(epub_, spineIndex, renderer_));
    if (!section_->startBuild(rs)) return -1;
    if (!section_->buildSomeMore(initialPages)) return -1;
    pageCount_ = section_->pageCount;
    return pageCount_;
  }

  // Lay out up to maxPages more pages of the section started above. >0 to completion.
  int buildSectionMore(int maxPages) {
    if (!section_) return -1;
    if (!section_->buildSomeMore(maxPages)) return -1;
    pageCount_ = section_->pageCount;
    return pageCount_;
  }

  // Gate switch (see renderPage): render three-pass instead of image-once, so the two can be compared
  // from one binary. Not a product setting.
  void setThreePass(bool on) { threePass_ = on; }
  bool threePass() const { return threePass_; }

  bool sectionBuildComplete() const { return section_ && section_->isBuildComplete(); }
  int availablePages() const { return section_ ? section_->pageCount : 0; }
  int estimatedPages() const { return section_ ? section_->estimatedTotalPages() : 0; }

  // Render one page of the current section into the display's physical planes
  // and return copies (bw / lsb / msb). Mirrors the device's text-settings AA
  // behavior: with AA on (default), all three passes render the full page so
  // 2-bit glyphs contribute their gray; with AA off, text renders only in the
  // BW pass (1-bit) while images still get their grayscale passes
  // (EpubReaderActivity: needsTextGrayscale ? page->render : page->renderImages).
  // The image decoders read these through ko::imageDitherOptions(); setting them here
  // keeps the decoders free of spec dependencies (they are shared with the device build).
  static void applyImageDitherOptions(const Spec& spec) {
    ko::ImageDitherOptions opts;
    const int maxCode = static_cast<int>(DitherMode::ZHOU_FANG);
    const int code = (spec.imageDither >= 0 && spec.imageDither <= maxCode)
                         ? spec.imageDither
                         : static_cast<int>(DitherMode::BLUE_NOISE);
    opts.mode = static_cast<DitherMode>(code);
    opts.toneDepth = (spec.imageToneDepth == 2) ? 2 : 4;
    ko::setImageDitherOptions(opts);
  }

  // `monoOnly` skips both gray passes. It is a NEGATIVE CONTROL, not an optimization — kept so
  // scripts/verify/mono_planes_gate.py can demonstrate why. Dropping the gray passes changes a 1-bit
  // container: the XTG writer's addMonoPage() reads them (grey pixels become ink dots; with AA on, solid
  // ink is thinned with them) and so does the preview compositor, so a 1-bit page is not a function of
  // the BW plane. Measured: thousands of differing pixels on a text fixture. Every product path passes
  // false; only `ko_xtch_host --drop-gray-planes` passes true.
  // The PRODUCT entry point for rendering a page. It takes NO boolean that can drop the gray passes.
  //
  // An audit proposed skipping them for 1-bit output on the theory that a 1-bit consumer reads only the
  // BW plane. That was implemented, pushed, and silently corrupted every 1-bit export and preview: both
  // xtch_writer.h::addMonoPage() and wasm_api.cpp::ko_compose_rgba() READ lsb/msb (grey pixels become ink
  // dots; AA ink is thinned with them). Reverted, gated by scripts/verify/mono_planes_gate.py.
  //
  // Leaving it as a defaulted parameter would leave the product one careless `true` away from the same
  // regression, so the capability now lives in the test-only entry point below and NOTHING in the shipped
  // module can select it: KO_TEST_NEGATIVE_CONTROLS is defined for the host CLI alone (see CMakeLists).
  bool renderPage(int pageIndex, const Spec& spec, RenderedPage& out, ManifestPage* probe = nullptr,
                  int spineIndex = 0) {
    return renderPageImpl(pageIndex, spec, out, probe, spineIndex, false);
  }

#ifdef KO_TEST_NEGATIVE_CONTROLS
  // Negative control, host-only. Passing true here is what proves the gray planes are load-bearing for a
  // 1-bit container; it must never be reachable from a product path.
  bool renderPageDroppingGrayForTest(int pageIndex, const Spec& spec, RenderedPage& out,
                                     ManifestPage* probe = nullptr, int spineIndex = 0) {
    return renderPageImpl(pageIndex, spec, out, probe, spineIndex, true);
  }
#endif

  // ---- TOC (chapter) access -------------------------------------------------
  // The device reader names the current chapter from the EPUB TOC (TocEntry:
  // title/href/anchor/level/spineIndex). Export chapters from the same source
  // so the file's chapter list matches device chapter navigation.
  int tocCount() const { return epub_ ? epub_->getTocItemsCount() : 0; }
  // Raw spine index stored in the TOC entry (-1 = unresolved). getSpineIndex-
  // ForTocIndex conflates spine 0 with errors, so read the entry directly.
  int tocSpine(int i) const {
    if (!epub_ || i < 0 || i >= epub_->getTocItemsCount()) return -1;
    return epub_->getTocItem(i).spineIndex;
  }
  std::string tocTitle(int i) const {
    if (!epub_ || i < 0 || i >= epub_->getTocItemsCount()) return "";
    return epub_->getTocItem(i).title;
  }
  std::string tocAnchor(int i) const {
    if (!epub_ || i < 0 || i >= epub_->getTocItemsCount()) return "";
    return epub_->getTocItem(i).anchor;
  }
  // Local page inside the CURRENT section for an anchor id (from the section's
  // anchor map), or -1 when unknown. Valid after buildSection().
  int anchorLocalPage(const std::string& anchor) const {
    if (!section_ || anchor.empty()) return -1;
    auto p = section_->findAnchor(anchor);
    return p ? static_cast<int>(*p) : -1;
  }

  void close() {
    ImageBlock::setExtractor(nullptr, nullptr);  // drop book ctx (mirrors onExit)
    section_.reset();
    epub_.reset();
  }

  ReaderRenderSpec toReaderSpec(const Spec& s) {
    ReaderRenderSpec rs;
    rs.fontId = s.fontId;
    rs.lineCompression = s.lineCompression;
    rs.extraParagraphSpacing = s.extraParagraphSpacing != 0;
    rs.paragraphAlignment = static_cast<uint8_t>(s.paragraphAlignment);
    rs.viewportWidth = s.viewportWidth;
    rs.viewportHeight = s.viewportHeight;
    rs.hyphenationEnabled = s.hyphenationEnabled != 0 && s.characterWrap == 0;
    rs.embeddedStyle = s.embeddedStyle != 0;
    rs.imageRendering = static_cast<uint8_t>(s.imageRendering);
    rs.focusReadingEnabled = s.focusReadingEnabled != 0;
    rs.paragraphIndent = s.paragraphIndent != 0;
    rs.characterWrap = s.characterWrap != 0;
    return rs;
  }

 private:

  // Private implementation. No defaults: every argument is supplied by the two entry points above, so a
  // defaulted `dropGrayPlanes` can never be reached by accident — which is the whole point of the split.
  bool renderPageImpl(int pageIndex, const Spec& spec, RenderedPage& out, ManifestPage* probe,
                      int spineIndex, bool dropGrayPlanes) {
    if (!section_) return false;
    // The export API snapshots Spec at begin(). Re-applying from that snapshot
    // here makes orientation transactional too: a stray live setter cannot
    // rotate only the latter half of an export.
    applyReaderOrientation(renderer_, spec.orientation);
    // Per-render image accounting starts here, in the ONE function every entry point goes through. It used
    // to live in ko_render_page, so when the worker switched to ko_render_page_mode the counters silently
    // accumulated across renders and reported nonsense (5, 6, 7, 8 for one image per page).
    ko::imagePerfReset();
    applyImageDitherOptions(spec);
    auto page = section_->loadPage(pageIndex);
    if (!page) return false;
    // Probe the layout while the page is alive. Cheap relative to the render, and it
    // keeps ONE code path: the manifest can never describe a different page than the
    // one that produced the planes.
    if (probe) *probe = probePage(*page, spineIndex, pageIndex);

    auto renderPass = [&](bool imagesOnly) {
      if (!imagesOnly || page->hasImages()) {
        if (imagesOnly) {
          page->renderImages(renderer_, spec.fontId, spec.marginLeft, spec.marginTop);
        } else {
          page->render(renderer_, spec.fontId, spec.marginLeft, spec.marginTop);
        }
      }
    };

    renderer_.clearScreen(0xFF);
    renderer_.setRenderMode(GfxRenderer::BW);

    const bool aaOn = spec.textAntiAliasing != 0;
    // ---------------------------------------------------------------------
    // TEXT-ONCE PATH — the port's optimization. ENABLED for the port build, gated
    // byte-identical against the pre-change engine (docs/stage-a-text-once.md).
    //
    // With AA on the gray passes contribute nothing but the text's coverage
    // classification, so text is blitted ONCE (the BW pass) while its per-pixel
    // level is recorded, and both gray planes are composed from that recording by
    // orCapturedGrayInto() instead of re-walking the page layout twice more.
    //
    // Two invariants make it exact:
    //   * the capture models BOTH halves of what a gray pass does - the OR of
    //     set-bits (captureLevel: accumulated, one bit per plane) and the
    //     mode-agnostic writes of drawLine / the fillRect dither templates /
    //     sup-sub scaled glyphs, which CLEAR the plane bit and therefore must be
    //     able to take back a level an earlier glyph blit recorded
    //     (captureAgnostic: overwriting). Modelling only the OR half left exactly
    //     the pixels where a rule or panel is drawn over text diverging.
    //   * everything the gray passes re-draw themselves (images, through
    //     DirectPixelWriter) only ever sets plane bits, never clears them, so
    //     OR-ing the captured text bits in afterwards reproduces the three-pass
    //     planes for image pages too.
    //
    // AA OFF is a separate, unchanged path (captureText is false): text is 1-bit,
    // only images reach the gray planes. Gated against the pre-change engine:
    // 0/2,034 pages differ with AA off, and 0/2,034 with AA on.
    //
    // Measured on the 2,034-page Korean text book: renderPage 1,370 -> 892 ms
    // (-35%), total 1,521 -> 1,043 ms; 0/2,034 pages differ. Book-wide gates
    // (text book, image book, and real EPUBs) are recorded in the doc above.
    //
    // REFERENCE BUILD (KO_ORACLE_BUILD): forced OFF, and the capture calls do not
    // exist at all. The reference GfxRenderer has no capture API — its behaviour IS
    // the plain three-pass render — so the reference build is also the standing
    // control that keeps this optimization honest: if the two builds' planes ever
    // differ, the shortcut is wrong.
    // ---------------------------------------------------------------------
#ifdef KO_ORACLE_BUILD
    const bool textOnce = false;
#else
    const bool textOnce = true;
#endif
    // AA OFF means text is drawn in the BW pass only: the reference's gray passes
    // render images and nothing else, so an AA-off page carries no text greys at
    // all. The capture must be switched off together with it, or the composed gray
    // planes would carry text greys that an AA-off page must not have.
    const bool captureText = textOnce && aaOn;

    // IMAGE-ONCE: the same capture now covers images too, so the gray passes have nothing left to draw
    // and are composed from the capture alone. DirectPixelWriter records the two gray bits an image
    // would set (level 1 -> both planes, level 2 -> MSB), which is exactly what its LSB/MSB passes did:
    // in those modes it either sets the bit or returns, never clears. Text was already covered here.
    //
    // What this removes per page: a full image decode AND dither for each gray pass. Measured on the
    // 549 KB JPEG cover, one preview render: 3 decodes, 28.4 ms of decode + 20.1 ms of draw for a 49.1 ms
    // render. With both gray passes skipped that is ~33 ms of the 49 ms, and the page still decodes once.
    //
    // AA OFF stays on the unchanged three-pass path, and so does the reference build: an AA-off page must
    // carry no text greys at all, and the reference renderer has no capture API. Both keep `textOnce`
    // false or `aaOn` false, so captureWholeGray is false for them by construction.
    // The gate switch: render the OLD way (capture text, still draw images in both gray passes) so the
    // optimized path can be compared against it from one binary. Default off; --three-pass on the host,
    // ko_set_three_pass() for the browser probe. Never exposed in the product UI.
    const bool captureWholeGray = textOnce && aaOn && !threePass_ && !dropGrayPlanes;

    // The three calls below are the port's capture API. It does not exist in the
    // reference renderer, so the reference build simply does not make them — and
    // captureText is compile-time false there, so nothing is skipped that would
    // have run.
#ifndef KO_ORACLE_BUILD
    if (captureText) renderer_.beginLevelCapture();
#endif
    renderPass(false);
#ifndef KO_ORACLE_BUILD
    if (captureText) renderer_.endLevelCapture();
#endif
    const size_t planeBytes = display_.getBufferSize();
    out.bw.assign(display_.getFrameBuffer(), display_.getFrameBuffer() + planeBytes);

    if (dropGrayPlanes) {
      // No gray work at all: no capture, no gray passes, no plane copies. The caller asked for the 1-bit
      // plane, so nothing here can change what it consumes.
      out.lsb.clear();
      out.msb.clear();
      renderer_.setRenderMode(GfxRenderer::BW);
      return true;
    }

    renderer_.clearScreen(0x00);
    renderer_.setRenderMode(GfxRenderer::GRAYSCALE_LSB);
    // Nothing to draw when the capture already holds both text's and images' contributions.
    if (!captureWholeGray) renderPass(textOnce ? true : !aaOn);
#ifndef KO_ORACLE_BUILD
    if (captureText) renderer_.orCapturedGrayInto(display_.getFrameBuffer(), true);
#endif
    renderer_.copyGrayscaleLsbBuffers();

    renderer_.clearScreen(0x00);
    renderer_.setRenderMode(GfxRenderer::GRAYSCALE_MSB);
    if (!captureWholeGray) renderPass(textOnce ? true : !aaOn);
#ifndef KO_ORACLE_BUILD
    if (captureText) renderer_.orCapturedGrayInto(display_.getFrameBuffer(), false);
#endif
    renderer_.copyGrayscaleMsbBuffers();

    renderer_.setRenderMode(GfxRenderer::BW);
    out.lsb.assign(display_.getLsbPlane(), display_.getLsbPlane() + planeBytes);
    out.msb.assign(display_.getMsbPlane(), display_.getMsbPlane() + planeBytes);
    return true;
  }
  GfxRenderer& renderer_;
  HalDisplay& display_;
  std::shared_ptr<Epub> epub_;
  std::unique_ptr<Section> section_;
  std::string epubPath_;
  int pageCount_ = 0;
  bool threePass_ = false;   // gate switch: render three-pass instead of image-once (see renderPage)
};

}  // namespace ko
