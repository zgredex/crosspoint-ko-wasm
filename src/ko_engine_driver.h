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
#include <EpdFont.h>
#include <EpdFontFamily.h>
#include <FontCacheManager.h>
#include <FontDecompressor.h>
#include <builtinFonts/kopub_14_regular.h>
#include <builtinFonts/pretendard_10_regular.h>
#include <builtinFonts/ridibatang_14_regular.h>
#include "fontIds.h"

namespace ko {

// Page-plane capture after the 3-pass render, in PHYSICAL (800x480) layout.
// The encoder converts to logical portrait 480x800 + XTH packing.
struct RenderedPage {
  std::vector<uint8_t> bw;    // 48000 bytes: BW pass (black=0)
  std::vector<uint8_t> lsb;   // 48000 bytes: LSB gray plane (dark-grey mask)
  std::vector<uint8_t> msb;   // 48000 bytes: MSB gray plane (light+dark mask)
};

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
  // --- page geometry ---
  uint16_t viewportWidth = 0;         // set by driver from margins
  uint16_t viewportHeight = 0;
  int marginTop = 14;                 // physical-ish logical offsets (9 viewable + 5 screen)
  int marginRight = 8;
  int marginBottom = 8;
  int marginLeft = 8;
  // --- reader face ---
  int fontId = RIDIBATANG_14_FONT_ID;  // KO typography build default
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

  // Render one page of the current section into the display's physical planes
  // and return copies (bw / lsb / msb). Mirrors the device's text-settings AA
  // behavior: with AA on (default), all three passes render the full page so
  // 2-bit glyphs contribute their gray; with AA off, text renders only in the
  // BW pass (1-bit) while images still get their grayscale passes
  // (EpubReaderActivity: needsTextGrayscale ? page->render : page->renderImages).
  bool renderPage(int pageIndex, const Spec& spec, RenderedPage& out) {
    if (!section_) return false;
    auto page = section_->loadPage(pageIndex);
    if (!page) return false;

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
    // TEXT-ONCE PATH - ENABLED, gated byte-identical (docs/stage-a-text-once.md).
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
    // Measured on the 2,034-page Korean text book: renderPage 1,370 -> 892 ms
    // (-35%), total 1,521 -> 1,043 ms; 0/2,034 pages differ. Book-wide gates
    // (text book, image book, and real EPUBs) are recorded in the doc above.
    // ---------------------------------------------------------------------
    const bool textOnce = true;

    if (textOnce) renderer_.beginLevelCapture();
    renderPass(false);
    if (textOnce) renderer_.endLevelCapture();
    out.bw.assign(display_.getFrameBuffer(), display_.getFrameBuffer() + 48000);

    renderer_.clearScreen(0x00);
    renderer_.setRenderMode(GfxRenderer::GRAYSCALE_LSB);
    renderPass(textOnce ? true : !aaOn);
    if (textOnce) renderer_.orCapturedGrayInto(display_.getFrameBuffer(), true);
    renderer_.copyGrayscaleLsbBuffers();

    renderer_.clearScreen(0x00);
    renderer_.setRenderMode(GfxRenderer::GRAYSCALE_MSB);
    renderPass(textOnce ? true : !aaOn);
    if (textOnce) renderer_.orCapturedGrayInto(display_.getFrameBuffer(), false);
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
