// host_main.cpp — KO-fork EPUB → XTCH converter driver (host build).
// Replicates EpubReaderActivity's render pipeline headlessly using the shared
// ko::EngineDriver, then encodes pages with ko::XtchWriter into a real XTCH
// container (56B header + 256B metadata + chapters + index + XTH page data).
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <algorithm>   // std::sort, std::max_element (spine distribution)
#include <numeric>     // std::accumulate
#include <chrono>
#include <memory>
#include <string>
#include <sys/stat.h>
#include <vector>

#include "ko_engine_driver.h"
#include "external_font_loader.h"   // §7 gate: register KoPub from an EPD2 blob
#include "xtch_writer.h"
#include "xtch_chapters.h"

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
            "          [--text-aa|--no-text-aa] [--font kopub|ridibatang] [--kopub-external blob]\n"
            "          [--external-font kopub|ridibatang blob] [--no-kern]\n"
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

  // Fonts: the READER faces only. KoPub Batang 14 is the reference default
  // (CrossPointSettings::getReaderFontId()); RIDIBatang 14 is the XTCKO extra face the web UI can
  // select.
  //
  // Pretendard 10 used to be registered here under the UI font ids, mirroring the device's
  // main.cpp. It is NOT needed to render a page and has been removed:
  //   * `setFallbackFont(UI_FONT_ID)` in the reference is a FONT-ID-level fallback — consulted only
  //     when a requested font id is not registered (GfxRenderer::getEffectiveFontId). KoPub is
  //     always registered, so it never fires for a page render.
  //   * the only GLYPH-level fallback in the reference is
  //     `setGlyphFallback(SYSTEM_FONT_ID, UI_FONT_ID)` (main.cpp:162), which backs an SD-card
  //     system font. That is a device path, not a reading path.
  // So a codepoint KoPub lacks is drawn as nothing on the device too — see
  // docs/ko-font-payload-measurement.md for the codepoint census behind that claim.
  EpdFont kopub14(&kopub_14_regular);
  EpdFontFamily kopubFamily(&kopub14);
  EpdFont ridibatang14(&ridibatang_14_regular);
  EpdFontFamily ridibatangFamily(&ridibatang14);
  renderer.insertFont(KOPUB_14_FONT_ID, &kopubFamily);
  renderer.insertFont(RIDIBATANG_14_FONT_ID, &ridibatangFamily);

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
    } else if ((flag == "--kopub-external" || flag == "--external-font") && i + 1 < argc) {
      // §7 acceptance gate: register a built-in face from a lossless EPD2 blob through the SAME
      // parser the wasm uses, instead of the embedded arrays. Any difference in the emitted pages
      // is a parity failure.
      //
      //   --kopub-external <blob>        the face this gate has always used (alias)
      //   --external-font <face> <blob>  face = kopub | ridibatang
      //
      // The face argument exists because the container is generic: RIDIBatang is the optional face a
      // default KoPub build need not carry, and its externalized path has to be testable too.
      // Function-local statics on purpose: EpdFont/EpdFontFamily only point into the bundle, so it
      // must outlive the whole render pass.
      std::string faceName = "kopub";
      if (flag == "--external-font") {
        faceName = argv[++i];
        if (i + 1 >= argc) { fprintf(stderr, "--external-font needs <face> <blob>\n"); return 2; }
      }
      int externalFaceId = 0;
      if (faceName == "kopub") externalFaceId = KOPUB_14_FONT_ID;
      else if (faceName == "ridibatang") externalFaceId = RIDIBATANG_14_FONT_ID;
      else { fprintf(stderr, "unknown face '%s' (kopub|ridibatang)\n", faceName.c_str()); return 2; }
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
      renderer.insertFont(externalFaceId, extFamily.get());
      fprintf(stderr, "%s registered from blob %s (%zu bytes, glyphs %zu, kern %zu cells, is2Bit %d)\n",
              faceName.c_str(), blobPath.c_str(), blobBytes.size(), extBundle->glyphs.size(),
              extBundle->kernMatrix.size(), extBundle->data.is2Bit ? 1 : 0);
      if (spec.fontId != externalFaceId) {
        fprintf(stderr, "note: --font is not %s, so the externalized face will not be the one "
                        "rendered; pass --font %s as well\n", faceName.c_str(), faceName.c_str());
      }
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
  // --pool: the reference implementation of the JS pool coordinator, in one process. Each spine is
  // laid out, rendered and encoded on its own into a LOCAL writer; page records are then appended to
  // an assembler in spine order and the container is written once. Comparing this against the serial
  // run is the whole point: same page records, same chapter logic (src/xtch_chapters.h), same bytes.
  // --chunk N: build each spine incrementally (N pages at a time) instead of one-shot. The pages,
  // chapter anchors and container must come out identical — this is how the incremental path is gated
  // against the one-shot path it replaces.
  int chunkPages = 0;
  for (int i = 1; i < argc; i++) {
    if (std::string(argv[i]) == "--chunk" && i + 1 < argc) chunkPages = std::atoi(argv[i + 1]);
  }

  const bool pooled = [] (int argc, char** argv) {
    for (int i = 1; i < argc; i++) if (std::string(argv[i]) == "--pool") return true;
    return false;
  } (argc, argv);

  std::vector<uint8_t> out_override;   // pooled assembly, when --pool
  std::vector<ko::ChapterCandidate> tocCandidates;
  std::vector<ko::XtchChapter> spineFallback;
  std::vector<double> spineMs;
  std::vector<int> spinePages;
  struct PoolSpine {
    std::vector<uint8_t> flat;
    std::vector<uint32_t> off, len;
    struct Anchor { std::string title; int localPage; };
    std::vector<Anchor> toc;
    std::string fallback;
    int pages = 0;
  };
  std::vector<PoolSpine> pool;

  for (int spine = 0; spine < spineCount; spine++) {
    const auto tSpine0 = Clock::now();
    auto t0 = Clock::now();
    int n;
    if (chunkPages > 0) {
      n = driver.startSection(spine, spec, chunkPages);
      while (n >= 0 && !driver.sectionBuildComplete()) {
        const int more = driver.buildSectionMore(chunkPages);
        if (more < 0) { n = -1; break; }
        n = more;
      }
    } else {
      n = driver.buildSection(spine, spec);
    }
    tBuild += msSince(t0);
    if (n < 0) { fprintf(stderr, "spine %d: build failed\n", spine); continue; }
    fprintf(stderr, "spine %d/%d: %d pages\n", spine, spineCount, n);
    PoolSpine ps;                       // only filled when --pool
    ko::XtchWriter local(writer.mode());
    local.setTextAa(spec.textAntiAliasing != 0);
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
      if (pooled) local.addPageFromPlanes(rp.bw, rp.lsb, rp.msb);
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
      // Candidates in SPINE order, then TOC order within the spine — the order the product's export
      // accumulates them in, and the order the pooled assembler must reproduce.
      for (int t = 0; t < driver.tocCount(); t++) {
        if (driver.tocSpine(t) != spine) continue;
        const std::string anchor = driver.tocAnchor(t);
        int local_ = anchor.empty() ? -1 : driver.anchorLocalPage(anchor);
        if (local_ < 0 || local_ >= n) local_ = 0;
        tocCandidates.push_back({driver.tocTitle(t),
                                 static_cast<uint32_t>(chapterStart + local_)});
      }
      std::string href = driver.spineHref(spine);
      const size_t slash = href.find_last_of('/');
      if (slash != std::string::npos) href = href.substr(slash + 1);
      const size_t dot = href.find_last_of('.');
      if (dot != std::string::npos) href = href.substr(0, dot);
      ko::XtchChapter ch;
      ch.name = href.empty() ? ("Chapter " + std::to_string(spine + 1)) : href;
      ch.startPage = static_cast<uint16_t>(chapterStart);
      ch.endPage = static_cast<uint16_t>(chapterStart + n - 1);
      spineFallback.push_back(ch);
      chapterStart += n;
    }
    if (pooled) {
      const int made = static_cast<int>(local.pageCount());
      if (made != n - (maxPages >= 0 && n > maxPages ? n - maxPages : 0) && maxPages < 0) {
        fprintf(stderr, "pool: spine %d encoded %d pages but rendered %d\n", spine, made, n);
      }
      ps.pages = made;
      for (int i = 0; i < made; i++) {
        const std::vector<uint8_t>& rec = local.page(static_cast<size_t>(i));
        ps.off.push_back(static_cast<uint32_t>(ps.flat.size()));
        ps.len.push_back(static_cast<uint32_t>(rec.size()));
        ps.flat.insert(ps.flat.end(), rec.begin(), rec.end());
      }
      // anchors, resolved while this spine's section is the built one
      for (int t = 0; t < driver.tocCount(); t++) {
        if (driver.tocSpine(t) != spine) continue;
        const std::string anchor = driver.tocAnchor(t);
        int local_ = anchor.empty() ? -1 : driver.anchorLocalPage(anchor);
        if (local_ < 0 || local_ >= made) local_ = 0;
        ps.toc.push_back({driver.tocTitle(t), local_});
      }
      {   // fallback name, exactly as the serial path derives it
        std::string href = driver.spineHref(spine);
        const size_t slash = href.find_last_of('/');
        if (slash != std::string::npos) href = href.substr(slash + 1);
        const size_t dot = href.find_last_of('.');
        if (dot != std::string::npos) href = href.substr(0, dot);
        ps.fallback = href.empty() ? ("Chapter " + std::to_string(spine + 1)) : href;
      }
      pool.push_back(std::move(ps));
    }
    spineMs.push_back(msSince(tSpine0));
    spinePages.push_back(n);
  }
  // Spine distribution. Export walks spines in order on one engine, so this is the shape a spine pool
  // would have to schedule around: even spines scale linearly with workers, a long tail does not.
  if (!spineMs.empty()) {
    std::vector<double> sorted = spineMs;
    std::sort(sorted.begin(), sorted.end());
    const auto pick = [&sorted](double q) {
      return sorted[std::min(sorted.size() - 1, static_cast<size_t>(q * sorted.size()))];
    };
    const double sum = std::accumulate(spineMs.begin(), spineMs.end(), 0.0);
    size_t heaviest = std::max_element(spineMs.begin(), spineMs.end()) - spineMs.begin();
    // Attainable speedup with N workers is sum / makespan, and makespan is at least the heaviest
    // single spine however many workers you add — so a long tail caps the whole thing. (An earlier
    // version printed sum/(sum/8), which is 8.00 by algebra and told nobody anything.)
    const auto speedup = [&](int n) {
      const double makespan = std::max(sum / n, sorted.back());
      return sum / makespan;
    };
    fprintf(stderr,
            "SPINES   %zu spines | min %.1f p50 %.1f p90 %.1f max %.1f ms | sum %.1f | "
            "heaviest spine %zu is %.0f%% of spine time | ceiling %.2fx (4w) %.2fx (8w) %.2fx (16w)\n",
            spineMs.size(), sorted.front(), pick(0.5), pick(0.9), sorted.back(), sum,
            heaviest, 100.0 * spineMs[heaviest] / sum, speedup(4), speedup(8), speedup(16));
    if (const char* dump = std::getenv("KO_SPINE_DUMP")) {
      if (std::string(dump) == "1") {
        for (size_t i = 0; i < spineMs.size(); i++)
          fprintf(stderr, "SPINE %zu %d pages %.1f ms\n", i, spinePages[i], spineMs[i]);
      }
    }
  }
  fprintf(stderr, "rendered %d pages; finalizing container\n", totalPages);
  fprintf(stderr,
          "PROFILE  buildSection(parse+paginate) %.1f ms | renderPage(glyphs+quantize) %.1f ms"
          " | writer %.1f ms | total %.1f ms | %.2f ms/page\n",
          tBuild, tRender, tWrite, tBuild + tRender + tWrite,
          totalPages ? (tBuild + tRender + tWrite) / totalPages : 0.0);

  auto tFin0 = Clock::now();
  // Pooled: assemble from the per-spine records. The chapter logic is the SHARED buildChapters() the
  // serial finalizer uses, and candidates are added in spine order — worker completion order must not
  // reach this code, which in the host means plain spine order.
  if (pooled) {
    ko::XtchWriter asmW(writer.mode());
    asmW.setTextAa(spec.textAntiAliasing != 0);
    asmW.adoptMetadataFrom(writer);
    std::vector<ko::ChapterCandidate> cands;
    std::vector<ko::XtchChapter> fallback;
    int base = 0;
    for (const PoolSpine& ps : pool) {
      if (ps.pages <= 0) continue;
      for (int i = 0; i < ps.pages; i++) {
        if (!asmW.addRawPage(ps.flat.data() + ps.off[static_cast<size_t>(i)], ps.len[static_cast<size_t>(i)])) {
          fprintf(stderr, "pool: refused page %d of the spine at base %d\n", i, base);
          return 1;
        }
      }
      for (const PoolSpine::Anchor& a : ps.toc) cands.push_back({a.title, static_cast<uint32_t>(base + a.localPage)});
      ko::XtchChapter ch;
      ch.name = ps.fallback;
      ch.startPage = static_cast<uint16_t>(base);
      ch.endPage = static_cast<uint16_t>(base + ps.pages - 1);
      fallback.push_back(ch);
      base += ps.pages;
    }
    const uint32_t total = static_cast<uint32_t>(asmW.pageCount());
    std::vector<ko::XtchChapter> asmChapters = ko::buildChapters(cands, fallback, total);
    std::vector<uint8_t> pooledOut = asmW.finish(asmChapters);
    fprintf(stderr, "POOL     %d spines, %u pages assembled, %zu chapters\n",
            static_cast<int>(pool.size()), total, asmChapters.size());
    out_override = std::move(pooledOut);
  }

  // One chapter table for both paths: the shared builder, fed in spine order.
  chapters = ko::buildChapters(tocCandidates, spineFallback, static_cast<uint32_t>(totalPages));
  std::vector<uint8_t> out = out_override.empty() ? writer.finish(chapters) : std::move(out_override);
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
