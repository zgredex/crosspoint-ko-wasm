#pragma once

#include <chrono>
#include <cstdint>

// Port instrumentation: where an image page actually spends its time, split so that a conclusion like
// "the decoder is the bottleneck" is measured rather than inferred. The port previously had a timer
// labelled as decode that began AFTER the scanline loop, i.e. it measured draw — and the
// hidden-image comparison ("normal page minus hidden image") measures everything the image costs,
// not the codec alone.
//
// Filled by the JPEG and PNG converters, reset once per render, read by the port's wasm API and
// reported to the browser. Harmless in any build: a few doubles and a counter, no behaviour change.
namespace ko {

struct ImagePerf {
  double readMs = 0;     // image bytes fetched out of storage (or inflated out of the ZIP)
  double headerMs = 0;   // header parse + sampling geometry
  double decodeMs = 0;   // the codec's own work: scanlines for JPEG, inflate+unfilter for PNG
  double drawMs = 0;     // scale + dither + framebuffer writes
  uint32_t decodes = 0;  // how many times an image was DECODED during this render
  uint32_t images = 0;   // how many image elements were rendered
};

inline ImagePerf& imagePerf() {
  static ImagePerf perf;
  return perf;
}

inline void imagePerfReset() { imagePerf() = ImagePerf{}; }

// Monotonic, sub-millisecond, and available in both the host and wasm builds.
inline double perfNowMs() {
  using clock = std::chrono::steady_clock;
  return std::chrono::duration<double, std::milli>(clock::now().time_since_epoch()).count();
}

}  // namespace ko
