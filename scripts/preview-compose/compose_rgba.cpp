// Preview compose: three packed 1-bpp planes -> 480x800 RGBA.
//
// Stage 1 of the WASM port: this file is standalone and native-testable so the algorithm
// can be proven byte-identical to the frozen JS reference BEFORE it goes anywhere near the
// engine. The same function body moves into src/wasm_api.cpp in stage 2.
//
// Layout (identical to the JS reference and to the file's own packing):
//   plane  = 100 bytes per physical row x 480 rows = 48,000 bytes
//   logical(x,y) <- physical(phyX = y, phyY = 479 - x)
//   idx = phyY * 100 + (phyX >> 3),  bit = 7 - (phyX & 7)
//   v = !ink ? 0 : lsb ? 1 : msb ? 2 : 3   with ink = (bit == 0)
//
// Why the 8-bit grouping: for a fixed byte column c, the eight byte offsets
// phyX = 8c+0 .. 8c+7 live in EIGHT BITS OF THE SAME BYTE. Processing y in groups of eight
// therefore turns one strided fetch into eight logical rows, cutting strided reads 8x
// (3 x 384,000 -> 3 x 48,000) while every output row is still written densely. The measured
// 1.29x ceiling of the optimised JS came from exactly this strided gather, so this is the
// fix rather than a relocation.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <vector>

static constexpr int kW = 480;
static constexpr int kH = 800;
static constexpr int kColBytes = 100;
static constexpr size_t kPlaneBytes = static_cast<size_t>(kColBytes) * 480;  // 48,000
static constexpr size_t kRgbaBytes = static_cast<size_t>(kW) * kH * 4;       // 1,536,000

// Packed little-endian RGBA words for the four panel shades, indexed by v.
static constexpr uint32_t kGray32[4] = {0xFFFFFFFFu, 0xFF808080u, 0xFFCDCDCDu, 0xFF000000u};
// Indexed by the plane bit: 0 = black (bit 0 is black on the device), 1 = white.
static constexpr uint32_t kMono32[2] = {0xFF000000u, 0xFFFFFFFFu};

// lsb/msb are ignored when mono != 0 and may be null.
static void composeRgba(const uint8_t* bw, const uint8_t* lsb, const uint8_t* msb, int mono, uint32_t* out) {
  for (int c = 0; c < kColBytes; ++c) {
    for (int phyY = 0; phyY < 480; ++phyY) {
      const size_t idx = static_cast<size_t>(phyY) * kColBytes + c;
      const uint8_t bwByte = bw[idx];
      const uint8_t lsbByte = lsb ? lsb[idx] : 0;
      const uint8_t msbByte = msb ? msb[idx] : 0;
      const int x = 479 - phyY;  // logical column for this physical row
      for (int b = 0; b < 8; ++b) {
        const int y = c * 8 + b;      // logical row
        const int shift = 7 - b;      // bit position inside the physical byte
        const int bit = (bwByte >> shift) & 1;
        uint32_t px;
        if (mono) {
          px = kMono32[bit];
        } else if (bit != 0) {
          px = kGray32[0];            // !ink -> white
        } else if (((lsbByte >> shift) & 1) == 1) {
          px = kGray32[1];            // dark grey
        } else if (((msbByte >> shift) & 1) == 1) {
          px = kGray32[2];            // light grey
        } else {
          px = kGray32[3];            // black
        }
        out[static_cast<size_t>(y) * kW + x] = px;
      }
    }
  }
}

// The trivial reference: same contract, one pixel at a time, no grouping. Used only to
// self-check the grouped loop inside this harness.
static void composeRgbaNaive(const uint8_t* bw, const uint8_t* lsb, const uint8_t* msb, int mono, uint32_t* out) {
  for (int y = 0; y < kH; ++y) {
    for (int x = 0; x < kW; ++x) {
      const int phyX = y;
      const int phyY = 479 - x;
      const size_t idx = static_cast<size_t>(phyY) * kColBytes + (phyX >> 3);
      const int shift = 7 - (phyX & 7);
      const int bit = (bw[idx] >> shift) & 1;
      uint32_t px;
      if (mono) {
        px = kMono32[bit];
      } else if (bit != 0) {
        px = kGray32[0];
      } else if (((lsb[idx] >> shift) & 1) == 1) {
        px = kGray32[1];
      } else if (((msb[idx] >> shift) & 1) == 1) {
        px = kGray32[2];
      } else {
        px = kGray32[3];
      }
      out[static_cast<size_t>(y) * kW + x] = px;
    }
  }
}

