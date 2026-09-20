#include "SdFont.h"

#include <Arduino.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Utf8.h>

#include <algorithm>
#include <cstring>
#include <limits>
#include <new>

// ============================================================================
// GlyphBitmapCache Implementation
// ============================================================================

GlyphBitmapCache::GlyphBitmapCache(size_t maxSize) : maxCacheSize(maxSize), currentSize(0) {}

GlyphBitmapCache::~GlyphBitmapCache() { clear(); }

void GlyphBitmapCache::evictOldest() {
  while (currentSize > maxCacheSize && !cacheList.empty()) {
    auto& oldest = cacheList.back();
    currentSize -= oldest.size;
    cacheMap.erase(oldest.key);
    free(oldest.bitmap);
    cacheList.pop_back();
  }
}

const uint8_t* GlyphBitmapCache::get(uint64_t key) {
  auto it = cacheMap.find(key);
  if (it == cacheMap.end()) {
    return nullptr;
  }

  // Move to front (most recently used)
  if (it->second != cacheList.begin()) {
    cacheList.splice(cacheList.begin(), cacheList, it->second);
  }

  return it->second->bitmap;
}

const uint8_t* GlyphBitmapCache::put(uint64_t key, const uint8_t* data, uint32_t size) {
  // Check if already cached
  auto it = cacheMap.find(key);
  if (it != cacheMap.end()) {
    // Move to front
    if (it->second != cacheList.begin()) {
      cacheList.splice(cacheList.begin(), cacheList, it->second);
    }
    return it->second->bitmap;
  }

  // Allocate and copy bitmap data
  uint8_t* bitmapCopy = static_cast<uint8_t*>(malloc(size));
  if (!bitmapCopy) {
    LOG_ERR("SDF", "Failed to allocate %u bytes for glyph cache", size);
    return nullptr;
  }
  memcpy(bitmapCopy, data, size);

  // Add to cache
  CacheEntry entry = {key, bitmapCopy, size};
  cacheList.push_front(entry);
  cacheMap[key] = cacheList.begin();
  currentSize += size;

  // Evict if over limit
  evictOldest();

  return bitmapCopy;
}

void GlyphBitmapCache::clear() {
  for (auto& entry : cacheList) {
    free(entry.bitmap);
  }
  cacheList.clear();
  cacheMap.clear();
  currentSize = 0;
}

// ============================================================================
// GlyphMetadataCache Implementation (simple fixed-size circular buffer)
// ============================================================================

const EpdGlyph* GlyphMetadataCache::get(uint32_t codepoint) {
  // Linear search through cache (simple but effective for small cache)
  for (size_t i = 0; i < MAX_ENTRIES; i++) {
    if (entries[i].valid && entries[i].codepoint == codepoint) {
      return &entries[i].glyph;
    }
  }
  return nullptr;
}

const EpdGlyph* GlyphMetadataCache::put(uint32_t codepoint, const EpdGlyph& glyph) {
  // Check if already cached
  for (size_t i = 0; i < MAX_ENTRIES; i++) {
    if (entries[i].valid && entries[i].codepoint == codepoint) {
      return &entries[i].glyph;
    }
  }

  // Add to next slot (circular overwrite)
  entries[nextSlot].codepoint = codepoint;
  entries[nextSlot].glyph = glyph;
  entries[nextSlot].valid = true;

  const EpdGlyph* result = &entries[nextSlot].glyph;
  nextSlot = (nextSlot + 1) % MAX_ENTRIES;
  return result;
}

void GlyphMetadataCache::clear() {
  for (size_t i = 0; i < MAX_ENTRIES; i++) {
    entries[i].valid = false;
  }
  nextSlot = 0;
}

// ============================================================================
// SdFontData Implementation
// ============================================================================

// Static members
GlyphBitmapCache* SdFontData::sharedCache = nullptr;
int SdFontData::cacheRefCount = 0;
uint32_t SdFontData::nextFontId = 0;

