// xtch_writer.h — XTC/XTCH container + XTG/XTH page encoder for the KO port.
//
// One EPUB → one device file. The container format is the same for both bit
// depths; only the magic + page payload differ:
//   - 2-bit "XTCH" (high quality): pages are XTH. Each XTH page is a 22B header
//     ("XTH\0", w, h, 2 zero bytes, u32 dataSize, 8B digest) followed by
//     two profile-sized planes. Device decode contract (XtcReaderActivity):
//     value = (plane1bit<<1)|plane2bit, columns right→left, 8 vertical px/byte,
//     MSB=topmost; 0=white 1=dark-grey 2=light-grey 3=black.
//   - 1-bit "XTC\0" (fast): pages are XTG, a 22B header ("XTG\0", …,
//     dataSize) followed by one row-major plane, 8 px/byte MSB first.
//     X4 is 60 bytes/row × 800; X3 is 66 × 792. Device decode:
//     bit 0 = BLACK, bit 1 = WHITE.
//
// Engine plane mapping (empirically verified, 97.6% AA-edge adjacency):
//   lsb&msb marks = dark grey (v1), msb-only = light grey (v2),
//   ink without grey = black (v3), no ink = white (v0).
// For 1-bit XTG the engine BW plane's ink (0) maps directly to the device's
// black=0 bit; text and dithered image black both land in the BW pass.
#pragma once

#include <cstdint>

#include "device_profile.h"
#include "utf8_utils.h"

#include "../vendor-lib/Epub/Epub/converters/DitherUtils.h"  // the quantizer model
#include "../vendor-lib/Epub/Epub/converters/BlueNoise64.h"  // 64x64 void-and-cluster

// Ink densities for the 1-bit dither, indexed by the 4-level grey value
// (v = 0 white | 1 dark grey | 2 light grey | 3 black).
//
// A dithered patch must have the same AVERAGE tone as the level it stands in for, so the ink
// fraction for a level L is (255 - L) / 255 and the mask threshold IS 255 - L. Both numbers
// are derived from the quantizer's profile rather than written out here, because this is the
// last place that could silently keep a different model: the old hard-coded {0,235,170,255}
// encoded the fork's perceived-luminance levels (dark grey as 92% ink, i.e. reflectance ~30),
// while the official model's levels are the nominal 0/85/170/255 - its dark grey (85) needs
// 67% ink and its light grey (170) needs 33%.
//
//   v=0 white      L=255 -> 0.000 ->   0
//   v=1 dark grey  L= 85 -> 0.667 -> 170
//   v=2 light grey L=170 -> 0.333 ->  85
//   v=3 black      L=  0 -> 1.000 -> 255
//
// Change the quantization model and these follow it; they are not independent knobs.
inline constexpr uint8_t kMonoInkDensity[4] = {
    0,
    static_cast<uint8_t>(255 - kProfileNominal.ditherLevels[1]),
    static_cast<uint8_t>(255 - kProfileNominal.ditherLevels[2]),
    255};

// Precomputed 8-bit masks for the mono dither. Densities are kMonoInkDensity and the test is
// `noise < density`, so there are THREE real thresholds: 235 (v=1 dark), 170 (v=2 light), 255 (v=3
// black). The 255 layer is NOT optional - the comparison is strict and the table holds all 256
// values, so 1 in 256 black pixels must not ink. Omitting it cost ~70 wrong bits per page, which is
// exactly the deficit a gate run measured.
// Indexed [layer][fileY & 63][fileX-byte]; bit (7-j) answers the
// corresponding portrait-file pixel. 66 is X3's 528-pixel portrait width;
// X4 uses the first 60 entries.
struct MonoNoiseMasks {
  uint8_t m[3][64][66] = {};
  uint16_t widthBytes = 0;
  explicit MonoNoiseMasks(const ko::DeviceGeometry& geometry)
      : widthBytes(static_cast<uint16_t>(geometry.portraitWidth / 8)) {
    const int thr[3] = {kMonoInkDensity[1], kMonoInkDensity[2], kMonoInkDensity[3]};
    for (int t = 0; t < 3; ++t) {
      for (int y = 0; y < 64; ++y) {
        for (int k = 0; k < widthBytes; ++k) {
          uint8_t mask = 0;
          for (int j = 0; j < 8; ++j) {
            const int phyY = geometry.physicalHeight - 1 - 8 * k - j;
            if (kBlueNoise64[(phyY & 63) * 64 + (y & 63)] < thr[t]) {
              mask |= static_cast<uint8_t>(1u << (7 - j));
            }
          }
          m[t][y][k] = mask;
        }
      }
    }
  }
};
inline const MonoNoiseMasks& monoNoiseMasks(ko::DeviceProfile profile) {
  static const MonoNoiseMasks x4(ko::kX4Geometry);
  static const MonoNoiseMasks x3(ko::kX3Geometry);
  return profile == ko::DeviceProfile::X3 ? x3 : x4;
}

