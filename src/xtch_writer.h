// xtch_writer.h — XTC/XTCH container + XTG/XTH page encoder for the KO port.
//
// One EPUB → one device file. The container format is the same for both bit
// depths; only the magic + page payload differ:
//   - 2-bit "XTCH" (high quality): pages are XTH. Each XTH page is a 22B header
//     ("XTH\0", w, h, 2 zero bytes, u32 dataSize=96000, 8B digest) followed by
//     two 48000-byte planes. Device decode contract (XtcReaderActivity):
//     value = (plane1bit<<1)|plane2bit, columns right→left, 8 vertical px/byte,
//     MSB=topmost; 0=white 1=dark-grey 2=light-grey 3=black.
//   - 1-bit "XTC\0" (fast): pages are XTG, a 22B header ("XTG\0", …,
//     dataSize=48000) followed by one row-major plane, 8 px/byte MSB first,
//     60 bytes/row × 800 rows. Device decode: bit 0 = BLACK, bit 1 = WHITE.
//
// Engine plane mapping (empirically verified, 97.6% AA-edge adjacency):
//   lsb&msb marks = dark grey (v1), msb-only = light grey (v2),
//   ink without grey = black (v3), no ink = white (v0).
// For 1-bit XTG the engine BW plane's ink (0) maps directly to the device's
// black=0 bit; text and dithered image black both land in the BW pass.
#pragma once

#include <cstdint>

#include "../vendor-lib/Epub/Epub/converters/BlueNoise64.h"  // 64x64 void-and-cluster

// Ink densities for the 1-bit dither, indexed by the 4-level grey value
// (v = 0 white | 1 dark grey | 2 light grey | 3 black).
//
// A dithered patch must have the same AVERAGE reflectance as the grey level it stands in
// for. With the panel's four states perceived as white 210 / light grey 80 / dark grey 30
// / black 15, density = (210 - R) / (210 - 15):
//
//   v=0 white      R=210 -> 0.000 ->   0
//   v=1 dark grey  R= 30 -> 0.923 -> 235
//   v=2 light grey R= 80 -> 0.667 -> 170
//   v=3 black      R= 15 -> 1.000 -> 255
//
// The upstream attempt used 62% / 19% here, which renders dark grey far too light and
// throws away most of the contrast the panel's greys actually have.
//
// NOTE: the 15/30/80/210 anchors come from the panel model used by the converter, not from
// a calibrated measurement of our own unit. If a real test pattern says otherwise, these
// three numbers are the only thing that needs to change.
inline constexpr uint8_t kMonoInkDensity[4] = {0, 235, 170, 255};

