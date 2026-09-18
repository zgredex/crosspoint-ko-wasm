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

// Page-plane capture after the 3-pass render, in PHYSICAL (800x480) layout.
// The encoder converts to logical portrait 480x800 + XTH packing.
struct RenderedPage {
  std::vector<uint8_t> bw;    // 48000 bytes: BW pass (black=0)
  std::vector<uint8_t> lsb;   // 48000 bytes: LSB gray plane (dark-grey mask)
  std::vector<uint8_t> msb;   // 48000 bytes: MSB gray plane (light+dark mask)
};

// Page geometry as the REFERENCE reader computes it.
//
// CrossPoint-KO @ release/korean 84a39194, EpubReaderActivity::render():
//
//   renderer.getOrientedViewableTRBL(&t,&r,&b,&l);   // portrait 9/3/3/3
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
// So the reference viewport on the 480x800 panel is 464x764 at the default
// screenMargin of 5, and every screenMargin step grows top/left/right by 5 while
// the bottom stays pinned to the 19 px lane until screenMargin exceeds it.
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

constexpr Margins referenceMargins(int screenMargin) {
  return Margins{kViewableTop + screenMargin, kViewableRight + screenMargin,
                 kViewableBottom + (screenMargin > kReferenceStatusLane ? screenMargin : kReferenceStatusLane),
                 kViewableLeft + screenMargin};
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
  // Reference-reader geometry at the default screenMargin: 14 / 8 / 22 / 8 →
  // viewport 464x764. See ko::geom above for the derivation from
  // EpubReaderActivity::render(). applyScreenMargin() recomputes all four from a
  // screenMargin value; setting the margins directly is the raw override the
  // measurement flags use.
  uint16_t viewportWidth = 0;         // set by driver from margins
  uint16_t viewportHeight = 0;
  int marginTop = geom::referenceMargins(geom::kScreenMarginDefault).top;
  int marginRight = geom::referenceMargins(geom::kScreenMarginDefault).right;
  int marginBottom = geom::referenceMargins(geom::kScreenMarginDefault).bottom;
  int marginLeft = geom::referenceMargins(geom::kScreenMarginDefault).left;
  // --- reader face ---
  // CrossPoint-KO's reader face. CrossPointSettings::getReaderFontId() returns
  // hasCustomFont() ? CUSTOM_FONT_ID : KOPUB_14_FONT_ID — RIDIBatang does not
  // exist upstream, so it is an XTCKO extra, never the default.
  int fontId = KOPUB_14_FONT_ID;

  // Recompute all four margins the way the reference does. Call before the
  // first buildSection(); the viewport is derived from the margins, never set
  // independently.
  void applyScreenMargin(int screenMargin) {
    const geom::Margins m = geom::referenceMargins(screenMargin);
    marginTop = m.top;
    marginRight = m.right;
    marginBottom = m.bottom;
    marginLeft = m.left;
  }
};

class EngineDriver {
 public:
  EngineDriver(GfxRenderer& renderer, HalDisplay& display)
      : renderer_(renderer), display_(display) {}

  bool loadEpubFromBlob(const uint8_t* data, size_t size, const std::string& virtualPath) {
    Storage.mountBlob(virtualPath, data, size);
    epubPath_ = virtualPath;
    epub_.reset(new Epub(virtualPath, "/.crosspoint"));
    if (!epub_->load(true, false)) return false;
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
  const std::string& title() const { return epub_->getTitle(); }
  const std::string& spineHref(int i) const { return epub_->getSpineItem(i).href; }

  // Build a section (spine) and return page count; -1 on failure.
  int buildSection(int spineIndex, const Spec& spec) {
    if (!epub_) return -1;
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

  bool renderPage(int pageIndex, const Spec& spec, RenderedPage& out, ManifestPage* probe = nullptr,
                  int spineIndex = 0) {
    if (!section_) return false;
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
    out.bw.assign(display_.getFrameBuffer(), display_.getFrameBuffer() + 48000);

    renderer_.clearScreen(0x00);
    renderer_.setRenderMode(GfxRenderer::GRAYSCALE_LSB);
    renderPass(textOnce ? true : !aaOn);
#ifndef KO_ORACLE_BUILD
    if (captureText) renderer_.orCapturedGrayInto(display_.getFrameBuffer(), true);
#endif
    renderer_.copyGrayscaleLsbBuffers();

    renderer_.clearScreen(0x00);
    renderer_.setRenderMode(GfxRenderer::GRAYSCALE_MSB);
    renderPass(textOnce ? true : !aaOn);
#ifndef KO_ORACLE_BUILD
    if (captureText) renderer_.orCapturedGrayInto(display_.getFrameBuffer(), false);
#endif
    renderer_.copyGrayscaleMsbBuffers();

    renderer_.setRenderMode(GfxRenderer::BW);
    out.lsb.assign(display_.getLsbPlane(), display_.getLsbPlane() + 48000);
    out.msb.assign(display_.getMsbPlane(), display_.getMsbPlane() + 48000);
    return true;
  }

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
  GfxRenderer& renderer_;
  HalDisplay& display_;
  std::shared_ptr<Epub> epub_;
  std::unique_ptr<Section> section_;
  std::string epubPath_;
  int pageCount_ = 0;
};

}  // namespace ko
