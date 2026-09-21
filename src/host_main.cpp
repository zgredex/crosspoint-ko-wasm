// host_main.cpp — KO-fork EPUB → XTCH converter driver (host build).
// Replicates EpubReaderActivity's render pipeline headlessly using the shared
// ko::EngineDriver, then encodes pages with ko::XtchWriter into a real XTCH
// container (56B header + 256B metadata + chapters + index + XTH page data).
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <cctype>
#include <cstring>
#include <algorithm>   // std::sort, std::max_element (spine distribution)
#include <numeric>     // std::accumulate
#include <chrono>
#include <fstream>   // --external: range reads straight off the real file
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

// NEGATIVE CONTROL ONLY. `--drop-gray-planes` makes a 1-bit export skip the two gray passes, which is
// what an audit once proposed as an optimization on the theory that a 1-bit consumer reads only the BW
// plane. It does not: addMonoPage() turns grey pixels into ink dots and uses them to thin AA ink, and
// the preview compositor reads the same planes. The flag exists so scripts/verify/mono_planes_gate.py
// can DEMONSTRATE that dropping them changes the container; nothing in the product may set it.
static bool g_dropGrayPlanes = false;
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
            "          [--device x4|x3] (or --device=x4|x3)\n"
            "          [--orientation portrait|landscape-cw|inverted|landscape-ccw]\n"
            "          [--screen-margin N | --margin-bottom N]\n"
            "[--manifest PATH] [--dump-planes DIR] [--max-pages N] [--external] [--drop-gray-planes] [--read-helpers] [--fail-render S:P]\n",
            argv[0]);
    return 2;
  }
  const std::string epubPath = argv[1];
  const std::string outPath = argc > 2 ? argv[2] : "out.xtch";

  // `--external` is pre-scanned because it changes how the book is read AT ALL: the range-backed mount
  // must not be preceded by the very bulk read it exists to avoid, or the host would be measuring a
  // memory cost it never pays. Everything else still reads the file the ordinary way.
  bool preExternal = false;
  for (int i = 1; i < argc; i++) if (std::string(argv[i]) == "--external") preExternal = true;

  // CrossPoint-KO selects the X3 FreeInk profile before display.begin(), then
  // GfxRenderer::begin() reads the profile's runtime panel dimensions. Mirror
  // that ordering exactly; this cannot be a late canvas-size override.
  ko::DeviceProfile deviceProfile = ko::DeviceProfile::X4;
  for (int i = 1; i < argc; i++) {
    const std::string arg = argv[i];
    if ((arg == "--device" && i + 1 < argc) || arg.rfind("--device=", 0) == 0) {
      const std::string name = arg == "--device" ? argv[++i] : arg.substr(strlen("--device="));
      if (name == "x3" || name == "X3") deviceProfile = ko::DeviceProfile::X3;
      else if (name == "x4" || name == "X4") deviceProfile = ko::DeviceProfile::X4;
      else {
        fprintf(stderr, "unknown device '%s' (x4|x3)\n", name.c_str());
        return 2;
      }
    }
  }

  FILE* f = fopen(epubPath.c_str(), "rb");
  if (!f) { fprintf(stderr, "cannot open %s\n", epubPath.c_str()); return 1; }
  fseek(f, 0, SEEK_END);
  const long sz = ftell(f);
  fseek(f, 0, SEEK_SET);
  if (sz < 0) { fclose(f); fprintf(stderr, "cannot size %s\n", epubPath.c_str()); return 1; }
  const size_t fileSizeBytes = static_cast<size_t>(sz);
  std::vector<uint8_t> epubBytes;
  if (!preExternal) {
    epubBytes.resize(fileSizeBytes);
    if (fread(epubBytes.data(), 1, fileSizeBytes, f) != fileSizeBytes) { fclose(f); return 1; }
  }
  fclose(f);

  display.setDeviceProfile(deviceProfile);
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

  // --mount-only: isolate the mount from the parse, so "adopt instead of copy" can be measured for what
  // it is. Reports both, in this process, on the same bytes.
  for (int i = 1; i < argc; i++) {
    if (std::string(argv[i]) == "--mount-only") {
      std::vector<uint8_t> src(epubBytes.size());
      for (int rep = 0; rep < 4; rep++) {
        auto t0 = Clock::now();
        Storage.mountBlob("/a.epub", epubBytes.data(), epubBytes.size());
        const double copyMs = msSince(t0);
        Storage.remove("/a.epub");
        auto* p = static_cast<uint8_t*>(std::malloc(epubBytes.size()));
        if (!p) { fprintf(stderr, "oom\n"); return 1; }
        // the streaming path's write into the heap, which the adopt path also pays for
        auto t1 = Clock::now();
        std::memcpy(p, epubBytes.data(), epubBytes.size());
        const double fillMs = msSince(t1);
        Storage.mountOwnedBlob("/b.epub", p, epubBytes.size());
        const double adoptMs = msSince(t1);
        Storage.remove("/b.epub");
        fprintf(stderr, "rep %d: copy(alloc+memcpy) %.2f ms | fill+adopt %.2f ms (of which fill %.2f) | "
                        "storage now %zu bytes\n",
                rep, copyMs, adoptMs, fillMs, Storage.totalBytes());
      }
      return 0;
    }
  }

  ko::EngineDriver driver(renderer, display);
  auto tLoad0 = Clock::now();
  // --owned exercises the path the browser now takes: an allocation the storage adopts instead of
  // copying. Its bytes must be the same bytes, so the container is compared against the copied mount.
  bool useOwned = false;
  bool useExternal = false;
  bool readHelpers = false;   // --read-helpers: exercise the generic read helpers on an external mount