// 8x8 bit-matrix transpose of the eight bytes packed into a uint64 (byte j at bits 8*(7-j)).
// Definition, used as the reference for the fast version below:
//     out[b] bit (7-j) = in[j] bit (7-b)
// This is exactly the transpose the mono writer needs: the eight strided plane bytes are the rows,
// and the eight logical rows sharing an input byte column are the columns.
inline uint64_t transpose8x8Naive(uint64_t L) {
  uint64_t T = 0;
  for (int j = 0; j < 8; ++j) {
    const uint8_t in = static_cast<uint8_t>((L >> (8 * (7 - j))) & 0xFF);
    for (int b = 0; b < 8; ++b) {
      if (((in >> (7 - b)) & 1) != 0) T |= static_cast<uint64_t>(1) << (8 * (7 - b) + (7 - j));
    }
  }
  return T;
}

// Fast form: three delta-swap stages (Hacker's Delight). Swapping bit (r,c) with (c,r) needs the
// deltas 7, 14, 28 for an 8x8 matrix held in 64 bits.
inline uint64_t transpose8x8(uint64_t x) {
  uint64_t t;
  t = (x ^ (x >> 7)) & 0x00AA00AA00AA00AAULL;  x = x ^ t ^ (t << 7);
  t = (x ^ (x >> 14)) & 0x0000CCCC0000CCCCULL; x = x ^ t ^ (t << 14);
  t = (x ^ (x >> 28)) & 0x00000000F0F0F0F0ULL; x = x ^ t ^ (t << 28);
  return x;
}
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

namespace ko {

// Output mode of the container (mirrors the device's two file magics).
// The container header stores the page count in 16 bits (out[6] | out[7] << 8) and chapters store their
// page range as uint16 on disk. So 65535 pages is a hard FORMAT limit and 65536 would wrap the header to
// zero — a container that says it has no pages while carrying them. Defined once, used by the writer and
// by both transaction entry points in the wasm API.
inline constexpr size_t MAX_XTC_PAGES = 65535;
inline constexpr size_t MAX_XTC_CHAPTERS = 65535;

enum class XtcMode {
  Mono1Bit = 0,   // "XTC\0", XTG pages (1-bit fast)
  Gray2Bit = 1,   // "XTCH", XTH pages (2-bit high quality)
};

struct XtchChapter {
  std::string name;
  uint16_t startPage = 0;  // 0-based
  uint16_t endPage = 0;    // 0-based inclusive
};

// A raw EPUB-TOC entry captured during export, before dedupe/ordering.
struct ChapterCandidate {
  std::string title;
  uint32_t page = 0;       // global (container) page the entry starts at
  // False for EPUB navigation entries, true for headings inferred from the
  // rendered XHTML. Navigation is authoritative when the device's 100-entry
  // chapter table is full; inferred headings may only consume spare slots.
  bool inferred = false;
};

class XtchWriter {
 public:
  explicit XtchWriter(XtcMode mode = XtcMode::Gray2Bit,
                      DeviceProfile profile = DeviceProfile::X4)
      : mode_(mode), deviceProfile_(profile) {}

  void setMode(XtcMode mode) { mode_ = mode; }
  XtcMode mode() const { return mode_; }
  void setDeviceProfile(DeviceProfile profile) { deviceProfile_ = profile; }
  DeviceProfile deviceProfile() const { return deviceProfile_; }
  const DeviceGeometry& geometry() const { return deviceGeometry(deviceProfile_); }

  void setMetadata(const std::string& title, const std::string& author,
                   const std::string& publisher, const std::string& language) {
    title_ = title;
    author_ = author;
    publisher_ = publisher;
    language_ = language;
  }

