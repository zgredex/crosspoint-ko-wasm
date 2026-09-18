// host_main.cpp — KO-fork EPUB → XTCH converter driver (host build).
// Replicates EpubReaderActivity's render pipeline headlessly using the shared
// ko::EngineDriver, then encodes pages with ko::XtchWriter into a real XTCH
// container (56B header + 256B metadata + chapters + index + XTH page data).
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <chrono>
#include <memory>
#include <string>
#include <vector>

#include "ko_engine_driver.h"
#include "xtch_writer.h"

// Phase profiling: where does whole-book conversion actually spend its time?
// Layout (parse + paginate) and rasterize (glyphs + quantize) have completely
// different optimization strategies, so measure before touching either.
using Clock = std::chrono::steady_clock;
static double msSince(const Clock::time_point& t0) {
  return std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
}

// Global display instance the renderer references
HalDisplay display;

int main(int argc, char** argv) {
  if (argc < 2) {
    fprintf(stderr, "usage: %s <book.epub> [out.xtch] [--1bit] [--no-mono-dither]\n", argv[0]);
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
  auto tLoad0 = Clock::now();
  if (!driver.loadEpubFromBlob(epubBytes.data(), epubBytes.size(), epubPath)) {
    fprintf(stderr, "Epub::load failed\n");
    return 1;
  }
  const double tLoad = msSince(tLoad0);
  fprintf(stderr, "loaded: title='%s' spines=%d  [load %.1f ms]\n", driver.title().c_str(),
          driver.spineCount(), tLoad);

  ko::Spec spec;
  const int mTop = spec.marginTop, mRight = spec.marginRight;
  const int mBottom = spec.marginBottom, mLeft = spec.marginLeft;
  spec.viewportWidth = renderer.getScreenWidth() - mLeft - mRight;
  spec.viewportHeight = renderer.getScreenHeight() - mTop - mBottom;
  fprintf(stderr, "viewport %ux%u (margins t%d r%d b%d l%d)\n", spec.viewportWidth,
          spec.viewportHeight, mTop, mRight, mBottom, mLeft);

  ko::XtchWriter writer;
  // Output-mode flags, for verifying the 1-bit path (the web app sets the same mode
  // through ko_set_output_mode).
  // The text-AA switch is the control for the 1-bit blue-noise dither, matching the
  // web app: AA on -> the grey levels are halftoned into the 1-bit plane (pseudo
  // grey / antialiased-looking text and photos), AA off -> hard threshold, no greys.
  // --mono-dither / --no-mono-dither force the flag for testing that relationship.
  int monoDitherOverride = -1;  // -1 = follow text AA
  for (int i = 1; i < argc; i++) {
    const std::string flag = argv[i];
    if (flag == "--1bit") writer.setMode(ko::XtcMode::Mono1Bit);
    else if (flag == "--no-text-aa") spec.textAntiAliasing = 0;
    else if (flag == "--text-aa") spec.textAntiAliasing = 1;
    else if (flag == "--mono-dither") monoDitherOverride = 1;
    else if (flag == "--no-mono-dither") monoDitherOverride = 0;
  }
  writer.setMonoGrayDither(monoDitherOverride < 0 ? spec.textAntiAliasing != 0
                                                  : monoDitherOverride != 0);

  writer.setMetadata(driver.title(), "unknown", "", "ko");
  int totalPages = 0;
  std::vector<ko::XtchChapter> chapters;
  int chapterStart = 0;
  int spineCount = driver.spineCount();
  double tBuild = 0, tRender = 0, tWrite = 0;
  for (int spine = 0; spine < spineCount; spine++) {
    auto t0 = Clock::now();
    const int n = driver.buildSection(spine, spec);
    tBuild += msSince(t0);
    if (n < 0) { fprintf(stderr, "spine %d: build failed\n", spine); continue; }
    fprintf(stderr, "spine %d/%d: %d pages\n", spine, spineCount, n);
    for (int p = 0; p < n; p++) {
      ko::RenderedPage rp;
      t0 = Clock::now();
      if (!driver.renderPage(p, spec, rp)) { fprintf(stderr, "  page %d failed\n", p); continue; }
      tRender += msSince(t0);
      t0 = Clock::now();
      writer.addPageFromPlanes(rp.bw, rp.lsb, rp.msb);
      tWrite += msSince(t0);
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
  fprintf(stderr,
          "PROFILE  buildSection(parse+paginate) %.1f ms | renderPage(glyphs+quantize) %.1f ms"
          " | writer %.1f ms | total %.1f ms | %.2f ms/page\n",
          tBuild, tRender, tWrite, tBuild + tRender + tWrite,
          totalPages ? (tBuild + tRender + tWrite) / totalPages : 0.0);

  auto tFin0 = Clock::now();
  std::vector<uint8_t> out = writer.finish(chapters);
  const double tFinish = msSince(tFin0);
  FILE* o = fopen(outPath.c_str(), "wb");
  if (!o) { fprintf(stderr, "cannot write %s\n", outPath.c_str()); return 1; }
  tFin0 = Clock::now();
  fwrite(out.data(), 1, out.size(), o);
  fclose(o);
  const double tDisk = msSince(tFin0);
  fprintf(stderr, "DONE %s (%zu bytes, %d pages, %zu chapters)\n", outPath.c_str(), out.size(),
          totalPages, chapters.size());
  fprintf(stderr, "PROFILE2 load %.1f ms | finish(container build) %.1f ms | disk write %.1f ms\n",
          tLoad, tFinish, tDisk);
  return 0;
}