#ifdef KO_TEST_NEGATIVE_CONTROLS
  int g_failRenderSpine = -1; // --fail-render spine:page — deterministic page-failure injection
  int g_failRenderPage = -1;
#endif
  bool hrefSweep = false;
  for (int i = 1; i < argc; i++) {
    if (std::string(argv[i]) == "--owned") useOwned = true;
    if (std::string(argv[i]) == "--external") useExternal = true;
    if (std::string(argv[i]) == "--drop-gray-planes") g_dropGrayPlanes = true;
    if (std::string(argv[i]) == "--read-helpers") readHelpers = true;
#ifdef KO_TEST_NEGATIVE_CONTROLS
    if (std::string(argv[i]) == "--fail-render" && i + 1 < argc) {
      const std::string spec2 = argv[++i];
      const size_t colon = spec2.find(':');
      if (colon != std::string::npos) {
        g_failRenderSpine = std::atoi(spec2.substr(0, colon).c_str());
        g_failRenderPage = std::atoi(spec2.substr(colon + 1).c_str());
      }
    }
#endif
    if (std::string(argv[i]) == "--three-pass") driver.setThreePass(true);
    if (std::string(argv[i]) == "--spine-hrefs") hrefSweep = true;
  }
  bool loaded;
  if (useExternal) {
    // Range-backed mount: the file is NEVER read into memory. Every read the ZIP/OPF parser makes is
    // served from a 256 KiB aligned window fetched through this callback, so the byte-identity gate
    // exercises exactly the code path the browser's FileReaderSync bridge drives — without a browser.
    if (fileSizeBytes == 0) {
      fprintf(stderr, "empty file %s\n", epubPath.c_str());
      return 1;
    }
    // A real pread per window, on a handle kept open for the life of the mount.
    static std::ifstream reader;
    reader.open(epubPath, std::ios::binary);
    if (!reader) {
      fprintf(stderr, "cannot open %s for range reads\n", epubPath.c_str());
      return 1;
    }
    // KO_EXTERNAL_MAX_READ clamps how much one callback reads. A real reader may return a SHORT positive
    // read, and the storage must loop rather than treat the first positive result as the whole window
    // (it used to serve the gap as file content). external_gate.py uses a tiny clamp to prove that.
    static const size_t kMaxRead = [] {
      const char* v = std::getenv("KO_EXTERNAL_MAX_READ");
      const long n = v ? std::strtol(v, nullptr, 10) : 0;
      return n > 0 ? static_cast<size_t>(n) : static_cast<size_t>(1) << 40;
    }();
    auto pread = [](void* ctx, size_t offset, uint8_t* dst, size_t len) -> int {
      std::ifstream* f = static_cast<std::ifstream*>(ctx);
      if (len > kMaxRead) len = kMaxRead;           // short read, by request
      f->clear();                                   // a previous EOF must not poison the next read
      f->seekg(static_cast<std::streamoff>(offset), std::ios::beg);
      if (!*f) return -1;
      f->read(reinterpret_cast<char*>(dst), static_cast<std::streamsize>(len));
      const std::streamsize got = f->gcount();
      return got > 0 ? static_cast<int>(got) : -1;
    };
    loaded = driver.loadEpubFromExternal(fileSizeBytes, epubPath, pread, &reader);
    if (loaded && readHelpers) {
      // Exercise the three GENERIC read helpers against an external mount. They used to read Blob::data
      // directly, which is nullptr for an external Blob by design — a null dereference waiting for the
      // first caller. This is the host's way of covering them; the size is what matters, not the bytes.
      char buf[1024];
      const size_t got = Storage.readFileToBuffer(epubPath.c_str(), buf, sizeof(buf), 0);
      const HalFile f = Storage.open(epubPath.c_str(), O_RDONLY);
      const String whole = Storage.readFile(epubPath.c_str());
      std::vector<uint8_t> sink;
      struct Collect : Print {
        std::vector<uint8_t>* out;
        explicit Collect(std::vector<uint8_t>* o) : out(o) {}
        size_t write(const uint8_t* p, size_t n) override {
          out->insert(out->end(), p, p + n);
          return n;
        }
        size_t write(uint8_t c) override { out->push_back(c); return 1; }
      };
      Collect collect(&sink);
      const bool streamed = Storage.readFileToStream(epubPath.c_str(), collect, 512);
      fprintf(stderr,
              "READ_HELPERS buffer=%zu fileSize=%zu whole=%zu streamed=%zu streamBytes=%zu\n",
              got, f.size(), whole.length(), streamed ? 1u : 0u, sink.size());
    }
  } else if (useOwned) {
    auto* owned = static_cast<uint8_t*>(std::malloc(epubBytes.size()));
    if (!owned) {
      fprintf(stderr, "out of memory for the owned mount\n");
      return 1;
    }
    std::memcpy(owned, epubBytes.data(), epubBytes.size());
    // Ownership is consumed by the call REGARDLESS of the outcome: mountOwnedBlob installs the Blob whose
    // deleter is std::free, and a failed openEpub() removes that mount. The `if (!loaded) std::free(owned)`
    // that used to be here was therefore a double free on malformed input — exactly the case a
    // malformed-input test exercises.
    loaded = driver.loadEpubFromOwnedBlob(owned, epubBytes.size(), epubPath);
  } else {
    loaded = driver.loadEpubFromBlob(epubBytes.data(), epubBytes.size(), epubPath);
  }
  if (!loaded) {
    fprintf(stderr, "Epub::load failed\n");
    return 1;
  }
  const double tLoad = msSince(tLoad0);
  fprintf(stderr, "loaded: title='%s' spines=%d  [load %.1f ms]\n", driver.title().c_str(),
          driver.spineCount(), tLoad);
  if (useExternal) {
    // The number that decides whether this is worth shipping: physical bytes against file size. A mount
    // that reads the whole archive has gained nothing over the bulk path and should not be enabled.
    const ExternalStats& es = externalStats();
    fprintf(stderr,
            "EXTERNAL file=%lld bytes, crossings=%zu, physical=%zu (%.1f%% of file), ram reads=%zu, "
            "read %.1f ms\n",
            static_cast<long long>(fileSizeBytes), es.calls, es.bytes,
            fileSizeBytes == 0 ? 0.0
                              : 100.0 * static_cast<double>(es.bytes) / static_cast<double>(fileSizeBytes),
            es.hits, es.readMs);
  }

  if (hrefSweep) {
    // Read every spine href, then read them AGAIN after stack-churning work. spineHref() used to return a
    // reference into a by-value temporary (BookMetadataCache::SpineEntry returns by value), so later reads
    // came out of a dead frame and produced nondeterministic garbage like "䏆". Two independent reads that
    // must agree, plus a charset check, is the strongest regression this host can run on the accessor.
    //
    // HONEST SCOPE — this sweep does NOT discriminate the bug. Measured with the reference form
    // reinstated: `SPINE_HREFS ok — 3/10 spines, 0 differing/ill-formed` on every fixture. Row A of
    // tools/spine_href_repro.cpp is the explanation: reading the href immediately after the call copies
    // out of the poisoned slot while it is still intact, and that is exactly what the loop below does.
    // The bug needs an intervening call to land on the dead frame, which is a real program's normal
    // state (title retrieval, an allocation, a message loop) but never this loop's.
    //
    // So the discriminating evidence for the fix is scripts/verify/spine_href_gate.py: it asserts the
    // accessor's signature and compiles the shape at -O0 and -O3, where one intervening call corrupts
    // the href. ASAN is NOT an option on this host — a trivial `-fsanitize=address` program hangs
    // (verified), so no stack-use-after-return report exists to point at.
    const int hn = driver.spineCount();
    std::vector<std::string> first;
    first.reserve(hn);
    for (int i = 0; i < hn; i++) first.push_back(driver.spineHref(i));

    volatile size_t churn = 0;
    for (int i = 0; i < hn; i++) {
      std::string scratch(64, 'x');
      std::string a = driver.spineHref(i), b = driver.spineHref(i), c = driver.spineHref(i);
      churn += scratch.size() + a.size() + b.size() + c.size();
    }
    (void)churn;

    int bad = 0;
    for (int i = 0; i < hn; i++) {
      const std::string again = driver.spineHref(i);
      bool ok = !again.empty() && again == first[i];
      for (char c : again) {
        if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '.' || c == '_' || c == '-' || c == '/')) {
          ok = false;
        }
      }
      if (!ok) {
        if (bad < 3) {
          printf("  SPINE_HREF_BAD %d: \"%s\" (first read \"%s\")\n", i, again.c_str(), first[i].c_str());
        }
        bad++;
      }
    }
    printf("SPINE_HREFS %s — %d spines, %d differing/ill-formed\n", bad ? "FAIL" : "ok", hn, bad);
    if (bad) return 1;
  }

  bool noKern = false;
  ko::Spec spec;
  spec.deviceProfile = deviceProfile;
  // Verification plumbing (host-only): layout manifest, plane dumps, page cap.
  std::string manifestPath;
  std::string planesDir;
  int maxPages = -1;
  std::string oracleRepo, oracleCommit, oracleBranch;

  ko::XtchWriter writer;
  writer.setDeviceProfile(deviceProfile);
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
    } else if (flag == "--image-rendering" && i + 1 < argc) {
      // 0 = show, 1 = placeholder, 2 = hidden (the UI's image handling). Hidden is how the DECODE cost
      // of an image page is isolated from everything else the page does.
      spec.imageRendering = static_cast<uint8_t>(std::atoi(argv[++i]));
    } else if (flag == "--image-dither" && i + 1 < argc) {
      spec.imageDither = std::atoi(argv[++i]);
    } else if (flag == "--image-dither-name" && i + 1 < argc) {
      spec.imageDither = ko::ditherModeFromName(argv[++i]);
    }
    else if (flag == "--no-text-aa") spec.textAntiAliasing = 0;
    else if (flag == "--text-aa") spec.textAntiAliasing = 1;
    else if (flag == "--no-kern") noKern = true;
    else if (flag == "--landscape-cw") spec.applyOrientation(ko::LANDSCAPE_CW);
    else if (flag == "--landscape-ccw") spec.applyOrientation(ko::LANDSCAPE_CCW);
    else if (flag == "--portrait-inverted") spec.applyOrientation(ko::PORTRAIT_INVERTED);
    else if (flag == "--orientation" && i + 1 < argc) {
      const std::string name = argv[++i];
      if (name == "portrait") spec.applyOrientation(ko::PORTRAIT);
      else if (name == "landscape-cw") spec.applyOrientation(ko::LANDSCAPE_CW);
      else if (name == "inverted" || name == "portrait-inverted") spec.applyOrientation(ko::PORTRAIT_INVERTED);
      else if (name == "landscape-ccw") spec.applyOrientation(ko::LANDSCAPE_CCW);
      else {
        fprintf(stderr, "unknown orientation '%s' (portrait|landscape-cw|inverted|landscape-ccw)\n",
                name.c_str());
        return 2;
      }
    }
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
  ko::applyReaderOrientation(renderer, spec.orientation);
  spec.viewportWidth = static_cast<uint16_t>(renderer.getScreenWidth() - spec.marginLeft - spec.marginRight);
  spec.viewportHeight = static_cast<uint16_t>(renderer.getScreenHeight() - spec.marginTop - spec.marginBottom);
  fprintf(stderr, "device %s physical %ux%u portrait %ux%u; viewport %ux%u "
                  "(margins t%d r%d b%d l%d) font %d\n",
          ko::deviceGeometry(deviceProfile).name, display.getDisplayWidth(), display.getDisplayHeight(),
          ko::deviceGeometry(deviceProfile).portraitWidth, ko::deviceGeometry(deviceProfile).portraitHeight,
          spec.viewportWidth,
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
  std::vector<double> spineMs;
  std::vector<int> spinePages;
  struct PoolSpine {
    std::vector<uint8_t> flat;
    std::vector<uint32_t> off, len;
    struct Anchor { std::string title; int localPage; };
    std::vector<Anchor> toc;
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
    // Same policy as a failed page: a spine that will not build is a failure of the run, not a spine to
    // leave out of the container. Skipping it silently produced a book missing a whole chapter.
    if (n < 0) { fprintf(stderr, "FATAL: spine %d build failed\n", spine); return 3; }
    fprintf(stderr, "spine %d/%d: %d pages\n", spine, spineCount, n);
    PoolSpine ps;                       // only filled when --pool
    ko::XtchWriter local(writer.mode(), deviceProfile);
    local.setTextAa(spec.textAntiAliasing != 0);
    int renderedThisSpine = 0;
    for (int p = 0; p < n; p++) {
      if (maxPages >= 0 && p >= maxPages) break;
      ko::RenderedPage rp;
      ko::ManifestPage probe;
      t0 = Clock::now();
      // Gray planes are always rendered (see g_dropGrayPlanes): a 1-bit container's writer consumes
      // them, so `monoOnly` is only ever true from the negative-control flag.
      // The rejected gray-plane-skipping state is only reachable through the test-only entry point, which
      // the product module does not compile (KO_TEST_NEGATIVE_CONTROLS is host-only).
      const bool dropGray = g_dropGrayPlanes && writer.mode() == ko::XtcMode::Mono1Bit;
      // Fail the run, do not skip the page. This binary certifies containers: accepting a page loss here
      // would let it certify a book that the device would render with a hole in it.
      const bool rendered =
          dropGray ? driver.renderPageDroppingGrayForTest(p, spec, rp, manifestPath.empty() ? nullptr : &probe, spine)
                   : driver.renderPage(p, spec, rp, manifestPath.empty() ? nullptr : &probe, spine);
      if (!rendered) {
        fprintf(stderr, "FATAL: render failed at spine %d page %d\n", spine, p);
        return 3;
      }
#ifdef KO_TEST_NEGATIVE_CONTROLS
      // Deterministic page-failure injection for the negative control: "spine:page". Proving that export
      // fails on a page it cannot render needs a failure the test can place exactly, not an accidentally
      // broken fixture.
      if (g_failRenderSpine == spine && g_failRenderPage == p) {
        fprintf(stderr, "INJECTED FAILURE at spine %d page %d\n", spine, p);
        return 3;
      }
#endif
      tRender += msSince(t0);
      if (!manifestPath.empty()) manifestPages.push_back(std::move(probe));
      t0 = Clock::now();
      if (!writer.addPageFromPlanes(rp.bw, rp.lsb, rp.msb)) {
        fprintf(stderr, "page encoder refused spine %d page %d\n", spine, p);
        return 1;
      }
      tWrite += msSince(t0);
      if (pooled && !local.addPageFromPlanes(rp.bw, rp.lsb, rp.msb)) {
        fprintf(stderr, "local page encoder refused spine %d page %d\n", spine, p);
        return 1;
      }
      if (dumpPlanes) {
        char base[512];
        snprintf(base, sizeof(base), "%s/p%05d_%05d", planesDir.c_str(), spine, p);
        ko::writeFile(std::string(base) + ".bw", std::string(rp.bw.begin(), rp.bw.end()));
        ko::writeFile(std::string(base) + ".lsb", std::string(rp.lsb.begin(), rp.lsb.end()));
        ko::writeFile(std::string(base) + ".msb", std::string(rp.msb.begin(), rp.msb.end()));
      }
      totalPages++;
      renderedThisSpine++;
      if (totalPages % 25 == 0) fprintf(stderr, "  ...%d\n", totalPages);
    }
    std::vector<PoolSpine::Anchor> resolvedChapterAnchors;
    if (renderedThisSpine > 0) {
      // Candidates in SPINE order, then TOC order within the spine — the order the product's export
      // accumulates them in, and the order the pooled assembler must reproduce.
      for (int t = 0; t < driver.tocCount(); t++) {
        if (driver.tocSpine(t) != spine) continue;
        const std::string anchor = driver.tocAnchor(t);
        int local_ = anchor.empty() ? -1 : driver.anchorLocalPage(anchor);
        if (local_ < 0 || local_ >= renderedThisSpine) local_ = 0;
        const std::string chapterTitle = ko::normalizeChapterTitle(driver.tocTitle(t));
        if (ko::retainChapterCandidate(
                tocCandidates,
                {chapterTitle, static_cast<uint32_t>(chapterStart + local_)})) {
          if (resolvedChapterAnchors.size() < ko::MAX_EXPORTED_CHAPTERS) {
            resolvedChapterAnchors.push_back({chapterTitle, local_});
          }
        }
      }
      chapterStart += renderedThisSpine;
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
      // Resolved only from the EPUB navigation document while this spine's
      // anchor map is live.
      ps.toc = resolvedChapterAnchors;
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
    ko::XtchWriter asmW(writer.mode(), deviceProfile);
    asmW.setTextAa(spec.textAntiAliasing != 0);
    asmW.adoptMetadataFrom(writer);
    std::vector<ko::ChapterCandidate> cands;
    int base = 0;
    for (const PoolSpine& ps : pool) {
      if (ps.pages <= 0) continue;
      for (int i = 0; i < ps.pages; i++) {
        if (!asmW.addRawPage(ps.flat.data() + ps.off[static_cast<size_t>(i)], ps.len[static_cast<size_t>(i)])) {
          fprintf(stderr, "pool: refused page %d of the spine at base %d\n", i, base);
          return 1;
        }
      }
      for (const PoolSpine::Anchor& a : ps.toc) {
        ko::retainChapterCandidate(
            cands, {a.title, static_cast<uint32_t>(base + a.localPage)});
      }
      base += ps.pages;
    }
    const uint32_t total = static_cast<uint32_t>(asmW.pageCount());
    std::vector<ko::XtchChapter> asmChapters = ko::buildChapters(cands, total);
    std::vector<uint8_t> pooledOut = asmW.finish(asmChapters);
    fprintf(stderr, "POOL     %d spines, %u pages assembled, %zu chapters\n",
            static_cast<int>(pool.size()), total, asmChapters.size());
    out_override = std::move(pooledOut);
  }

  // One chapter table for both paths: the shared builder, fed in spine order.
  chapters = ko::buildChapters(tocCandidates, static_cast<uint32_t>(totalPages));
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
    h.deviceProfile = ko::deviceGeometry(deviceProfile).name;
    h.physicalWidth = display.getDisplayWidth();
    h.physicalHeight = display.getDisplayHeight();
    h.planeBytes = display.getBufferSize();
    h.screenWidth = renderer.getScreenWidth();
    h.screenHeight = renderer.getScreenHeight();
    h.orientation = spec.orientation;
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