SdFontData::SdFontData(const char* path) : filePath(path), loaded(false), fontId(nextFontId++) {
  memset(&header, 0, sizeof(header));

  // Initialize shared cache on first SdFontData creation
  // Host build: RAM is not constrained — use a generous cache for any glyph
  // bitmaps that still flow through the non-resident fallback path.
  if (sharedCache == nullptr) {
    sharedCache = new GlyphBitmapCache(4 * 1024 * 1024);  // 4MB
  }
  cacheRefCount++;
}

SdFontData::~SdFontData() {
  if (fontFile) {
    fontFile.close();
  }

  // Cleanup shared cache when last SdFontData is destroyed
  cacheRefCount--;
  if (cacheRefCount == 0 && sharedCache != nullptr) {
    delete sharedCache;
    sharedCache = nullptr;
  }
}

SdFontData::SdFontData(SdFontData&& other) noexcept
    : filePath(std::move(other.filePath)),
      loaded(other.loaded),
      header(other.header),
      residentData_(std::move(other.residentData_)),
      resident_(other.resident_),
      residentGlyphs_(std::move(other.residentGlyphs_)),
      residentGlyphsReady_(other.residentGlyphsReady_),
      fontId(other.fontId) {  // inherit identity so already-cached glyphs stay valid
  other.loaded = false;
  other.resident_ = false;
  other.residentGlyphsReady_ = false;
  cacheRefCount++;  // New instance references the cache
}

SdFontData& SdFontData::operator=(SdFontData&& other) noexcept {
  if (this != &other) {
    // Clean up current resources
    if (fontFile) {
      fontFile.close();
    }

    // Move from other
    filePath = std::move(other.filePath);
    loaded = other.loaded;
    header = other.header;
    residentData_ = std::move(other.residentData_);
    resident_ = other.resident_;
    residentGlyphs_ = std::move(other.residentGlyphs_);
    residentGlyphsReady_ = other.residentGlyphsReady_;
    fontId = other.fontId;  // inherit identity so already-cached glyphs stay valid
    lastIntervalValid = false;

    other.loaded = false;
    other.resident_ = false;
    other.residentGlyphsReady_ = false;
  }
  return *this;
}

// Maximum reasonable values for validation
// CJK fonts (Korean + Chinese + Japanese) can have 120K+ glyphs
// Glyphs are loaded on-demand from SD, so high count doesn't affect memory
static constexpr uint32_t MAX_INTERVAL_COUNT = 10000;
static constexpr uint32_t MAX_GLYPH_COUNT = 150000;
// Runtime custom fonts are copied into HalStorage and, on the host/WASM port,
// normally preloaded once more for zero-I/O glyph access.  Keep the parser's
// own ceiling aligned with the public API so a path-based caller cannot bypass
// the boundary check and force two unbounded allocations.
static constexpr size_t MAX_EPDFONT_FILE_BYTES = 64u * 1024u * 1024u;