// Precomputed 8-bit masks for the mono dither. Densities are {0, 235, 170, 255} and the test is
// `noise < density`, so there are THREE real thresholds: 235 (v=1 dark), 170 (v=2 light), 255 (v=3
// black). The 255 layer is NOT optional - the comparison is strict and the table holds all 256
// values, so 1 in 256 black pixels must not ink. Omitting it cost ~70 wrong bits per page, which is
// exactly the deficit a gate run measured.
// Indexed [layer][y & 63][k]; bit (7 - j) answers for the pixel at x = 8k + j (phyY = 479-8k-j).
struct MonoNoiseMasks {
  uint8_t m[3][64][60];
  MonoNoiseMasks() {
    const int thr[3] = {235, 170, 255};
    for (int t = 0; t < 3; ++t) {
      for (int y = 0; y < 64; ++y) {
        for (int k = 0; k < 60; ++k) {
          uint8_t mask = 0;
          for (int j = 0; j < 8; ++j) {
            const int phyY = 479 - 8 * k - j;
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
inline const MonoNoiseMasks& monoNoiseMasks() {
  static const MonoNoiseMasks inst;
  return inst;
}
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

namespace ko {

// Output mode of the container (mirrors the device's two file magics).
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
};

class XtchWriter {
 public:
  explicit XtchWriter(XtcMode mode = XtcMode::Gray2Bit) : mode_(mode) {}

  void setMode(XtcMode mode) { mode_ = mode; }
  XtcMode mode() const { return mode_; }

  void setMetadata(const std::string& title, const std::string& author,
                   const std::string& publisher, const std::string& language) {
    title_ = title;
    author_ = author;
    publisher_ = publisher;
    language_ = language;
  }

  // Encode one logical (480x800 portrait) page from the three physical
  // (800x480) plane captures. Appends an XTG (1-bit) or XTH (2-bit) page to
  // pendingPages_ according to mode_.
  // Blue-noise dithering of grey text on 1-bit pages. ON by default: without it every
  // grey edge collapses to a hard threshold and 1-bit output has no anti-aliasing at all.
  void setMonoGrayDither(bool on) { monoGrayDither_ = on; }
  bool monoGrayDither() const { return monoGrayDither_; }

  bool addPageFromPlanes(const std::vector<uint8_t>& bw, const std::vector<uint8_t>& lsb,
                         const std::vector<uint8_t>& msb) {
    constexpr uint16_t LOGICAL_W = 480;
    constexpr uint16_t LOGICAL_H = 800;
    if (mode_ == XtcMode::Mono1Bit) {
      return addMonoPage(bw, lsb, msb, LOGICAL_W, LOGICAL_H);
    }
    return addGrayPage(bw, lsb, msb, LOGICAL_W, LOGICAL_H);
  }

 private:
  // 1-bit XTG page: single row-major plane, 8 px/byte MSB first, bit 0 = black.
  bool addMonoPage(const std::vector<uint8_t>& bw, const std::vector<uint8_t>& lsb,
                   const std::vector<uint8_t>& msb, uint16_t LOGICAL_W, uint16_t LOGICAL_H) {
    std::vector<uint8_t> plane(48000, 0xFF);  // start white (1)
    const bool haveGray = lsb.size() >= 48000 && msb.size() >= 48000;
    const bool dither = monoGrayDither_ && haveGray;

    auto physBit = [](const std::vector<uint8_t>& buf, int phyX, int phyY) -> int {
      return (buf[phyY * 100 + (phyX >> 3)] >> (7 - (phyX & 7))) & 1;
    };

    // Bit-parallel: one OUTPUT BYTE (8 pixels) per iteration instead of one pixel. The output
    // byte at [y * 60 + k] holds x = 8k..8k+7; those eight pixels sit at phyY = 479-8k-j with
    // phyX = y fixed, so they are the same bit position (7 - (y & 7)) of eight plane bytes.
    // The dither uses precomputed mask layers instead of a per-pixel table lookup - that lookup
    // was measured at ~0.32 ns/pixel, about one cycle each.
    const MonoNoiseMasks& nm = monoNoiseMasks();
    for (int y = 0; y < LOGICAL_H; y++) {
      const int planeBit = 7 - (y & 7);
      const uint8_t yc = static_cast<uint8_t>(y & 63);
      const uint8_t* bwRow = bw.data() + (y >> 3);
      const uint8_t* lsbRow = haveGray ? lsb.data() + (y >> 3) : nullptr;
      const uint8_t* msbRow = haveGray ? msb.data() + (y >> 3) : nullptr;
      uint8_t* outRow = plane.data() + static_cast<size_t>(y) * 60;
      for (int k = 0; k < LOGICAL_W / 8; k++) {
        uint8_t bwBits = 0, lBits = 0, mBits = 0;
        for (int j = 0; j < 8; ++j) {
          const size_t off = static_cast<size_t>(479 - 8 * k - j) * 100;
          const uint8_t sh = static_cast<uint8_t>(1u << (7 - j));
          if (((bwRow[off] >> planeBit) & 1) == 0) bwBits |= sh;   // ink bit = 0 in engine BW
          if (haveGray) {
            if (((lsbRow[off] >> planeBit) & 1) != 0) lBits |= sh;
            if (((msbRow[off] >> planeBit) & 1) != 0) mBits |= sh;
          }
        }
        uint8_t inkBits = bwBits;
        if (dither && inkBits) {
          // v: 1 = lsb; 2 = !lsb && msb; 3 = !lsb && !msb (same encoding as the scalar path).
          const uint8_t m3 = static_cast<uint8_t>(~lBits & ~mBits);
          const uint8_t m2 = static_cast<uint8_t>(~lBits &  mBits);
          inkBits = static_cast<uint8_t>(
              inkBits & static_cast<uint8_t>((m3 & nm.m[2][yc][k]) |
                                             (lBits & nm.m[0][yc][k]) |
                                             (m2 & nm.m[1][yc][k])));
        }
        outRow[k] &= static_cast<uint8_t>(~inkBits);   // XTG bit 0 = black
      }
    }

    const uint32_t dataSize = 48000;
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
    return true;
  }

  // 2-bit XTH page: two column-major planes (bit1/bit2), columns right→left.
  //
  // Optimized from the per-pixel form: it is a byte-aligned permutation, so it
  // runs 8 pixels at a time. For a logical column x every pixel reads physical
  // row phyY = 479 - x, and a pixel's source bit position (7 - (y & 7)) is the
  // SAME as its destination bit position — so no shifting or bit reversal is
  // needed and the inner 800-pixel loop becomes 100 byte operations:
  //     ink = ~bw   (BW bit 0 = ink)
  //     v = 0 white | 1 dark grey | 2 light grey | 3 black   (from ink/l/m)
  //     p1 = v & 2  =  ink & ~l              (light grey or black)
  //     p2 = v & 1  =  ink & (l | ~m)        (dark grey or black)
  bool addGrayPage(const std::vector<uint8_t>& bw, const std::vector<uint8_t>& lsb,
                   const std::vector<uint8_t>& msb, uint16_t LOGICAL_W, uint16_t LOGICAL_H) {
    std::vector<uint8_t> p1(48000, 0), p2(48000, 0);
    if (bw.size() < 48000 || lsb.size() < 48000 || msb.size() < 48000) return false;

    const int rowBytes = 100;         // physical row stride: 800 px / 8
    const int rowsPerCol = LOGICAL_H / 8;   // 800 logical y -> 100 bytes
    for (int x = 0; x < LOGICAL_W; x++) {
      const int targetCol = LOGICAL_W - 1 - x;   // XTH columns right→left
      const int phyY = 479 - x;                  // portrait: phyX = y, phyY = 479 - x
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
    const uint32_t dataSize = 96000;
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
    return true;
  }

 public:
  // Finalize container bytes: 56B header + 256B metadata + chapters + index + data
  std::vector<uint8_t> finish(const std::vector<XtchChapter>& chapters) {
    const size_t pageCount = pendingPages_.size();
    const size_t chapterCount = chapters.size();
    const uint64_t metadataOffset = 56;
    const uint64_t chapterOffset = metadataOffset + 256;
    const uint64_t indexOffset = chapterOffset + chapterCount * 96;
    // data area: 16B/page index then pages
    uint64_t totalData = 0;
    for (const auto& p : pendingPages_) totalData += p.size();
    const uint64_t dataOffset = indexOffset + pageCount * 16;

    // Pre-reserve exact final size: avoids realloc doubling that can spike peak
    // memory past the wasm heap on multi-hundred-MB books.
    std::vector<uint8_t> out;
    out.reserve(dataOffset + totalData);
    out.resize(dataOffset, 0);
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
      auto& page = pendingPages_[i];
      const uint64_t e = indexOffset + i * 16;
      putU64(out, e + 0x00, cursor);
      putU32(out, e + 0x08, static_cast<uint32_t>(page.size()));
      putU16(out, e + 0x0C, 480);
      putU16(out, e + 0x0E, 800);
      out.insert(out.end(), page.begin(), page.end());
      cursor += page.size();
      std::vector<uint8_t>().swap(page);  // release this page's heap now
    }
    return out;
  }

  void reset() { pendingPages_.clear(); }
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
    size_t n = s.size() < maxLen ? s.size() : maxLen - 1;
    memcpy(v.data() + off, s.data(), n);
  }

  std::string title_, author_, publisher_, language_;
  XtcMode mode_ = XtcMode::Gray2Bit;
  std::vector<std::vector<uint8_t>> pendingPages_;
  bool monoGrayDither_ = true;
};

}  // namespace ko
