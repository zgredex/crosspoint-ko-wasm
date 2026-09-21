#pragma once
#include <HalStorage.h>
#include <Logging.h>

#include <deque>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

#include "InflateBudget.h"

class ZipFile {
 public:
  // ONE record shape for every walker. Four independent re-decodings of the same structure is how the
  // local-header offset came to be read two bytes late in one of them: the fix is a single parser, not four
  // sets of bounds checks that have to stay in sync by hand.
  struct CentralEntry {
    uint16_t flags = 0;
    uint16_t method = 0;
    uint32_t crc32 = 0;
    uint32_t compressedSize = 0;
    uint32_t uncompressedSize = 0;
    uint32_t localHeaderOffset = 0;
    std::string name;
  };

  struct FileStatSlim {
    uint16_t flags;              // General-purpose bit flags (encryption, data descriptor, ...)
    uint16_t method;             // Compression method
    uint32_t crc32;              // Central-directory CRC of the uncompressed member
    uint32_t compressedSize;     // Compressed size
    uint32_t uncompressedSize;   // Uncompressed size
    uint32_t localHeaderOffset;  // Offset of local file header
  };

  struct ZipDetails {
    uint32_t centralDirOffset;
    uint32_t centralDirSize;   // validated range of the central directory
    uint16_t totalEntries;
    bool isSet;
  };

  // Target for batch uncompressed size lookup (sorted by hash, then path).
  //
  // The PATH is carried, not a length. A 64-bit FNV-1a summary plus a length is a filter, not an identity:
  // with only those two the batch scanner had to accept a single same-length bucket member without ever
  // comparing the bytes, so a deliberate collision silently wrote one entry's size into another spine's
  // slot. Hash narrows; exact bytes decide.
  struct SizeTarget {
    uint64_t hash;       // FNV-1a 64-bit hash of the normalized path
    uint32_t index;      // Caller's index (e.g. spine index) — see MAX_BMC_INDEXED_ITEMS
    std::string path;    // the real path, for exact comparison before a size is committed
  };

  // FNV-1a 64-bit hash computed from char buffer (no std::string allocation)
  static uint64_t fnvHash64(const char* s, size_t len) {
    uint64_t hash = 14695981039346656037ull;
    for (size_t i = 0; i < len; i++) {
      hash ^= static_cast<uint8_t>(s[i]);
      hash *= 1099511628211ull;
    }
    return hash;
  }

 private:
  const std::string& filePath;
  HalFile file;
  ZipDetails zipDetails = {0, 0, false};
  std::unordered_map<std::string, FileStatSlim> fileStatSlimCache;

  // Cursor for sequential central-dir scanning optimization
  uint32_t lastCentralDirPos = 0;
  bool lastCentralDirPosValid = false;

  bool loadFileStatSlim(const char* filename, FileStatSlim* fileStat);
  // The ONE central-directory reader. Reads the fixed 46-byte record, validates the declared variable-length
  // tail against `cdEnd`, and consumes name + extra + comment. Returns false on any bounds violation, a bad
  // signature, or a short read.
  bool readCentralEntry(CentralEntry& out, size_t cdEnd);
  // expectName, when given, is compared against the LOCAL header's filename: a local record that names a
  // different file than the central directory contradicts itself, and the two describe the same bytes.
  long getDataOffset(const FileStatSlim& fileStat, const char* expectName = nullptr);
  bool loadZipDetails();

 public:
  explicit ZipFile(const std::string& filePath) : filePath(filePath) {}
  ~ZipFile() = default;
  // Zip file can be opened and closed by hand in order to allow for quick calculation of inflated file size
  // It is NOT recommended to pre-open it for any kind of inflation due to memory constraints
  bool isOpen() const { return !!file; }
  bool open();
  bool close();
  bool loadAllFileStatSlims();
  bool getInflatedFileSize(const char* filename, size_t* size);
  // Batch lookup: scan ZIP central dir once and fill sizes for matching targets.
  // targets must be sorted by (hash, path). sizes[target.index] receives uncompressedSize.
  // Returns number of targets matched.
  int fillUncompressedSizes(std::deque<SizeTarget>& targets, std::deque<uint32_t>& sizes);
  // Due to the memory required to run each of these, it is recommended to not preopen the zip file for multiple
  // These functions will open and close the zip as needed
  // maxOutputBytes bounds the DECLARED uncompressed size before anything is inflated: a decoder-side check
  // happens after the member has already expanded into memory-backed storage, which is too late.
  uint8_t* readFileToMemory(const char* filename, size_t* size, bool trailingNullByte,
                            size_t maxOutputBytes, InflateBudget& workBudget);
  // allowEarlyStop: a short write from `out` is treated as the sink asking to
  // stop (returns true) instead of a write failure — used by header probes
  // that only need the first bytes of an entry.
  bool readFileToStream(const char* filename, Print& out, size_t chunkSize, bool allowEarlyStop,
                        size_t maxOutputBytes, InflateBudget& workBudget);

  template <typename F>
  bool enumerateFilePaths(F&& callback) {
    if (!fileStatSlimCache.empty()) {
      for (const auto& entry : fileStatSlimCache) {
        callback(std::string_view{entry.first});
      }
      return true;
    }

    return enumerateFileEntries([&callback](std::string_view path, uint32_t, uint32_t) { callback(path); });
  }

  // Callback receives (path, crc32, compressedSize) for each central-directory entry.
  // Always scans the central directory: the slim-stat cache does not hold CRCs.
  //
  // This walker is on the ORDINARY loading path (Epub::discoverCssFilesFromZip), so it is not a rarely
  // exercised branch. It used to be `while (file.available())` with unchecked reads and seeks, which let a
  // hostile archive decide where its own directory ended — and it returned true for a truncated directory.
  // It now consumes the same checked record parser as every other walker, is bounded by the declared entry
  // count, rejects duplicate names, and reports a malformed directory as FAILURE.
  template <typename F>
  bool enumerateFileEntries(F&& callback) {
    const bool wasOpen = isOpen();
    if (!wasOpen && !open()) return false;

    const auto finish = [&](bool result) {
      if (!wasOpen) close();
      return result;
    };

    if (!loadZipDetails()) return finish(false);

    const size_t cdEnd = static_cast<size_t>(zipDetails.centralDirOffset) + zipDetails.centralDirSize;
    if (!file.seek(zipDetails.centralDirOffset)) return finish(false);

    std::unordered_set<std::string> seen;

    for (uint32_t i = 0; i < zipDetails.totalEntries; ++i) {
      CentralEntry e;
      if (!readCentralEntry(e, cdEnd)) return finish(false);

      if (!seen.insert(e.name).second) {
        LOG_ERR("ZIP", "duplicate central-directory name: %s", e.name.c_str());
        return finish(false);
      }

      callback(std::string_view{e.name}, e.crc32, e.compressedSize);
    }

    return finish(file.position() <= cdEnd);
  }
};