bool SdFontData::load() {
  if (loaded) {
    return true;
  }

  // Open font file
  if (!Storage.openFileForRead("SdFont", filePath.c_str(), fontFile)) {
    LOG_ERR("SDF", "Failed to open font file: %s", filePath.c_str());
    return false;
  }

  // Validate into locals and commit to the object only after the complete v1
  // structure has been proved.  A failed load must never leave `loaded` true or
  // partially trusted offsets in `header`.
  EpdFontHeader candidateHeader{};
  const auto fail = [&](const char* why) {
    LOG_ERR("SDF", "%s: %s", why, filePath.c_str());
    fontFile.close();
    residentData_.clear();
    residentGlyphs_.clear();
    resident_ = false;
    residentGlyphsReady_ = false;
    loaded = false;
    memset(&header, 0, sizeof(header));
    return false;
  };

  if (fontFile.read(&candidateHeader, sizeof(candidateHeader)) != sizeof(candidateHeader)) {
    LOG_ERR("SDF", "Failed to read header from: %s", filePath.c_str());
    return fail("short epdfont header");
  }

  // Validate magic number
  if (candidateHeader.magic != EPDFONT_MAGIC) {
    LOG_ERR("SDF", "Invalid magic: 0x%08X (expected 0x%08X)", candidateHeader.magic, EPDFONT_MAGIC);
    return fail("invalid epdfont magic");
  }

  // Validate version
  if (candidateHeader.version != EPDFONT_VERSION) {
    LOG_ERR("SDF", "Bad version: %u (expected %u)", candidateHeader.version, EPDFONT_VERSION);
    return fail("unsupported epdfont version");
  }

  const size_t fileBytes = fontFile.size();
  if (fileBytes < sizeof(EpdFontHeader) || fileBytes > MAX_EPDFONT_FILE_BYTES) {
    return fail("epdfont file size is outside the supported range");
  }
  if (candidateHeader.is2Bit > 1 || candidateHeader.advanceY == 0) {
    return fail("invalid epdfont metrics");
  }
  if (candidateHeader.intervalCount == 0 || candidateHeader.intervalCount > MAX_INTERVAL_COUNT) {
    LOG_ERR("SDF", "Invalid interval count: %u (max %u)", candidateHeader.intervalCount, MAX_INTERVAL_COUNT);
    return fail("invalid epdfont interval count");
  }
  if (candidateHeader.glyphCount == 0 || candidateHeader.glyphCount > MAX_GLYPH_COUNT) {
    LOG_ERR("SDF", "Invalid glyph count: %u (max %u)", candidateHeader.glyphCount, MAX_GLYPH_COUNT);
    return fail("invalid epdfont glyph count");
  }

  // Version 1 has one canonical packed layout.  Compute every boundary in 64
  // bits before comparing it to the 32-bit fields; otherwise a malicious count
  // can wrap an offset back into the header.
  const uint64_t expectedIntervals = sizeof(EpdFontHeader);
  const uint64_t expectedGlyphs = expectedIntervals +
                                  static_cast<uint64_t>(candidateHeader.intervalCount) * sizeof(EpdFontInterval);
  const uint64_t expectedBitmap = expectedGlyphs +
                                  static_cast<uint64_t>(candidateHeader.glyphCount) * sizeof(EpdFontGlyph);
  if (expectedBitmap > fileBytes ||
      candidateHeader.intervalsOffset != expectedIntervals ||
      candidateHeader.glyphsOffset != expectedGlyphs ||
      candidateHeader.bitmapOffset != expectedBitmap) {
    return fail("non-canonical or out-of-range epdfont offsets");
  }

  // The binary search requires sorted, disjoint intervals.  Requiring each
  // interval's glyph offset to equal the cumulative span proves both that no
  // interval points past the glyph table and that every glyph is covered once.
  uint64_t coveredGlyphs = 0;
  uint32_t previousLast = 0;
  for (uint32_t i = 0; i < candidateHeader.intervalCount; ++i) {
    const uint64_t pos = expectedIntervals + static_cast<uint64_t>(i) * sizeof(EpdFontInterval);
    EpdFontInterval interval{};
    if (pos > fileBytes - sizeof(interval) || !fontFile.seekSet(static_cast<size_t>(pos)) ||
        fontFile.read(&interval, sizeof(interval)) != static_cast<int>(sizeof(interval))) {
      return fail("truncated epdfont interval table");
    }
    if (interval.first > interval.last || interval.last > 0x10ffffu ||
        (i != 0 && interval.first <= previousLast) || interval.offset != coveredGlyphs) {
      return fail("invalid epdfont interval ordering or glyph offset");
    }
    const uint64_t span = static_cast<uint64_t>(interval.last) - interval.first + 1u;
    if (coveredGlyphs > candidateHeader.glyphCount ||
        span > static_cast<uint64_t>(candidateHeader.glyphCount) - coveredGlyphs) {
      return fail("epdfont interval exceeds the glyph table");
    }
    coveredGlyphs += span;
    previousLast = interval.last;
  }
  if (coveredGlyphs != candidateHeader.glyphCount) {
    return fail("epdfont intervals do not cover the glyph table");
  }

  // Runtime EpdGlyph::dataLength is uint16_t.  Reject, never truncate, a v1
  // uint32_t length that cannot be represented.  Every bitmap span is checked
  // relative to bitmapOffset with widened arithmetic before later pointer use.
  const uint64_t bitmapBytes = fileBytes - expectedBitmap;
  for (uint32_t i = 0; i < candidateHeader.glyphCount; ++i) {
    const uint64_t pos = expectedGlyphs + static_cast<uint64_t>(i) * sizeof(EpdFontGlyph);
    EpdFontGlyph glyph{};
    if (pos > fileBytes - sizeof(glyph) || !fontFile.seekSet(static_cast<size_t>(pos)) ||
        fontFile.read(&glyph, sizeof(glyph)) != static_cast<int>(sizeof(glyph))) {
      return fail("truncated epdfont glyph table");
    }
    if (glyph.dataLength > std::numeric_limits<uint16_t>::max() ||
        static_cast<uint64_t>(glyph.dataOffset) > bitmapBytes ||
        static_cast<uint64_t>(glyph.dataLength) > bitmapBytes - glyph.dataOffset) {
      return fail("epdfont glyph bitmap lies outside the file");
    }
  }

  // COMMIT.  The on-demand path below now consumes only offsets and records
  // that were fully validated above.
  header = candidateHeader;
  loaded = true;
  lastIntervalValid = false;

  // ---- Resident whole-font preload (no longer RAM-constrained) ----
  // Slurp the entire .epdfont into RAM so every subsequent glyph/interval/
  // bitmap lookup is pointer arithmetic in memory. The device-era on-demand
  // paths remain below as fallback (used only if this read fails).
  if (fileBytes >= sizeof(EpdFontHeader)) {
    residentData_.resize(fileBytes);
    if (fontFile.seekSet(0) &&
        fontFile.read(residentData_.data(), fileBytes) == static_cast<int>(fileBytes)) {
      resident_ = true;
    } else {
      residentData_.clear();
      resident_ = false;
    }
  }

  // Pre-convert the glyph records (file 16B layout -> runtime EpdGlyph with the
  // integer-px -> 12.4 fp4 advanceX upcast) so getGlyph() is a table index,
  // not a per-call file read + conversion.
  if (resident_ && header.glyphCount <= MAX_GLYPH_COUNT &&
      residentData_.size() >= static_cast<size_t>(header.glyphsOffset) +
                                  static_cast<size_t>(header.glyphCount) * sizeof(EpdFontGlyph)) {
    residentGlyphs_.resize(header.glyphCount);
    const uint8_t* src = residentData_.data() + header.glyphsOffset;
    for (uint32_t i = 0; i < header.glyphCount; i++) {
      EpdFontGlyph fg;
      memcpy(&fg, src + static_cast<size_t>(i) * sizeof(EpdFontGlyph), sizeof(fg));
      EpdGlyph& g = residentGlyphs_[i];
      g.width = fg.width;
      g.height = fg.height;
      g.advanceX = static_cast<uint16_t>(fg.advanceX) << fp4::FRAC_BITS;
      g.left = fg.left;
      g.top = fg.top;
      g.dataLength = static_cast<uint16_t>(fg.dataLength);
      g.dataOffset = fg.dataOffset;
    }
    residentGlyphsReady_ = true;
  } else {
    residentGlyphsReady_ = false;
  }

  LOG_DBG("SDF", "Loaded: %s (advanceY=%u, %u intervals, %u glyphs, %s)",
          filePath.c_str(), header.advanceY, header.intervalCount, header.glyphCount,
          resident_ ? "resident" : "on-demand");

  return true;
}