  // Encode the selected profile's physical panel capture into its portrait
  // record geometry consumed by the Korean fork's XtcReaderActivity: X4
  // 480x800, X3 528x792. In landscape the renderer has already rotated the
  // logical layout into the physical capture, so the record is pre-rotated.
  // Blue-noise dithering of grey text on 1-bit pages. ON by default: without it every
  // grey edge collapses to a hard threshold and 1-bit output has no anti-aliasing at all.
  // The text-AA switch, as it affects a 1-bit page. Grey-bearing pixels are ALWAYS
  // halftoned (that is how a 4-level page becomes a 1-bit one, and it is what images
  // depend on - they must be dithered to 2 levels in every mode). This switch adds the
  // thinning of SOLID ink by the 255 layer, which is the AA-on look; with text AA off
  // the solid ink stays crisp (text renders as clean 1-bit), while image greys are
  // still halftoned.
  void setTextAa(bool on) { textAa_ = on; }
  bool textAa() const { return textAa_; }

  bool addPageFromPlanes(const std::vector<uint8_t>& bw, const std::vector<uint8_t>& lsb,
                         const std::vector<uint8_t>& msb) {
    if (pageCount() >= MAX_XTC_PAGES) return false;   // see MAX_XTC_PAGES: the header count wraps

    const auto& g = geometry();
    const uint16_t LOGICAL_W = g.portraitWidth;
    const uint16_t LOGICAL_H = g.portraitHeight;
    if (mode_ == XtcMode::Mono1Bit) {
      return addMonoPage(bw, lsb, msb, LOGICAL_W, LOGICAL_H);
    }
    return addGrayPage(bw, lsb, msb, LOGICAL_W, LOGICAL_H);
  }

 private:
  // 1-bit XTG page: single row-major plane, 8 px/byte MSB first, bit 0 = black.
  bool addMonoPage(const std::vector<uint8_t>& bw, const std::vector<uint8_t>& lsb,
                   const std::vector<uint8_t>& msb, uint16_t LOGICAL_W, uint16_t LOGICAL_H) {
    const auto& g = geometry();
    const size_t planeBytes = g.planeBytes;
    if (bw.size() != planeBytes || lsb.size() != planeBytes || msb.size() != planeBytes) return false;
    std::vector<uint8_t> plane(planeBytes, 0xFF);  // start white (1)
    const bool thinSolid = textAa_;  // AA on: solid ink is thinned too

    // Bit-parallel: one OUTPUT BYTE (8 pixels) per iteration instead of one pixel. The output
    // byte at [y * outputRowBytes + k] holds x = 8k..8k+7; those pixels sit on descending phyY with
    // phyX = y fixed, so they are the same bit position (7 - (y & 7)) of eight plane bytes.
    // The dither uses precomputed mask layers instead of a per-pixel table lookup - that lookup
    // was measured at ~0.32 ns/pixel, about one cycle each.
    const MonoNoiseMasks& nm = monoNoiseMasks(deviceProfile_);
    const int physicalRowBytes = g.physicalRowBytes;
    const int outputRowBytes = LOGICAL_W / 8;
    // One 8x8 block per iteration: 8 strided loads per plane cover 64 pixels (previous version:
    // 24 loads per 8 pixels). Gather, transpose, then decide eight output bytes.
    for (int c = 0; c < LOGICAL_H / 8; ++c) {        // input byte column = phyX >> 3 = y >> 3
      for (int k = 0; k < LOGICAL_W / 8; ++k) {      // output byte column = x >> 3
        uint64_t Lbw = 0, Llsb = 0, Lmsb = 0;
        for (int j = 0; j < 8; ++j) {
          const size_t off = static_cast<size_t>(g.physicalHeight - 1 - 8 * k - j) *
                                 physicalRowBytes + c;
          const int sh = 8 * (7 - j);
          Lbw |= static_cast<uint64_t>(bw[off]) << sh;
          Llsb |= static_cast<uint64_t>(lsb[off]) << sh;
          Lmsb |= static_cast<uint64_t>(msb[off]) << sh;
        }
        const uint64_t Tbw = transpose8x8(Lbw);
        const uint64_t Tlsb = transpose8x8(Llsb);
        const uint64_t Tmsb = transpose8x8(Lmsb);
        for (int b = 0; b < 8; ++b) {
          const int y = c * 8 + b;
          const int sh = 8 * (7 - b);
          // Transposed byte: bit (7-j) holds the pixel at x = 8k + j of this logical row.
          uint8_t inkBits = static_cast<uint8_t>(~((Tbw >> sh) & 0xFF));  // ink bit = 0
          if (inkBits) {
            const uint8_t lBits = static_cast<uint8_t>((Tlsb >> sh) & 0xFF);
            const uint8_t mBits = static_cast<uint8_t>((Tmsb >> sh) & 0xFF);
            const uint8_t m3 = static_cast<uint8_t>(~lBits & ~mBits);  // solid ink, no grey
            const uint8_t m2 = static_cast<uint8_t>(~lBits & mBits);   // light grey
            const uint8_t yc = static_cast<uint8_t>(y & 63);
            // Grey pixels are halftoned unconditionally: this is the 4-level -> 2-level
            // step that images (and AA-on text) rely on in every mode.
            uint8_t keep = static_cast<uint8_t>((lBits & nm.m[0][yc][k]) |
                                                (m2 & nm.m[1][yc][k]));
            // Solid ink is thinned only with text AA on; with it off the ink stays
            // crisp, which is the whole point of that switch position for text.
            keep |= static_cast<uint8_t>(thinSolid ? (m3 & nm.m[2][yc][k]) : m3);
            inkBits = static_cast<uint8_t>(inkBits & keep);
          }
          plane[static_cast<size_t>(y) * outputRowBytes + k] &= static_cast<uint8_t>(~inkBits);
        }
      }
    }

    const uint32_t dataSize = static_cast<uint32_t>(planeBytes);
    std::vector<uint8_t> page;
    page.reserve(22 + dataSize);
    page.push_back('X'); page.push_back('T'); page.push_back('G'); page.push_back(0);
    page.push_back(LOGICAL_W & 0xFF); page.push_back(LOGICAL_W >> 8);
    page.push_back(LOGICAL_H & 0xFF); page.push_back(LOGICAL_H >> 8);
    page.push_back(0); page.push_back(0);  // reserved
    page.push_back(dataSize & 0xFF); page.push_back((dataSize >> 8) & 0xFF);
    page.push_back((dataSize >> 16) & 0xFF); page.push_back((dataSize >> 24) & 0xFF);
    page.insert(page.end(), 8, 0);  // digest = 0
    page.insert(page.end(), plane.begin(), plane.end());
    pendingPages_.push_back(std::move(page));
    pendingPageBytes_ += pendingPages_.back().capacity();
    return true;
  }

