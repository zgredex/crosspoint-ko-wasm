#pragma once

#include <HalStorage.h>

#include <cstdint>
#include <list>
#include <string>
#include <unordered_map>
#include <vector>

#include "EpdFontData.h"
#include "SdFontFormat.h"

/**
 * LRU Cache for glyph bitmap data loaded from SD card.
 * Automatically evicts least recently used entries when memory limit is reached.
 */
class GlyphBitmapCache {
 public:
  struct CacheEntry {
    // Composite (fontId<<32 | codepoint) key. The cache is shared across every
    // SdFontData instance, so keying on codepoint alone would alias the same
    // glyph across different fonts (e.g. a Hanja present in both the reader and
    // the system font), returning one font's bitmap bytes for another font's
    // metadata — a guaranteed size mismatch and heap over-read. Fold in the
    // font identity so each font's glyphs occupy distinct cache slots.
    uint64_t key;
    uint8_t* bitmap;
    uint32_t size;
  };

 private:
  size_t maxCacheSize;
  size_t currentSize;
  std::list<CacheEntry> cacheList;  // Most recent at front
  std::unordered_map<uint64_t, std::list<CacheEntry>::iterator> cacheMap;

  void evictOldest();

 public:
  explicit GlyphBitmapCache(size_t maxSize = 4 * 1024 * 1024);  // 4MB fallback (host build: RAM is not constrained)
  ~GlyphBitmapCache();

  // Returns cached bitmap or nullptr if not cached
  const uint8_t* get(uint64_t key);

  // Stores bitmap in cache, returns pointer to cached data
  const uint8_t* put(uint64_t key, const uint8_t* data, uint32_t size);

  void clear();
  size_t getUsedSize() const { return currentSize; }
  size_t getMaxSize() const { return maxCacheSize; }
};

/**
 * SD Card font data structure.
 * Mimics EpdFontData interface but loads data on-demand from SD card.
 */
/**
 * Simple fixed-size cache for glyph metadata (EpdGlyph) loaded on-demand.
 * Uses a simple circular buffer to avoid STL container overhead on ESP32.
 */
class GlyphMetadataCache {
 public:
  static constexpr size_t MAX_ENTRIES = 128;  // Balanced for Korean text while conserving memory

  struct CacheEntry {
    uint32_t codepoint;
    EpdGlyph glyph;
    bool valid;
  };

 private:
  CacheEntry entries[MAX_ENTRIES];
  size_t nextSlot;

 public:
  GlyphMetadataCache() : nextSlot(0) {
    for (size_t i = 0; i < MAX_ENTRIES; i++) {
      entries[i].valid = false;
    }
  }

  const EpdGlyph* get(uint32_t codepoint);
  const EpdGlyph* put(uint32_t codepoint, const EpdGlyph& glyph);
  void clear();
};

class SdFontData {
 private:
  std::string filePath;
  bool loaded;

  // Font metadata (loaded once, kept in RAM)
  EpdFontHeader header;
  // Resident whole-font preload. The .epdfont v1 file is small (a 11.6k-glyph
  // Korean face is ~2.2MB); this port no longer targets the RAM-constrained
  // ESP32 device, so the entire file is read into RAM at load() time and every
  // glyph/interval/bitmap lookup is served from memory — zero file I/O and no
  // per-glyph cache churn after mount. The device-era on-demand SD paths below
  // remain as a fallback when the resident copy is unavailable.
  mutable std::vector<uint8_t> residentData_;
  mutable bool resident_ = false;
  // Pre-converted glyph table (file format -> runtime EpdGlyph) for O(1)
  // metadata lookups without repeating the <<4 advanceX upcast per access.
  mutable std::vector<EpdGlyph> residentGlyphs_;
  mutable bool residentGlyphsReady_ = false;

  // Intervals are NOT held in RAM on the device path. A CJK font's interval
  // table is 50KB+ and a single contiguous allocation that fragments the heap —
  // starving the XTC page buffer and blocking large-font loads. findGlyphIndex()
  // binary-searches the table directly on the SD file instead. Only the
  // most-recently matched interval is cached so runs of same-script text
  // resolve with zero SD I/O.  With the resident preload above, the same search
  // runs over the in-RAM table instead.
  mutable EpdFontInterval lastInterval{};
  mutable bool lastIntervalValid = false;