bool SdFontData::ensureFileOpen() const {
  if (fontFile && fontFile.isOpen()) {
    return true;
  }
  return Storage.openFileForRead("SdFont", filePath.c_str(), fontFile);
}

bool SdFontData::loadGlyphFromSD(int glyphIndex, EpdGlyph* outGlyph) const {
  if (!loaded || outGlyph == nullptr || glyphIndex < 0 || glyphIndex >= static_cast<int>(header.glyphCount)) {
    return false;
  }

  // Keep file open for better performance
  if (!ensureFileOpen()) {
    return false;
  }

  // Calculate position in file
  const uint64_t glyphFileOffset = static_cast<uint64_t>(header.glyphsOffset) +
                                   static_cast<uint64_t>(glyphIndex) * sizeof(EpdFontGlyph);
  if (glyphFileOffset > fontFile.size() || sizeof(EpdFontGlyph) > fontFile.size() - glyphFileOffset) return false;

  if (!fontFile.seekSet(static_cast<size_t>(glyphFileOffset))) {
    return false;
  }

  // Read the glyph from file format
  EpdFontGlyph fileGlyph;
  if (fontFile.read(&fileGlyph, sizeof(EpdFontGlyph)) != sizeof(EpdFontGlyph)) {
    return false;
  }

  // Convert from file format to runtime format.
  // .epdfont v1 stores advanceX as integer pixels (uint8); EpdGlyph.advanceX is
  // 12.4 fixed-point pixels (fp4). Shift left by 4 to upcast pixels -> fp4.
  outGlyph->width = fileGlyph.width;
  outGlyph->height = fileGlyph.height;
  outGlyph->advanceX = static_cast<uint16_t>(fileGlyph.advanceX) << fp4::FRAC_BITS;
  outGlyph->left = fileGlyph.left;
  outGlyph->top = fileGlyph.top;
  outGlyph->dataLength = static_cast<uint16_t>(fileGlyph.dataLength);
  outGlyph->dataOffset = fileGlyph.dataOffset;

  return true;
}