  // 2-bit XTH page: two column-major planes (bit1/bit2), columns right→left.
  //
  // Optimized from the per-pixel form: it is a byte-aligned permutation, so it
  // runs 8 pixels at a time. For a logical column x every pixel reads physical
  // row phyY = physicalHeight - 1 - x, and a pixel's source bit position (7 - (y & 7)) is the
  // SAME as its destination bit position — so no shifting or bit reversal is
  // needed and the inner portrait-height loop becomes one byte operation per 8 pixels:
  //     ink = ~bw   (BW bit 0 = ink)
  //     v = 0 white | 1 dark grey | 2 light grey | 3 black   (from ink/l/m)
  //     p1 = v & 2  =  ink & ~l              (light grey or black)
  //     p2 = v & 1  =  ink & (l | ~m)        (dark grey or black)
  bool addGrayPage(const std::vector<uint8_t>& bw, const std::vector<uint8_t>& lsb,
                   const std::vector<uint8_t>& msb, uint16_t LOGICAL_W, uint16_t LOGICAL_H) {
    const auto& g = geometry();
    const size_t planeBytes = g.planeBytes;
    std::vector<uint8_t> p1(planeBytes, 0), p2(planeBytes, 0);
    if (bw.size() != planeBytes || lsb.size() != planeBytes || msb.size() != planeBytes) return false;

    const int rowBytes = g.physicalRowBytes;
    const int rowsPerCol = LOGICAL_H / 8;
    for (int x = 0; x < LOGICAL_W; x++) {
      const int targetCol = LOGICAL_W - 1 - x;   // XTH columns right→left
      const int phyY = g.physicalHeight - 1 - x;
      const uint8_t* rowB = bw.data() + static_cast<size_t>(phyY) * rowBytes;
      const uint8_t* rowL = lsb.data() + static_cast<size_t>(phyY) * rowBytes;
      const uint8_t* rowM = msb.data() + static_cast<size_t>(phyY) * rowBytes;
      uint8_t* out1 = p1.data() + static_cast<size_t>(targetCol) * rowBytes;
      uint8_t* out2 = p2.data() + static_cast<size_t>(targetCol) * rowBytes;
      for (int by = 0; by < rowsPerCol; by++) {
        const uint8_t ink = static_cast<uint8_t>(~rowB[by]);
        const uint8_t l = rowL[by];
        const uint8_t m = rowM[by];
        out1[by] = static_cast<uint8_t>(ink & static_cast<uint8_t>(~l));
        out2[by] = static_cast<uint8_t>(ink & static_cast<uint8_t>(l | static_cast<uint8_t>(~m)));
      }
    }

    // XTH page: 22B header + plane1 + plane2
    const uint32_t dataSize = static_cast<uint32_t>(planeBytes * 2);
    std::vector<uint8_t> page;
    page.reserve(22 + dataSize);
    page.push_back('X'); page.push_back('T'); page.push_back('H'); page.push_back(0);
    page.push_back(LOGICAL_W & 0xFF); page.push_back(LOGICAL_W >> 8);
    page.push_back(LOGICAL_H & 0xFF); page.push_back(LOGICAL_H >> 8);
    page.push_back(0); page.push_back(0);  // reserved
    page.push_back(dataSize & 0xFF); page.push_back((dataSize >> 8) & 0xFF);
    page.push_back((dataSize >> 16) & 0xFF); page.push_back((dataSize >> 24) & 0xFF);
    page.insert(page.end(), 8, 0);  // digest = 0 (vendor writes none)
    page.insert(page.end(), p1.begin(), p1.end());
    page.insert(page.end(), p2.begin(), p2.end());
    pendingPages_.push_back(std::move(page));
    pendingPageBytes_ += pendingPages_.back().capacity();
    return true;
  }

