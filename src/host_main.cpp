// host_main.cpp — KO-fork EPUB → XTCH converter driver (host build).
// Replicates EpubReaderActivity's render pipeline headlessly using the shared
// ko::EngineDriver, then encodes pages with ko::XtchWriter into a real XTCH
// container (56B header + 256B metadata + chapters + index + XTH page data).
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "ko_engine_driver.h"
#include "xtch_writer.h"

// Global display instance the renderer references
HalDisplay display;

int main(int argc, char** argv) {
  if (argc < 2) {
    fprintf(stderr, "usage: %s <book.epub> [out.xtch]\n", argv[0]);
    return 2;
  }
  const std::string epubPath = argv[1];
  const std::string outPath = argc > 2 ? argv[2] : "out.xtch";

  FILE* f = fopen(epubPath.c_str(), "rb");
  if (!f) { fprintf(stderr, "cannot open %s\n", epubPath.c_str()); return 1; }
  fseek(f, 0, SEEK_END);
  long sz = ftell(f);
  fseek(f, 0, SEEK_SET);
  std::vector<uint8_t> epubBytes(sz);
  if (fread(epubBytes.data(), 1, sz, f) != static_cast<size_t>(sz)) { fclose(f); return 1; }
  fclose(f);

  display.begin();
  GfxRenderer renderer(display);
  renderer.begin();

  // Fonts: KO typography build = Pretendard 10 (UI) + RIDIBatang 14 (reader,
  // default), KoPub Batang 14 kept switchable.
  EpdFont pretendard10(&pretendard_10_regular);
  EpdFontFamily uiFamily(&pretendard10);
  EpdFont kopub14(&kopub_14_regular);
  EpdFontFamily kopubFamily(&kopub14);
  EpdFont ridibatang14(&ridibatang_14_regular);
  EpdFontFamily ridibatangFamily(&ridibatang14);
  renderer.insertFont(UI_FONT_ID, &uiFamily);
  renderer.insertFont(UI_10_FONT_ID, &uiFamily);
  renderer.insertFont(UI_12_FONT_ID, &uiFamily);
  renderer.insertFont(SMALL_FONT_ID, &uiFamily);
  renderer.insertFont(KOPUB_14_FONT_ID, &kopubFamily);
  renderer.insertFont(RIDIBATANG_14_FONT_ID, &ridibatangFamily);
  renderer.setFallbackFont(UI_FONT_ID);

  ko::EngineDriver driver(renderer, display);
  if (!driver.loadEpubFromBlob(epubBytes.data(), epubBytes.size(), epubPath)) {
    fprintf(stderr, "Epub::load failed\n");
    return 1;
  }
  fprintf(stderr, "loaded: title='%s' spines=%d\n", driver.title().c_str(), driver.spineCount());

  ko::Spec spec;
  const int mTop = spec.marginTop, mRight = spec.marginRight;
  const int mBottom = spec.marginBottom, mLeft = spec.marginLeft;
  spec.viewportWidth = renderer.getScreenWidth() - mLeft - mRight;
  spec.viewportHeight = renderer.getScreenHeight() - mTop - mBottom;
  fprintf(stderr, "viewport %ux%u (margins t%d r%d b%d l%d)\n", spec.viewportWidth,
          spec.viewportHeight, mTop, mRight, mBottom, mLeft);

  ko::XtchWriter writer;
  writer.setMetadata(driver.title(), "unknown", "", "ko");
  int totalPages = 0;
  std::vector<ko::XtchChapter> chapters;
  int chapterStart = 0;
  int spineCount = driver.spineCount();
  for (int spine = 0; spine < spineCount; spine++) {
    const int n = driver.buildSection(spine, spec);
    if (n < 0) { fprintf(stderr, "spine %d: build failed\n", spine); continue; }
    fprintf(stderr, "spine %d/%d: %d pages\n", spine, spineCount, n);
    for (int p = 0; p < n; p++) {
      ko::RenderedPage rp;
      if (!driver.renderPage(p, spec, rp)) { fprintf(stderr, "  page %d failed\n", p); continue; }
      writer.addPageFromPlanes(rp.bw, rp.lsb, rp.msb);
      totalPages++;
      if (totalPages % 25 == 0) fprintf(stderr, "  ...%d\n", totalPages);
    }
    if (n > 0) {
      ko::XtchChapter ch;
      ch.name = "Chapter " + std::to_string(spine + 1);
      ch.startPage = static_cast<uint16_t>(chapterStart);
      ch.endPage = static_cast<uint16_t>(chapterStart + n - 1);
      chapters.push_back(ch);
      chapterStart += n;
    }
  }
  fprintf(stderr, "rendered %d pages; finalizing container\n", totalPages);

  std::vector<uint8_t> out = writer.finish(chapters);
  FILE* o = fopen(outPath.c_str(), "wb");
  if (!o) { fprintf(stderr, "cannot write %s\n", outPath.c_str()); return 1; }
  fwrite(out.data(), 1, out.size(), o);
  fclose(o);
  fprintf(stderr, "DONE %s (%zu bytes, %d pages, %zu chapters)\n", outPath.c_str(), out.size(),
          totalPages, chapters.size());
  return 0;
}