int SdFontData::findGlyphIndex(uint32_t codepoint) const {
  if (!loaded) {
    return -1;
  }

  // Fast path: codepoint falls inside the most recently matched interval. Runs
  // of same-script text (e.g. a Hangul paragraph) stay in one interval, so this
  // resolves with zero SD I/O for the common case.
  if (lastIntervalValid && codepoint >= lastInterval.first && codepoint <= lastInterval.last) {
    return static_cast<int>(lastInterval.offset + (codepoint - lastInterval.first));
  }

  if (header.intervalCount == 0) {
    return -1;
  }

  // Resident path: binary search the interval table in RAM (no file I/O).
  const uint8_t* table =
      resident_ ? residentData_.data() + header.intervalsOffset : nullptr;
  if (table == nullptr && !ensureFileOpen()) {
    return -1;
  }

  auto readInterval = [&](uint32_t index, EpdFontInterval& out) -> bool {
    const size_t off = static_cast<size_t>(index) * sizeof(EpdFontInterval);
    if (resident_) {
      const size_t tableStart = static_cast<size_t>(header.intervalsOffset);
      if (tableStart > residentData_.size() || off > residentData_.size() - tableStart ||
          sizeof(out) > residentData_.size() - tableStart - off) return false;
      memcpy(&out, table + off, sizeof(out));
      return true;
    }
    const uint64_t intervalOffset = static_cast<uint64_t>(header.intervalsOffset) +
                                    static_cast<uint64_t>(index) * sizeof(EpdFontInterval);
    if (intervalOffset > fontFile.size() || sizeof(out) > fontFile.size() - intervalOffset) return false;
    return fontFile.seekSet(static_cast<size_t>(intervalOffset)) &&
           fontFile.read(&out, sizeof(out)) == static_cast<int>(sizeof(out));
  };

  int left = 0;
  int right = static_cast<int>(header.intervalCount) - 1;

  while (left <= right) {
    const int mid = left + (right - left) / 2;

    EpdFontInterval interval;
    if (!readInterval(mid, interval)) {
      return -1;
    }

    if (codepoint < interval.first) {
      right = mid - 1;
    } else if (codepoint > interval.last) {
      left = mid + 1;
    } else {
      // Found: cache this interval for subsequent same-interval lookups.
      lastInterval = interval;
      lastIntervalValid = true;
      return static_cast<int>(interval.offset + (codepoint - interval.first));
    }
  }

  return -1;  // Not found
}