 public:
  // Finalize container bytes: 56B header + 256B metadata + chapters + index + data
  // ---- pooling support -------------------------------------------------------
  // A spine is the unit of parallel work. Workers encode spines into page records; ONE assembler
  // appends those records in spine order and writes the container. addRawPage validates every record
  // against the assembler's mode, geometry, size, and page ceiling.
  const std::vector<uint8_t>& page(size_t i) const { return pendingPages_[i]; }

  // Append an already-encoded page record verbatim.
  bool addRawPage(const uint8_t* data, size_t size) {
    if (pageCount() >= MAX_XTC_PAGES) return false;   // see MAX_XTC_PAGES

    if (data == nullptr || size < 22) return false;   // every record carries its 22-byte header
    const bool mono = data[0] == 'X' && data[1] == 'T' && data[2] == 'G' && data[3] == 0;
    const bool gray = data[0] == 'X' && data[1] == 'T' && data[2] == 'H' && data[3] == 0;
    if (!mono && !gray) return false;
    if ((mode_ == XtcMode::Mono1Bit) != mono) return false;
    const uint16_t width = static_cast<uint16_t>(data[4] | (data[5] << 8));
    const uint16_t height = static_cast<uint16_t>(data[6] | (data[7] << 8));
    const uint32_t payload = static_cast<uint32_t>(data[10]) |
                             (static_cast<uint32_t>(data[11]) << 8) |
                             (static_cast<uint32_t>(data[12]) << 16) |
                             (static_cast<uint32_t>(data[13]) << 24);
    const auto& g = geometry();
    const uint32_t expectedPayload = static_cast<uint32_t>(g.planeBytes * (gray ? 2 : 1));
    if (width != g.portraitWidth || height != g.portraitHeight ||
        payload != expectedPayload || size != 22u + expectedPayload) {
      return false;
    }
    pendingPages_.emplace_back(data, data + size);
    pendingPageBytes_ += pendingPages_.back().capacity();
    return true;
  }

  void clearPages() {
    std::vector<std::vector<uint8_t>>().swap(pendingPages_);
    pendingPageBytes_ = 0;
  }
  uint64_t residentPageBytes() const { return pendingPageBytes_; }