  // Glyph metadata cache (per-font, small LRU cache)
  mutable GlyphMetadataCache glyphCache;

  // Bitmap cache (shared across all SdFontData instances)
  static GlyphBitmapCache* sharedCache;
  static int cacheRefCount;

  // Per-instance font identity, folded into the shared bitmap cache key so two
  // distinct SD fonts loaded at once (reader + system font) never alias glyphs.
  static uint32_t nextFontId;
  uint32_t fontId;

  // Build the shared-cache key for a codepoint within this font instance.
  uint64_t bitmapCacheKey(uint32_t codepoint) const { return (static_cast<uint64_t>(fontId) << 32) | codepoint; }

  // File handle for reading (opened on demand).
  // Upstream HalStorage renamed FsFile -> HalFile; use HalFile directly so the
  // typedef pulled in by HalStorage.h does not fight with SdFat's FsFile class.
  mutable HalFile fontFile;

  // Binary search for glyph index
  int findGlyphIndex(uint32_t codepoint) const;

  // Load a single glyph from SD card by index
  bool loadGlyphFromSD(int glyphIndex, EpdGlyph* outGlyph) const;

  // Ensure font file is open (keeps handle open for performance)
  bool ensureFileOpen() const;

 public:
  explicit SdFontData(const char* path);
  ~SdFontData();

  // Disable copy to prevent resource issues
  SdFontData(const SdFontData&) = delete;
  SdFontData& operator=(const SdFontData&) = delete;

  // Move constructor and assignment
  SdFontData(SdFontData&& other) noexcept;
  SdFontData& operator=(SdFontData&& other) noexcept;

  // Load font header and metadata from SD card
  bool load();
  bool isLoaded() const { return loaded; }

  // EpdFontData-compatible getters
  uint8_t getAdvanceY() const { return header.advanceY; }
  int8_t getAscender() const { return header.ascender; }
  int8_t getDescender() const { return header.descender; }
  bool is2Bit() const { return header.is2Bit != 0; }
  uint32_t getIntervalCount() const { return header.intervalCount; }
  uint32_t getGlyphCount() const { return header.glyphCount; }

  // Get glyph by codepoint (loads bitmap on demand)
  const EpdGlyph* getGlyph(uint32_t codepoint) const;

  // Returns true if this font actually contains codepoint (interval lookup only,
  // no SD bitmap/metadata load — safe to call cheaply for glyph-fallback decisions).
  bool hasGlyph(uint32_t codepoint) const { return loaded && findGlyphIndex(codepoint) >= 0; }

  // Get bitmap for a glyph (loads from SD if not cached)
  const uint8_t* getGlyphBitmap(uint32_t codepoint) const;

  // Static cache management
  static void setCacheSize(size_t maxBytes);
  static void clearCache();
  static size_t getCacheUsedSize();
};

/**
 * SD Card font class - similar interface to EpdFont but loads from SD card.
 */
class SdFont {
 private:
  SdFontData* data;
  bool ownsData;

 public:
  explicit SdFont(SdFontData* fontData, bool takeOwnership = false);
  explicit SdFont(const char* filePath);
  ~SdFont();

  // Disable copy
  SdFont(const SdFont&) = delete;
  SdFont& operator=(const SdFont&) = delete;

  // Move semantics
  SdFont(SdFont&& other) noexcept;
  SdFont& operator=(SdFont&& other) noexcept;

  bool load();
  bool isLoaded() const { return data && data->isLoaded(); }

  // EpdFont-compatible interface
  void getTextDimensions(const char* string, int* w, int* h) const;
  bool hasPrintableChars(const char* string) const;
  const EpdGlyph* getGlyph(uint32_t cp) const;
  // Returns true only if the font actually contains a real glyph for cp.
  bool hasGlyph(uint32_t cp) const { return data && data->hasGlyph(cp); }
  const uint8_t* getGlyphBitmap(uint32_t cp) const;

  // Metadata accessors
  uint8_t getAdvanceY() const { return data ? data->getAdvanceY() : 0; }
  int8_t getAscender() const { return data ? data->getAscender() : 0; }
  int8_t getDescender() const { return data ? data->getDescender() : 0; }
  bool is2Bit() const { return data ? data->is2Bit() : false; }

  SdFontData* getData() const { return data; }
};