const EpdGlyph* SdFontData::getGlyph(uint32_t codepoint) const {
  if (!loaded) {
    return nullptr;
  }

  // Resident path: the whole glyph table was converted at load() — a direct
  // index into residentGlyphs_, no file I/O, no per-call conversion, no
  // metadata-cache churn (pointers stay valid for the font's lifetime).
  if (residentGlyphsReady_) {
    const int index = findGlyphIndex(codepoint);
    if (index < 0 || index >= static_cast<int>(residentGlyphs_.size())) {
      return nullptr;
    }
    return &residentGlyphs_[static_cast<size_t>(index)];
  }

  // Check cache first
  const EpdGlyph* cached = glyphCache.get(codepoint);
  if (cached != nullptr) {
    return cached;
  }

  // Find glyph index using binary search on intervals
  int index = findGlyphIndex(codepoint);
  if (index < 0 || index >= static_cast<int>(header.glyphCount)) {
    return nullptr;
  }

  // Load glyph from SD card
  EpdGlyph glyph;
  if (!loadGlyphFromSD(index, &glyph)) {
    return nullptr;
  }

  // Store in cache and return pointer to cached copy
  return glyphCache.put(codepoint, glyph);
}

const uint8_t* SdFontData::getGlyphBitmap(uint32_t codepoint) const {
  if (!loaded || sharedCache == nullptr) {
    return nullptr;
  }

  // Find glyph index
  int glyphIndex = findGlyphIndex(codepoint);
  if (glyphIndex < 0 || glyphIndex >= static_cast<int>(header.glyphCount)) {
    return nullptr;
  }

  // Resident path: glyph metadata is pre-converted and bitmap bytes are already
  // in RAM — return a pointer straight into the resident buffer. Zero copy,
  // zero allocation, and the pointer stays valid for the font's lifetime
  // (strictly stronger than the cache contract used by the fallback below).
  if (residentGlyphsReady_) {
    const EpdGlyph& meta = residentGlyphs_[static_cast<size_t>(glyphIndex)];
    if (meta.dataLength == 0) {
      return nullptr;
    }
    const uint64_t off = static_cast<uint64_t>(header.bitmapOffset) + meta.dataOffset;
    if (off > residentData_.size() || meta.dataLength > residentData_.size() - off) {
      return nullptr;
    }
    return residentData_.data() + static_cast<size_t>(off);
  }

  // Check cache first (keyed by font identity + codepoint to avoid cross-font aliasing)
  const uint64_t cacheKey = bitmapCacheKey(codepoint);
  const uint8_t* cached = sharedCache->get(cacheKey);
  if (cached != nullptr) {
    return cached;
  }

  // Ensure file is open (keeps file handle open for performance)
  if (!ensureFileOpen()) {
    return nullptr;
  }

  // Read glyph metadata first (we need dataLength and dataOffset)
  const uint64_t glyphFileOffset = static_cast<uint64_t>(header.glyphsOffset) +
                                   static_cast<uint64_t>(glyphIndex) * sizeof(EpdFontGlyph);
  if (glyphFileOffset > fontFile.size() || sizeof(EpdFontGlyph) > fontFile.size() - glyphFileOffset ||
      !fontFile.seekSet(static_cast<size_t>(glyphFileOffset))) {
    return nullptr;
  }

  EpdFontGlyph fileGlyph;
  if (fontFile.read(&fileGlyph, sizeof(EpdFontGlyph)) != sizeof(EpdFontGlyph)) {
    return nullptr;
  }

  if (fileGlyph.dataLength == 0) {
    return nullptr;
  }

  // Seek to bitmap data
  const uint64_t bitmapFileOffset = static_cast<uint64_t>(header.bitmapOffset) + fileGlyph.dataOffset;
  if (fileGlyph.dataLength > std::numeric_limits<uint16_t>::max() || bitmapFileOffset > fontFile.size() ||
      fileGlyph.dataLength > fontFile.size() - bitmapFileOffset ||
      !fontFile.seekSet(static_cast<size_t>(bitmapFileOffset))) {
    return nullptr;
  }

  // Allocate temporary buffer for reading
  uint8_t* tempBuffer = static_cast<uint8_t*>(malloc(fileGlyph.dataLength));
  if (!tempBuffer) {
    return nullptr;
  }

  if (fontFile.read(tempBuffer, fileGlyph.dataLength) != static_cast<int>(fileGlyph.dataLength)) {
    free(tempBuffer);
    return nullptr;
  }

  // File stays open for next glyph read (performance optimization)

  // Store in cache
  const uint8_t* result = sharedCache->put(cacheKey, tempBuffer, fileGlyph.dataLength);
  free(tempBuffer);

  return result;
}