  // The container header carries the book's metadata, so an assembler writer — constructed fresh, with
  // no book of its own — has to adopt it from the engine's writer or the file loses its title.
  void adoptMetadataFrom(const XtchWriter& other) {
    title_ = other.title_; author_ = other.author_;
    publisher_ = other.publisher_; language_ = other.language_;
  }

  // The container's fixed-layout prefix: 56-byte header, 256-byte metadata, the chapter table, and the
  // 16-byte-per-page index. Everything in it is a function of the page SIZES and the chapters — each
  // index entry is (running offset, size, profile width, profile height) — so it can be produced without ever holding the
  // page bytes. That is what lets the pool assemble a file as [prefix][records…] in the browser, with
  // no whole-container copy after encoding. finish() below is this same code plus the records, so the
  // format still has exactly one implementation.
  std::vector<uint8_t> buildPrefix(const std::vector<XtchChapter>& chapters,
                                   const std::vector<uint32_t>& pageSizes) const {
    const size_t pageCount = pageSizes.size();
    if (pageCount == 0 || pageCount > MAX_XTC_PAGES || chapters.size() > MAX_XTC_CHAPTERS) return {};
    const size_t chapterCount = chapters.size();
    const uint64_t metadataOffset = 56;
    const uint64_t chapterOffset = metadataOffset + 256;
    const uint64_t indexOffset = chapterOffset + chapterCount * 96;
    const uint64_t dataOffset = indexOffset + pageCount * 16;

    std::vector<uint8_t> out(static_cast<size_t>(dataOffset), 0);
    writePrefix(out, chapters, pageSizes, metadataOffset, chapterOffset, indexOffset, dataOffset);
    return out;
  }

  std::vector<uint8_t> finish(const std::vector<XtchChapter>& chapters) {
    std::vector<uint32_t> sizes;
    sizes.reserve(pendingPages_.size());
    for (const auto& p : pendingPages_) sizes.push_back(static_cast<uint32_t>(p.size()));
    std::vector<uint8_t> out = buildPrefix(chapters, sizes);
    if (out.empty()) return {};
    const size_t pageCount = pendingPages_.size();

    // Allocate the final buffer exactly once.  The old code claimed buildPrefix
    // did this but it only allocated the fixed prefix, so repeated insert()
    // growth could retain pending pages, an old output allocation and a new
    // allocation simultaneously.  The API preflight accounts for the one
    // unavoidable overlap: pending records + this final allocation.
    uint64_t finalSize = out.size();
    for (uint32_t size : sizes) {
      if (size > UINT64_MAX - finalSize) return {};
      finalSize += size;
    }
    if (finalSize > static_cast<uint64_t>(SIZE_MAX)) return {};
    out.reserve(static_cast<size_t>(finalSize));

    // --- data ---
    // Memory-slim streaming: append each page then free its pending buffer, so peak usage stays ~= final
    // file size instead of 2x (pendingPages + copy). The index entries were already written by
    // buildPrefix from the sizes.
    for (size_t i = 0; i < pageCount; i++) {
      auto& page = pendingPages_[i];
      out.insert(out.end(), page.begin(), page.end());
      pendingPageBytes_ -= std::min<uint64_t>(pendingPageBytes_, page.capacity());
      std::vector<uint8_t>().swap(page);  // release this page's heap now
    }
    return out;
  }

