// host_main.cpp — KO-fork EPUB → XTCH converter driver (host build).
// Replicates EpubReaderActivity's render pipeline headlessly using the shared
// ko::EngineDriver, then encodes pages with ko::XtchWriter into a real XTCH
// container (56B header + 256B metadata + chapters + index + XTH page data).
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <chrono>
#include <memory>
#include <string>
#include <sys/stat.h>
#include <vector>

#include "ko_engine_driver.h"
#include "external_font_loader.h"   // §7 gate: register KoPub from an EPD2 blob
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
    fprintf(stderr,
            "usage: %s <book.epub> [out.xtch] [--1bit] [--image-dither N] [--image-dither-name NAME]\n"
            "          [--text-aa|--no-text-aa] [--font kopub|ridibatang] [--kopub-external blob] [--no-kern]\n"
            "          [--screen-margin N | --margin-bottom N]\n"
            "          [--manifest PATH] [--dump-planes DIR] [--max-pages N]\n",
            argv[0]);
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

  // Fonts: KO typography build = Pretendard 10 (UI) + KoPub Batang 14 (reader,
  // reference default — CrossPointSettings::getReaderFontId()), RIDIBatang 14 kept
  // as the XTCKO extra face the web UI can select.
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

  bool noKern = false;
  ko::Spec spec;
  // Verification plumbing (host-only): layout manifest, plane dumps, page cap.
  std::string manifestPath;
  std::string planesDir;
  int maxPages = -1;
  std::string oracleRepo, oracleCommit, oracleBranch;

  ko::XtchWriter writer;
  // Output-mode flags, for verifying the 1-bit path (the web app sets the same mode
  // through ko_set_output_mode).
  // Images are dithered to 4 levels in 2-bit output and to 2 levels in 1-bit output in
  // every configuration. The text-AA switch (--text-aa / --no-text-aa) affects text
  // only: AA off means the grey passes draw no text greys at all, and in 1-bit output
  // it also stops the writer from thinning solid ink, so text stays crisp while image
  // greys are still halftoned.
  for (int i = 1; i < argc; i++) {
    const std::string flag = argv[i];
    if (flag == "--1bit") {
      writer.setMode(ko::XtcMode::Mono1Bit);
      spec.imageToneDepth = 2;   // 1-bit pages are dithered straight to 2 tones
    } else if (flag == "--tone-depth-2") {
      spec.imageToneDepth = 2;
    } else if (flag == "--image-dither" && i + 1 < argc) {
      spec.imageDither = std::atoi(argv[++i]);
    } else if (flag == "--image-dither-name" && i + 1 < argc) {
      spec.imageDither = ko::ditherModeFromName(argv[++i]);
    }
    else if (flag == "--no-text-aa") spec.textAntiAliasing = 0;
    else if (flag == "--text-aa") spec.textAntiAliasing = 1;
    else if (flag == "--no-kern") noKern = true;
    else if (flag == "--screen-margin" && i + 1 < argc) {
      // The reference reader's own knob: all four margins are a function of it.
      const int m = std::atoi(argv[++i]);
      if (!ko::geom::isScreenMarginAllowed(m)) {
        fprintf(stderr, "--screen-margin %d: reference allows %d..%d step %d\n", m,
                ko::geom::kScreenMarginMin, ko::geom::kScreenMarginMax, ko::geom::kScreenMarginStep);
        return 2;
      }
      spec.applyScreenMargin(m);
    } else if (flag == "--margin-bottom" && i + 1 < argc) {
      // Raw override, for isolating the status-bar lane alone. NOT a reference state:
      // the reference only ever produces the bottom margin via applyScreenMargin().
      spec.marginBottom = std::atoi(argv[++i]);
    } else if (flag == "--manifest" && i + 1 < argc) {
      manifestPath = argv[++i];
    } else if (flag == "--manifest-oracle" && i + 3 < argc) {
      // owner/repo  branch  commit — recorded so a stored manifest says which reference
      // revision it was produced under. Three separate values: no parsing ambiguity.
      oracleRepo = argv[++i];
      oracleBranch = argv[++i];
      oracleCommit = argv[++i];
    } else if (flag == "--dump-planes" && i + 1 < argc) {
      planesDir = argv[++i];
    } else if (flag == "--max-pages" && i + 1 < argc) {
      maxPages = std::atoi(argv[++i]);
    } else if (flag == "--font" && i + 1 < argc) {
      const std::string name = argv[++i];
      if (name == "kopub") spec.fontId = KOPUB_14_FONT_ID;
      else if (name == "ridibatang") spec.fontId = RIDIBATANG_14_FONT_ID;
      else { fprintf(stderr, "unknown font '%s' (kopub|ridibatang)\n", name.c_str()); return 2; }
    } else if (flag == "--kopub-external" && i + 1 < argc) {
      // §7 acceptance gate: register KoPub from a lossless EPD2 blob through the SAME parser the wasm
      // uses, instead of the embedded arrays. Any difference in the emitted pages is a parity failure.
      // Function-local statics on purpose: EpdFont/EpdFontFamily only point into the bundle, so it must
      // outlive the whole render pass.
      const std::string blobPath = argv[++i];
      FILE* bf = fopen(blobPath.c_str(), "rb");
      if (!bf) { fprintf(stderr, "cannot open %s\n", blobPath.c_str()); return 2; }
      fseek(bf, 0, SEEK_END);
      const long bsz = ftell(bf);
      fseek(bf, 0, SEEK_SET);
      static std::vector<uint8_t> blobBytes;
      blobBytes.resize(static_cast<size_t>(bsz));
      if (fread(blobBytes.data(), 1, blobBytes.size(), bf) != blobBytes.size()) { fclose(bf); return 2; }
      fclose(bf);
      static std::unique_ptr<ko::ExternalBuiltinFont> extBundle;
      std::string err;
      if (!ko::parseExternalFont(blobBytes.data(), blobBytes.size(), extBundle, err)) {
        fprintf(stderr, "EPD2 parse failed: %s\n", err.c_str());
        return 2;
      }
      static std::unique_ptr<EpdFont> extFont;
      static std::unique_ptr<EpdFontFamily> extFamily;
      extFont = std::make_unique<EpdFont>(&extBundle->data);
      extFamily = std::make_unique<EpdFontFamily>(extFont.get());
      if (noKern) {
        // Sensitivity control: zero the kern matrix IN PLACE (the pointer stays valid), isolating
        // kerning alone. A fixture that cannot detect this cannot certify the blob either.
        for (size_t k = 0; k < extBundle->kernMatrix.size(); ++k) extBundle->kernMatrix[k] = 0;
        fprintf(stderr, "kern matrix zeroed by --no-kern (control)\n");
      }
      renderer.insertFont(KOPUB_14_FONT_ID, extFamily.get());
      fprintf(stderr, "KoPub registered from blob %s (%zu bytes, glyphs %zu, kern %zu cells)\n",
              blobPath.c_str(), blobBytes.size(), extBundle->glyphs.size(),
              extBundle->kernMatrix.size());
    }
  }
  writer.setTextAa(spec.textAntiAliasing != 0);

  // Geometry last: every flag that can move a margin has been parsed, and the
  // viewport is always derived from the margins (never set on its own).
  spec.viewportWidth = static_cast<uint16_t>(renderer.getScreenWidth() - spec.marginLeft - spec.marginRight);
  spec.viewportHeight = static_cast<uint16_t>(renderer.getScreenHeight() - spec.marginTop - spec.marginBottom);
  fprintf(stderr, "viewport %ux%u (margins t%d r%d b%d l%d) font %d\n", spec.viewportWidth,
          spec.viewportHeight, spec.marginTop, spec.marginRight, spec.marginBottom, spec.marginLeft,
          spec.fontId);

  writer.setMetadata(driver.title(), "unknown", "", "ko");
  int totalPages = 0;
  std::vector<ko::XtchChapter> chapters;
  int chapterStart = 0;
  int spineCount = driver.spineCount();
  double tBuild = 0, tRender = 0, tWrite = 0;
  std::vector<ko::ManifestPage> manifestPages;
  const bool dumpPlanes = !planesDir.empty();
  if (dumpPlanes) mkdir(planesDir.c_str(), 0755);
  for (int spine = 0; spine < spineCount; spine++) {
    auto t0 = Clock::now();
    const int n = driver.buildSection(spine, spec);
    tBuild += msSince(t0);
    if (n < 0) { fprintf(stderr, "spine %d: build failed\n", spine); continue; }
    fprintf(stderr, "spine %d/%d: %d pages\n", spine, spineCount, n);
    for (int p = 0; p < n; p++) {
      if (maxPages >= 0 && p >= maxPages) break;
      ko::RenderedPage rp;
      ko::ManifestPage probe;
      t0 = Clock::now();
      if (!driver.renderPage(p, spec, rp, manifestPath.empty() ? nullptr : &probe, spine)) {
        fprintf(stderr, "  page %d failed\n", p);
        continue;
      }
      tRender += msSince(t0);
      if (!manifestPath.empty()) manifestPages.push_back(std::move(probe));
      t0 = Clock::now();
      writer.addPageFromPlanes(rp.bw, rp.lsb, rp.msb);
      tWrite += msSince(t0);
      if (dumpPlanes) {
        char base[512];
        snprintf(base, sizeof(base), "%s/p%05d_%05d", planesDir.c_str(), spine, p);
        ko::writeFile(std::string(base) + ".bw", std::string(rp.bw.begin(), rp.bw.end()));
        ko::writeFile(std::string(base) + ".lsb", std::string(rp.lsb.begin(), rp.lsb.end()));
        ko::writeFile(std::string(base) + ".msb", std::string(rp.msb.begin(), rp.msb.end()));
      }
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

  if (!manifestPath.empty()) {
    ko::ManifestHeader h;
    h.oracleRepo = oracleRepo;
    h.oracleBranch = oracleBranch;
    h.oracleCommit = oracleCommit;
    h.screenWidth = renderer.getScreenWidth();
    h.screenHeight = renderer.getScreenHeight();
    h.marginTop = spec.marginTop;
    h.marginRight = spec.marginRight;
    h.marginBottom = spec.marginBottom;
    h.marginLeft = spec.marginLeft;
    h.viewportWidth = spec.viewportWidth;
    h.viewportHeight = spec.viewportHeight;
    h.fontId = spec.fontId;
    h.lineCompression = spec.lineCompression;
    h.characterWrap = spec.characterWrap != 0;
    h.hyphenation = spec.hyphenationEnabled != 0;
    h.embeddedStyle = spec.embeddedStyle != 0;
    h.paragraphIndent = spec.paragraphIndent != 0;
    h.extraParagraphSpacing = spec.extraParagraphSpacing != 0;
    h.paragraphAlignment = spec.paragraphAlignment;
    h.imageRendering = spec.imageRendering;
    h.textAa = spec.textAntiAliasing != 0;
    if (!ko::writeFile(manifestPath, ko::serializeLayoutManifest(h, manifestPages))) {
      fprintf(stderr, "cannot write manifest %s\n", manifestPath.c_str());
      return 1;
    }
    fprintf(stderr, "MANIFEST %s (%zu pages)\n", manifestPath.c_str(), manifestPages.size());
  }
  return 0;
}