void SdFontData::setCacheSize(size_t maxBytes) {
  if (sharedCache != nullptr) {
    delete sharedCache;
  }
  sharedCache = new GlyphBitmapCache(maxBytes);
}

void SdFontData::clearCache() {
  if (sharedCache != nullptr) {
    sharedCache->clear();
  }
}

size_t SdFontData::getCacheUsedSize() {
  if (sharedCache != nullptr) {
    return sharedCache->getUsedSize();
  }
  return 0;
}

// ============================================================================
// SdFont Implementation
// ============================================================================

SdFont::SdFont(SdFontData* fontData, bool takeOwnership) : data(fontData), ownsData(takeOwnership) {}

SdFont::SdFont(const char* filePath) : data(new SdFontData(filePath)), ownsData(true) {}

SdFont::~SdFont() {
  if (ownsData) {
    delete data;
  }
}

SdFont::SdFont(SdFont&& other) noexcept : data(other.data), ownsData(other.ownsData) {
  other.data = nullptr;
  other.ownsData = false;
}

SdFont& SdFont::operator=(SdFont&& other) noexcept {
  if (this != &other) {
    if (ownsData) {
      delete data;
    }
    data = other.data;
    ownsData = other.ownsData;
    other.data = nullptr;
    other.ownsData = false;
  }
  return *this;
}

bool SdFont::load() {
  if (data == nullptr) {
    return false;
  }
  return data->load();
}

void SdFont::getTextDimensions(const char* string, int* w, int* h) const {
  *w = 0;
  *h = 0;

  if (data == nullptr || !data->isLoaded() || string == nullptr || *string == '\0') {
    return;
  }

  int minX = 0, minY = 0, maxX = 0, maxY = 0;
  int cursorX = 0;
  const int cursorY = 0;

  uint32_t cp;
  while ((cp = utf8NextCodepoint(reinterpret_cast<const uint8_t**>(&string)))) {
    const EpdGlyph* glyph = data->getGlyph(cp);
    if (!glyph) {
      glyph = data->getGlyph('?');
    }
    if (!glyph) {
      continue;
    }

    minX = std::min(minX, cursorX + glyph->left);
    maxX = std::max(maxX, cursorX + glyph->left + glyph->width);
    minY = std::min(minY, cursorY + glyph->top - glyph->height);
    maxY = std::max(maxY, cursorY + glyph->top);
    cursorX += fp4::toPixel(glyph->advanceX);  // advanceX is 12.4 fixed-point pixels
  }

  *w = maxX - minX;
  *h = maxY - minY;
}

bool SdFont::hasPrintableChars(const char* string) const {
  int w = 0, h = 0;
  getTextDimensions(string, &w, &h);
  return w > 0 || h > 0;
}

const EpdGlyph* SdFont::getGlyph(uint32_t cp) const {
  if (data == nullptr) {
    return nullptr;
  }
  return data->getGlyph(cp);
}

const uint8_t* SdFont::getGlyphBitmap(uint32_t cp) const {
  if (data == nullptr) {
    return nullptr;
  }
  return data->getGlyphBitmap(cp);
}