  void writePrefix(std::vector<uint8_t>& out, const std::vector<XtchChapter>& chapters,
                   const std::vector<uint32_t>& pageSizes, uint64_t metadataOffset,
                   uint64_t chapterOffset, uint64_t indexOffset, uint64_t dataOffset) const {
    const size_t pageCount = pageSizes.size();
    const size_t chapterCount = chapters.size();
    // --- header ---
    if (mode_ == XtcMode::Mono1Bit) {
      out[0] = 'X'; out[1] = 'T'; out[2] = 'C'; out[3] = 0;   // "XTC\0" 1-bit
    } else {
      out[0] = 'X'; out[1] = 'T'; out[2] = 'C'; out[3] = 'H'; // "XTCH" 2-bit
    }
    out[4] = 0x01; out[5] = 0x00;                            // version 1.0
    out[6] = pageCount & 0xFF; out[7] = (pageCount >> 8) & 0xFF;
    out[8] = 0;                      // readDirection L→R (Korean prose)
    out[9] = 1;                      // hasMetadata
    out[10] = 0;                     // hasThumbnails
    out[11] = chapterCount > 0 ? 1 : 0;  // hasChapters
    // currentPage (0xC..0xF): vendor files store 1
    putU32(out, 0x0C, 1);
    putU64(out, 0x10, metadataOffset);
    putU64(out, 0x18, indexOffset);
    putU64(out, 0x20, dataOffset);
    putU64(out, 0x28, 0);            // thumbOffset
    // chapterOffset: official writer always points at 312 (56 header + 256
    // metadata) even with zero chapters; device parser reads it as u64 @0x30
    // and only consults it when hasChapters=1.
    putU64(out, 0x30, chapterOffset);
    // --- metadata (256B @56) ---
    putStr(out, metadataOffset + 0x00, title_, 128);
    putStr(out, metadataOffset + 0x80, author_, 64);
    putStr(out, metadataOffset + 0xC0, publisher_, 32);
    putStr(out, metadataOffset + 0xE0, language_, 16);
    putU32(out, metadataOffset + 0xF0, static_cast<uint32_t>(time(nullptr)));  // createTime
    putU16(out, metadataOffset + 0xF4, 0);              // coverPage: official leaves 0 (page 0)
    putU16(out, metadataOffset + 0xF6, static_cast<uint16_t>(chapterCount));
    // reserved 0xF8..0xFF stays 0
    // --- chapters (96B each) ---
    // Chapter page fields are 1-BASED on disk: the device parser decrements
    // (startPage-- / endPage--) when reading. Official writer stores +1.
    for (size_t i = 0; i < chapterCount; i++) {
      const uint64_t base = chapterOffset + i * 96;
      putStr(out, base + 0x00, chapters[i].name, 80);
      putU16(out, base + 0x50, static_cast<uint16_t>(chapters[i].startPage + 1));
      putU16(out, base + 0x52, static_cast<uint16_t>(chapters[i].endPage + 1));
      // reserved 0x54..0x5F stays 0
    }
    // --- index + data ---
    // Memory-slim streaming: append each page then free its pending buffer, so
    // peak usage stays ~= final file size instead of 2x (pendingPages + copy).
    uint64_t cursor = dataOffset;
    for (size_t i = 0; i < pageCount; i++) {
      const uint64_t e = indexOffset + i * 16;
      putU64(out, e + 0x00, cursor);
      putU32(out, e + 0x08, pageSizes[i]);
      putU16(out, e + 0x0C, geometry().portraitWidth);
      putU16(out, e + 0x0E, geometry().portraitHeight);
      cursor += pageSizes[i];
    }
  }


  // §4 of the 1.2 audit: clear() destroys the page vectors but the OUTER vector keeps its
  // allocation, so a cancelled export of a big book could stay resident at ~100 MB. swap() hands the
  // allocation back to the allocator immediately.
  void reset() {
    std::vector<std::vector<uint8_t>>().swap(pendingPages_);
    pendingPageBytes_ = 0;
  }
  size_t pageCount() const { return pendingPages_.size(); }

 private:
  static void putU16(std::vector<uint8_t>& v, size_t off, uint16_t val) {
    v[off] = val & 0xFF; v[off + 1] = (val >> 8) & 0xFF;
  }
  static void putU32(std::vector<uint8_t>& v, size_t off, uint32_t val) {
    v[off] = val & 0xFF; v[off + 1] = (val >> 8) & 0xFF;
    v[off + 2] = (val >> 16) & 0xFF; v[off + 3] = (val >> 24) & 0xFF;
  }
  static void putU64(std::vector<uint8_t>& v, size_t off, uint64_t val) {
    for (int i = 0; i < 8; i++) v[off + i] = (val >> (8 * i)) & 0xFF;
  }
  static void putStr(std::vector<uint8_t>& v, size_t off, const std::string& s, size_t maxLen) {
    if (maxLen == 0) return;
    const size_t n = utf8SafePrefixLength(s, maxLen - 1);
    memcpy(v.data() + off, s.data(), n);
  }

  std::string title_, author_, publisher_, language_;
  XtcMode mode_ = XtcMode::Gray2Bit;
  DeviceProfile deviceProfile_ = DeviceProfile::X4;
  std::vector<std::vector<uint8_t>> pendingPages_;
  uint64_t pendingPageBytes_ = 0;
  bool textAa_ = true;
};

}  // namespace ko