static void fillRandom(std::vector<uint8_t>& v, uint32_t& state) {
  for (auto& b : v) {
    state = state * 1664525u + 1013904223u;
    b = static_cast<uint8_t>(state >> 24);
  }
}

static inline double nowMs() {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

static int runBench() {
  std::vector<uint8_t> bw(kPlaneBytes), lsb(kPlaneBytes), msb(kPlaneBytes);
  uint32_t s = 42;
  fillRandom(bw, s);
  fillRandom(lsb, s);
  fillRandom(msb, s);
  std::vector<uint32_t> out(kW * kH);
  const int N = 300;

  double t0 = nowMs();
  for (int i = 0; i < N; ++i) composeRgba(bw.data(), lsb.data(), msb.data(), 0, out.data());
  const double page = nowMs() - t0;
  t0 = nowMs();
  for (int i = 0; i < N; ++i) composeRgba(bw.data(), nullptr, nullptr, 1, out.data());
  const double mono = nowMs() - t0;
  t0 = nowMs();
  for (int i = 0; i < N; ++i) composeRgbaNaive(bw.data(), lsb.data(), msb.data(), 0, out.data());
  const double naive = nowMs() - t0;

  printf("composeRgba 2-bit (grouped) : %7.3f ms/page  (%d iters)\n", page / N, N);
  printf("composeRgba 1-bit (grouped) : %7.3f ms/page\n", mono / N);
  printf("composeRgba naive reference : %7.3f ms/page  -> grouped is %.2fx faster\n", naive / N, naive / page);
  printf("JS reference for comparison  : 2.880 ms/page 2-bit, 0.385 ms/page 1-bit\n");
  return 0;
}

static bool readFile(const char* path, std::vector<uint8_t>& out) {
  FILE* f = fopen(path, "rb");
  if (!f) return false;
  fseek(f, 0, SEEK_END);
  const long n = ftell(f);
  fseek(f, 0, SEEK_SET);
  if (n <= 0) {
    fclose(f);
    return false;
  }
  out.resize(static_cast<size_t>(n));
  const size_t got = fread(out.data(), 1, out.size(), f);
  fclose(f);
  return got == out.size();
}

int main(int argc, char** argv) {
  if (argc == 2 && strcmp(argv[1], "bench") == 0) return runBench();
  if (argc == 4) {  // file <planes.bin> <rgba_out.bin> <mono 0|1>
    std::vector<uint8_t> planes;
    if (!readFile(argv[1], planes)) {
      fprintf(stderr, "cannot read %s\n", argv[1]);
      return 2;
    }
    const int mono = atoi(argv[3]);
    const size_t need = mono ? kPlaneBytes : kPlaneBytes * 3;
    if (planes.size() != need) {
      fprintf(stderr, "planes.bin is %zu bytes, expected %zu\n", planes.size(), need);
      return 2;
    }
    std::vector<uint32_t> out(kW * kH);
    composeRgba(planes.data(), mono ? nullptr : planes.data() + kPlaneBytes,
                mono ? nullptr : planes.data() + 2 * kPlaneBytes, mono, out.data());
    FILE* f = fopen(argv[2], "wb");
    if (!f) return 2;
    fwrite(out.data(), 1, kRgbaBytes, f);
    fclose(f);
    printf("wrote %s (%zu bytes, mono=%d)\n", argv[2], kRgbaBytes, mono);
    return 0;
  }
  fprintf(stderr, "usage: %s bench | %s <planes.bin> <rgba_out.bin> <mono 0|1>\n", argv[0], argv[0]);
  return 2;
}
