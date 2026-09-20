#include "ZipFile.h"

#include <HalStorage.h>
#include <InflateStream.h>
#include <Logging.h>

#include <algorithm>
#include <limits>

struct ZipInflateCtx {
  HalFile* file = nullptr;
  size_t fileRemaining = 0;
  uint8_t* readBuf = nullptr;
  size_t readBufSize = 0;
};

namespace {
constexpr uint16_t ZIP_METHOD_STORED = 0;
constexpr uint16_t ZIP_METHOD_DEFLATED = 8;

// Byte-safe little-endian readers. The previous code used `*reinterpret_cast<uint32_t*>(&buffer[i])`,
// which is unaligned-access AND strict-aliasing undefined behaviour on native builds, and silently
// depended on the host's alignment tolerance for archive metadata that a caller can author freely.
inline uint16_t le16(const uint8_t* p) {
  return static_cast<uint16_t>(static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1]) << 8));
}
inline uint32_t le32(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
         (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

// CRC-32 (IEEE 802.3), computed bitwise so no 1 KiB table sits in the binary. A damaged DEFLATE stream can
// still inflate "successfully" into wrong bytes, and for a converter whose output must be a function of the
// EPUB bytes, silent corruption is worse than a clean rejection.
inline uint32_t crc32Update(uint32_t crc, const uint8_t* p, size_t n) {
  crc = ~crc;
  while (n--) {
    crc ^= *p++;
    for (int k = 0; k < 8; k++) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
  }
  return ~crc;
}

constexpr uint32_t ZIP_SIG_EOCD = 0x06054b50;
constexpr uint32_t ZIP_SIG_CENTRAL = 0x02014b50;
constexpr uint32_t ZIP_SIG_LOCAL = 0x04034b50;
constexpr size_t ZIP_EOCD_MIN = 22;
constexpr size_t ZIP_MAX_COMMENT = 65535;    // a ZIP comment is a uint16_t length
constexpr size_t ZIP_CD_MIN_ENTRY = 46;      // smallest possible central-directory record
// Parsing the whole central directory once is the trust boundary for every
// later lookup.  These limits bound the validation set itself: without them,
// duplicate-name detection could become the archive's next memory attack.
constexpr uint32_t MAX_ZIP_ENTRIES = 32768;
constexpr size_t MAX_ZIP_CENTRAL_DIRECTORY_BYTES = 32u * 1024u * 1024u;
constexpr size_t MAX_ZIP_TOTAL_NAME_BYTES = 8u * 1024u * 1024u;

// RAII zip: opens the zip if not already open, closes on destruction only if
// it performed the open.  Removes the wasOpen/close boilerplate from every method.
class ScopedOpenClose final {
 public:
  [[nodiscard]] explicit ScopedOpenClose(ZipFile& zf) : zf(zf), needsClose(!zf.isOpen()) {
    if (needsClose) ok = zf.open();
  }
  ~ScopedOpenClose() {
    if (needsClose && ok) zf.close();
  }
  ScopedOpenClose(const ScopedOpenClose&) = delete;
  ScopedOpenClose& operator=(const ScopedOpenClose&) = delete;
  ScopedOpenClose(ScopedOpenClose&&) = delete;
  ScopedOpenClose& operator=(ScopedOpenClose&&) = delete;
  explicit operator bool() const { return ok || !needsClose; }

 private:
  ZipFile& zf;
  bool needsClose = false;
  bool ok = true;  // true when zip was already open (no open() call needed)
};

// HalFile::read() returns int and reports failure as -1. Assigning that straight to a size_t (as this
// did) turns a failed range read into SIZE_MAX, underflows fileRemaining by SIZE_MAX + 1, and then hands
// the inflater an enormous input block. Every read() consumer below is checked while the value is still
// signed.
size_t zipFillCallback(void* vctx, const uint8_t** data) {
  auto* ctx = static_cast<ZipInflateCtx*>(vctx);
  *data = nullptr;

  if (!ctx || !ctx->file || ctx->readBuf == nullptr || ctx->readBufSize == 0) return 0;
  if (ctx->fileRemaining == 0) return 0;

  const size_t toRead = ctx->fileRemaining < ctx->readBufSize ? ctx->fileRemaining : ctx->readBufSize;
  const int got = ctx->file->read(ctx->readBuf, toRead);
  if (got <= 0) return 0;                       // I/O error and EOF are both "no more input"

  const size_t bytesRead = static_cast<size_t>(got);
  if (bytesRead > toRead) return 0;             // impossible unless HalFile::read breaks its contract

  ctx->fileRemaining -= bytesRead;

  *data = ctx->readBuf;
  return bytesRead;
}
}  // namespace

bool ZipFile::loadAllFileStatSlims() {
  const ScopedOpenClose zip{*this};
  if (!zip) return false;

  if (!loadZipDetails()) return false;

  if (!file.seek(zipDetails.centralDirOffset)) return false;

  const size_t cdEnd = static_cast<size_t>(zipDetails.centralDirOffset) + zipDetails.centralDirSize;

  // TRANSACTIONAL: build a candidate and commit only after the WHOLE declared directory parsed. A version
  // that broke out of the loop on the first bad record and still returned true made the cache authoritative
  // while holding only the entries up to the corruption — every later entry then looked absent.
  std::unordered_map<std::string, FileStatSlim> candidate;
  candidate.reserve(zipDetails.totalEntries);

  for (uint32_t i = 0; i < zipDetails.totalEntries; ++i) {
    CentralEntry e;
    if (!readCentralEntry(e, cdEnd)) {
      fileStatSlimCache.clear();
      return false;
    }

    FileStatSlim fileStat = {};
    fileStat.flags = e.flags;
    fileStat.method = e.method;
    fileStat.crc32 = e.crc32;
    fileStat.compressedSize = e.compressedSize;
    fileStat.uncompressedSize = e.uncompressedSize;
    fileStat.localHeaderOffset = e.localHeaderOffset;

    if (!candidate.emplace(e.name, fileStat).second) {
      LOG_ERR("ZIP", "duplicate central-directory name: %s", e.name.c_str());
      fileStatSlimCache.clear();
      return false;
    }
  }

  // COMMIT — only after every declared record passed.
  fileStatSlimCache.swap(candidate);
  // Set cursor to start of central directory for sequential access
  lastCentralDirPos = zipDetails.centralDirOffset;
  lastCentralDirPosValid = true;

  return true;
}

// The single checked central-directory record reader. Every walker below consumes this instead of decoding
// the fixed offsets itself: four independent re-decodings is how one of them came to read the local-header
// offset two bytes late, and bounds checks that must stay in sync by hand are bounds checks that will drift.
bool ZipFile::readCentralEntry(CentralEntry& out, size_t cdEnd) {
  const size_t here = file.position();
  if (here > cdEnd || cdEnd - here < ZIP_CD_MIN_ENTRY) return false;

  uint8_t fixed[ZIP_CD_MIN_ENTRY];
  if (file.read(fixed, sizeof(fixed)) != static_cast<int>(sizeof(fixed))) return false;
  if (le32(fixed) != ZIP_SIG_CENTRAL) return false;

  out.flags = le16(fixed + 8);
  out.method = le16(fixed + 10);
  out.crc32 = le32(fixed + 16);
  out.compressedSize = le32(fixed + 20);
  out.uncompressedSize = le32(fixed + 24);

  const uint16_t nameLen = le16(fixed + 28);
  const uint16_t extraLen = le16(fixed + 30);
  const uint16_t commentLen = le16(fixed + 32);
  out.localHeaderOffset = le32(fixed + 42);            // measured: byte 42, not 44

  constexpr size_t MAX_ZIP_PATH = 4096;
  if (nameLen == 0 || nameLen > MAX_ZIP_PATH) return false;

  // The declared variable-length tail must fit inside the declared directory — a name/extra/comment that
  // runs past cdEnd means the next "record" is really this record's leftovers.
  if (static_cast<uint64_t>(nameLen) + extraLen + commentLen > cdEnd - file.position()) return false;

  out.name.resize(nameLen);
  if (file.read(out.name.data(), nameLen) != static_cast<int>(nameLen)) return false;
  if (!file.seekCur(static_cast<int64_t>(extraLen) + commentLen)) return false;

  return file.position() <= cdEnd;
}

bool ZipFile::loadFileStatSlim(const char* filename, FileStatSlim* fileStat) {
  if (!fileStatSlimCache.empty()) {
    const auto it = fileStatSlimCache.find(filename);
    if (it != fileStatSlimCache.end()) {
      *fileStat = it->second;
      return true;
    }
    return false;
  }

  const ScopedOpenClose zip{*this};
  if (!zip) return false;

  if (!loadZipDetails()) return false;

  // Phase 1: Try scanning from cursor position first
  uint32_t startPos = lastCentralDirPosValid ? lastCentralDirPos : zipDetails.centralDirOffset;
  bool wrapped = false;
  bool found = false;

  if (!file.seek(startPos)) return false;

  const size_t cdEnd = static_cast<size_t>(zipDetails.centralDirOffset) + zipDetails.centralDirSize;
  uint32_t walked = 0;

  while (true) {
    // A hostile central directory must not be able to keep this loop running: it is bounded by the entry
    // count the EOCD declared, and every record it hands out is bounds-checked against the declared end.
    if (++walked > static_cast<uint32_t>(zipDetails.totalEntries) + 1u) break;

    const size_t entryStart = file.position();
    // If we've wrapped and reached our start position, stop
    if (wrapped && entryStart >= startPos) break;

    CentralEntry e;
    if (!readCentralEntry(e, cdEnd)) {
      // No usable record here: either the end of the declared directory (normal) or a malformed record. The
      // sequential-cursor optimization resumes from the middle, so wrapping once is what makes an entry
      // BEFORE the cursor reachable at all.
      if (!wrapped && lastCentralDirPosValid && startPos != zipDetails.centralDirOffset) {
        if (!file.seek(zipDetails.centralDirOffset)) break;
        wrapped = true;
        continue;
      }
      break;
    }

    if (e.name == filename) {
      lastCentralDirPos = static_cast<uint32_t>(file.position());
      lastCentralDirPosValid = true;
      fileStat->flags = e.flags;
      fileStat->method = e.method;
      fileStat->crc32 = e.crc32;
      fileStat->compressedSize = e.compressedSize;
      fileStat->uncompressedSize = e.uncompressedSize;
      fileStat->localHeaderOffset = e.localHeaderOffset;
      found = true;
      break;
    }
  }

  return found;
}

long ZipFile::getDataOffset(const FileStatSlim& fileStat, const char* expectName) {
  const ScopedOpenClose zip{*this};
  if (!zip) return -1;

  constexpr auto localHeaderSize = 30;

  uint8_t pLocalHeader[localHeaderSize];
  const uint64_t fileOffset = fileStat.localHeaderOffset;

  // The local-header offset is central-directory metadata too: a hostile value must fail here rather
  // than seek somewhere arbitrary and be interpreted as a header.
  if (fileOffset > file.size() || file.size() - fileOffset < localHeaderSize) {
    LOG_ERR("ZIP", "Local header lies outside the file");
    return -1;
  }
  if (!file.seek(static_cast<size_t>(fileOffset))) {
    LOG_ERR("ZIP", "Failed to seek to the local header");
    return -1;
  }
  const int got = file.read(pLocalHeader, localHeaderSize);

  if (got < 0 || static_cast<size_t>(got) != localHeaderSize) {
    LOG_ERR("ZIP", "Something went wrong reading the local header");
    return -1;
  }

  if (le32(pLocalHeader) != ZIP_SIG_LOCAL /* ZIP local file header signature */) {
    LOG_ERR("ZIP", "Not a valid zip file header");
    return -1;
  }

  const uint16_t filenameLength = pLocalHeader[26] + (pLocalHeader[27] << 8);
  const uint16_t extraOffset = pLocalHeader[28] + (pLocalHeader[29] << 8);

  // The local header names the file too, and the two names must be the same one. A bounded read: the length
  // is attacker-controlled, so it is checked against the header's own position first.
  if (expectName != nullptr) {
    constexpr size_t MAX_LOCAL_NAME = 4096;
    if (filenameLength == 0 || filenameLength > MAX_LOCAL_NAME ||
        static_cast<size_t>(fileOffset) + localHeaderSize + filenameLength > file.size()) {
      LOG_ERR("ZIP", "local filename length is out of range");
      return -1;
    }
    std::vector<uint8_t> localName(filenameLength);
    if (file.read(localName.data(), filenameLength) != static_cast<int>(filenameLength)) {
      LOG_ERR("ZIP", "could not read the local filename");
      return -1;
    }
    if (strlen(expectName) != filenameLength ||
        memcmp(localName.data(), expectName, filenameLength) != 0) {
      LOG_ERR("ZIP", "local filename contradicts the central directory");
      return -1;
    }
    // restore the cursor to the data offset: the name read consumed it
    if (!file.seek(static_cast<size_t>(fileOffset) + localHeaderSize)) return -1;
  }

  // Encrypted entries: unsupported, and MUST fail explicitly. An encrypted member's "compressed" bytes are
  // not a deflate stream, so treating them as one is how a hostile archive gets the reader to consume
  // arbitrary following bytes as entry content.
  const uint16_t localFlags = le16(pLocalHeader + 6);
  if (localFlags & 0x0001u) {
    LOG_ERR("ZIP", "encrypted ZIP entry is not supported");
    return -1;
  }
  // The local and central records must AGREE about the method. A contradiction is malformed input, not
  // something to interpret: the two records describe the same bytes.
  if (le16(pLocalHeader + 8) != fileStat.method) {
    LOG_ERR("ZIP", "local compression method contradicts the central directory");
    return -1;
  }
  // ...and about the flags that change how the payload must be read. Encryption is checked above on the
  // local record; the data-descriptor bit changes where the member's sizes come from, so a disagreement is
  // a contradiction rather than something to resolve by preferring one record.
  constexpr uint16_t ZIP_FLAG_RELEVANT = 0x0008;      // streaming data descriptor
  if ((localFlags & ZIP_FLAG_RELEVANT) != (fileStat.flags & ZIP_FLAG_RELEVANT)) {
    LOG_ERR("ZIP", "local/central flag contradiction");
    return -1;
  }
  // STORED means the member IS its bytes, so the two sizes must be equal. Without this, a record saying
  // compressed=10, uncompressed=1,000,000 makes the reader consume following ZIP structures as content.
  if (fileStat.method == ZIP_METHOD_STORED && fileStat.compressedSize != fileStat.uncompressedSize) {
    LOG_ERR("ZIP", "STORED entry size mismatch (compressed %u != uncompressed %u)",
            fileStat.compressedSize, fileStat.uncompressedSize);
    return -1;
  }

  // Validate the DATA RANGE this returns, not merely the 30-byte header it was computed from: a hostile
  // filename/extra length used to push the offset past EOF, where the "data" is whatever follows — and the
  // caller then inflated from there. Every step is a widening, checked addition.
  const size_t base = static_cast<size_t>(fileOffset);
  if (filenameLength > SIZE_MAX - base - localHeaderSize) {
    LOG_ERR("ZIP", "Local header name length overflows the offset");
    return -1;
  }
  size_t dataOffset = base + localHeaderSize + filenameLength;
  if (extraOffset > SIZE_MAX - dataOffset) {
    LOG_ERR("ZIP", "Local header extra length overflows the offset");
    return -1;
  }
  dataOffset += extraOffset;
  if (dataOffset > file.size() || fileStat.compressedSize > file.size() - dataOffset) {
    LOG_ERR("ZIP", "Compressed data lies outside the file");
    return -1;
  }
  // Tighter than EOF: entry data must not run into the central directory. Physical EOF is not the real
  // boundary — the directory is, and an entry that "ends" inside it is overlapping another member's record.
  if (zipDetails.centralDirSize != 0 || zipDetails.totalEntries != 0) {
    const size_t cdStart = static_cast<size_t>(zipDetails.centralDirOffset);
    if (dataOffset > cdStart || fileStat.compressedSize > cdStart - dataOffset) {
      LOG_ERR("ZIP", "entry data overlaps the central directory");
      return -1;
    }
  }
  return static_cast<long>(dataOffset);
}

bool ZipFile::loadZipDetails() {
  if (zipDetails.isSet) {
    return true;
  }

  const ScopedOpenClose zip{*this};
  if (!zip) return false;

  const size_t fileSize = file.size();
  if (fileSize < ZIP_EOCD_MIN) {
    LOG_ERR("ZIP", "File too small to be a valid zip");
    return false;  // Minimum EOCD size is 22 bytes
  }

  // The EOCD is followed by an OPTIONAL COMMENT of up to 65535 bytes, so scanning only the trailing 1 KiB
  // rejects valid archives outright and can also lock onto a stray signature inside a comment. Scan the
  // widest legal window instead, and accept a candidate only when its comment length lands exactly on EOF.
  const size_t maxScan = ZIP_EOCD_MIN + ZIP_MAX_COMMENT;
  const size_t scanRange = fileSize < maxScan ? fileSize : maxScan;
  const auto buffer = static_cast<uint8_t*>(malloc(scanRange));
  if (!buffer) {
    LOG_ERR("ZIP", "Failed to allocate memory for EOCD scan buffer");
    return false;
  }

  if (!file.seek(fileSize - scanRange)) {
    free(buffer);
    return false;
  }
  const int scanRead = file.read(buffer, scanRange);
  if (scanRead < 0 || static_cast<size_t>(scanRead) != scanRange) {
    LOG_ERR("ZIP", "Short read while scanning for EOCD");
    free(buffer);
    return false;
  }

  // Scan backwards for the signature. The window ends at EOF, so `i + 22 + commentLength == scanRange`
  // is exactly "this record's comment runs to the end of the file".
  long foundOffset = -1;
  for (long i = static_cast<long>(scanRange) - static_cast<long>(ZIP_EOCD_MIN); i >= 0; --i) {
    if (le32(&buffer[i]) != ZIP_SIG_EOCD) continue;
    const size_t commentLength = le16(&buffer[i + 20]);
    if (static_cast<size_t>(i) + ZIP_EOCD_MIN + commentLength != scanRange) continue;
    foundOffset = i;
    break;
  }

  if (foundOffset == -1) {
    LOG_ERR("ZIP", "EOCD signature not found in zip file");
    free(buffer);
    return false;
  }

  // EOCD fields: 10 = total entries (2), 12 = central directory size (4), 16 = central directory
  // offset (4). All of them are attacker-controlled until proven otherwise, so nothing here is stored
  // before it has been checked against the file the record claims to describe.
  const uint8_t* eocd = &buffer[foundOffset];
  const uint16_t diskNumber = le16(eocd + 4);
  const uint16_t cdStartDisk = le16(eocd + 6);
  const uint16_t entriesOnDisk = le16(eocd + 8);
  const uint16_t entries = le16(eocd + 10);
  const uint32_t centralDirSize = le32(eocd + 12);
  const uint32_t centralDirOffset = le32(eocd + 16);
  // Absolute offset of the EOCD RECORD. The central directory has to end before the record that describes
  // it, not merely before physical EOF — otherwise a directory could be declared into the slack after the
  // record and still satisfy every "inside the file" test.
  const size_t eocdAbsolute = fileSize - scanRange + static_cast<size_t>(foundOffset);
  free(buffer);

  // ZIP64 sentinels: rejected explicitly rather than interpreted as if they were ordinary values.
  if (entries == 0xffff || centralDirSize == 0xffffffffu || centralDirOffset == 0xffffffffu) {
    LOG_ERR("ZIP", "ZIP64 archives are not supported");
    return false;
  }
  // Multi-disk archives: every offset in them is relative to a disk we do not have, so "the CD is inside
  // the file" is not even the right question. Reject outright rather than misread the first disk.
  if (diskNumber != 0 || cdStartDisk != 0 || entriesOnDisk != entries) {
    LOG_ERR("ZIP", "Multi-disk archives are not supported");
    return false;
  }
  if (centralDirOffset > fileSize || centralDirSize > fileSize - centralDirOffset) {
    LOG_ERR("ZIP", "Central directory lies outside the file");
    return false;
  }
  if (centralDirOffset > eocdAbsolute || centralDirSize > eocdAbsolute - centralDirOffset) {
    LOG_ERR("ZIP", "Central directory does not end before the EOCD record");
    return false;
  }
  if (entries != 0 && static_cast<size_t>(entries) > centralDirSize / ZIP_CD_MIN_ENTRY) {
    LOG_ERR("ZIP", "Entry count exceeds what the central directory can hold");
    return false;
  }
  if (entries > MAX_ZIP_ENTRIES || centralDirSize > MAX_ZIP_CENTRAL_DIRECTORY_BYTES) {
    LOG_ERR("ZIP", "Central directory exceeds the supported metadata budget");
    return false;
  }

  // Validate the ENTIRE directory, including global name uniqueness, before a
  // lazy filename lookup may trust any one entry.  The result lives on the
  // backing Blob, not this short-lived ZipFile: Epub constructs a ZipFile per
  // member read, and rescanning a 30k-entry directory for every spine/image
  // would turn this safety check into quadratic work.  HalFile writes
  // invalidate the mark, so it can never certify mutated bytes.
  if (!file.isZipDirectoryValidated()) {
    if (!file.seek(centralDirOffset)) return false;
    const size_t cdEnd = static_cast<size_t>(centralDirOffset) + centralDirSize;
    std::unordered_set<std::string> names;
    names.reserve(entries);
    size_t totalNameBytes = 0;
    for (uint32_t i = 0; i < entries; ++i) {
      CentralEntry entry;
      if (!readCentralEntry(entry, cdEnd)) {
        LOG_ERR("ZIP", "Malformed central-directory record");
        return false;
      }
      if (entry.name.size() > MAX_ZIP_TOTAL_NAME_BYTES - totalNameBytes) {
        LOG_ERR("ZIP", "ZIP filename metadata exceeds the supported budget");
        return false;
      }
      totalNameBytes += entry.name.size();
      if (!names.insert(entry.name).second) {
        LOG_ERR("ZIP", "duplicate central-directory name: %s", entry.name.c_str());
        return false;
      }
    }
    file.markZipDirectoryValidated();
  }

  zipDetails.totalEntries = entries;
  zipDetails.centralDirOffset = centralDirOffset;
  zipDetails.centralDirSize = centralDirSize;
  zipDetails.isSet = true;
  return true;
}

bool ZipFile::open() {
  if (!Storage.openFileForRead("ZIP", filePath, file)) {
    return false;
  }
  return true;
}

bool ZipFile::close() {
  if (file) {
    // Explicit close() required: member variable persists beyond function scope
    file.close();
  }
  lastCentralDirPos = 0;
  lastCentralDirPosValid = false;
  return true;
}

bool ZipFile::getInflatedFileSize(const char* filename, size_t* size) {
  FileStatSlim fileStat = {};
  if (!loadFileStatSlim(filename, &fileStat)) {
    return false;
  }

  *size = static_cast<size_t>(fileStat.uncompressedSize);
  return true;
}

int ZipFile::fillUncompressedSizes(std::deque<SizeTarget>& targets, std::deque<uint32_t>& sizes) {
  if (targets.empty()) return 0;

  const ScopedOpenClose zip{*this};
  if (!zip) return -1;

  if (!loadZipDetails()) return -1;

  if (!file.seek(zipDetails.centralDirOffset)) return -1;

  const size_t cdEnd = static_cast<size_t>(zipDetails.centralDirOffset) + zipDetails.centralDirSize;

  int matched = 0;
  const int targetCount = static_cast<int>(targets.size());

  // Bounded by the declared entry count and parsed by the SAME record parser as every other walker. This
  // loop is not dead code: BookMetadataCache uses it for books at or above LARGE_SPINE_THRESHOLD.
  for (uint32_t i = 0; i < zipDetails.totalEntries; ++i) {
    CentralEntry e;
    if (!readCentralEntry(e, cdEnd)) return -1;      // a malformed directory is fatal, not "no match"

    const uint64_t hash = fnvHash64(e.name.c_str(), e.name.size());
    SizeTarget key;
    key.hash = hash;
    key.index = 0;
    key.path = e.name;

    auto it = std::lower_bound(targets.begin(), targets.end(), key,
                               [](const SizeTarget& a, const SizeTarget& b) {
                                 return a.hash < b.hash || (a.hash == b.hash && a.path < b.path);
                               });

    // Hash NARROWS the bucket; the exact path DECIDES. Comparing bytes is what closes the deliberate
    // collision case — a bucket of one is not proof of identity, which is why the path is carried at all.
    while (it != targets.end() && it->hash == hash) {
      if (it->path == e.name && it->index < sizes.size()) {
        sizes[it->index] = e.uncompressedSize;
        matched++;
      }
      ++it;
    }

    if (matched >= targetCount) return matched;
  }

  // Fewer matches than targets is legitimate (the caller falls back); a malformed directory is not.
  return matched;
}

uint8_t* ZipFile::readFileToMemory(const char* filename, size_t* size, const bool trailingNullByte,
                                   const size_t maxOutputBytes) {
  const ScopedOpenClose zip{*this};
  if (!zip) return nullptr;

  FileStatSlim fileStat = {};
  if (!loadFileStatSlim(filename, &fileStat)) return nullptr;

  // BEFORE any allocation or inflation: the declared size is attacker-controlled, and a decoder-side check
  // happens after the member already expanded into memory-backed storage.
  if (static_cast<uint64_t>(fileStat.uncompressedSize) > static_cast<uint64_t>(maxOutputBytes)) {
    LOG_ERR("ZIP", "entry declares %u bytes, above the caller's %zu byte budget", fileStat.uncompressedSize,
            maxOutputBytes);
    return nullptr;
  }

  const long fileOffset = getDataOffset(fileStat, filename);
  if (fileOffset < 0) return nullptr;

  file.seek(fileOffset);

  const auto deflatedDataSize = fileStat.compressedSize;
  // Hostile central-directory metadata: uncompressedSize is uint32_t, so `0xffffffff + 1` wraps to 0 in
  // 32-bit arithmetic and a "zero byte" entry becomes a multi-gigabyte inflate target. Widen first, then
  // check the addition.
  const size_t inflatedDataSize = static_cast<size_t>(fileStat.uncompressedSize);
  size_t allocSize = inflatedDataSize;
  if (trailingNullByte) {
    if (inflatedDataSize == std::numeric_limits<size_t>::max()) {
      LOG_ERR("ZIP", "Entry size overflow");
      return nullptr;
    }
    allocSize = inflatedDataSize + 1;
  }
  if (allocSize == 0) {
    LOG_ERR("ZIP", "Empty entry rejected");
    return nullptr;
  }
  const auto data = static_cast<uint8_t*>(malloc(allocSize));
  if (data == nullptr) {
    LOG_ERR("ZIP", "Failed to allocate memory for output buffer (%zu bytes)", allocSize);
    return nullptr;
  }

  if (fileStat.method == ZIP_METHOD_STORED) {
    // no deflation, just read content
    const int got = file.read(data, inflatedDataSize);
    if (got < 0 || static_cast<size_t>(got) != inflatedDataSize) {
      LOG_ERR("ZIP", "Failed to read data (STORED entry)");
      free(data);
      return nullptr;
    }

    // Continue out of block with data set
  } else if (fileStat.method == ZIP_METHOD_DEFLATED) {
    auto* fileReadBuffer = static_cast<uint8_t*>(malloc(1024));
    if (!fileReadBuffer) {
      LOG_ERR("ZIP", "Failed to allocate memory for zip file read buffer");
      free(data);
      return nullptr;
    }

    ZipInflateCtx ctx;
    ctx.file = &file;
    ctx.fileRemaining = deflatedDataSize;
    ctx.readBuf = fileReadBuffer;
    ctx.readBufSize = 1024;

    // One-shot mode: `data` holds the entire output, so back-references
    // resolve inside it and no 32KB window is allocated.
    InflateStream inflate;
    if (!inflate.init(false)) {
      LOG_ERR("ZIP", "Failed to init inflate stream");
      free(fileReadBuffer);
      free(data);
      return nullptr;
    }
    inflate.setFill(zipFillCallback, &ctx);

    if (!inflate.read(data, inflatedDataSize)) {
      LOG_ERR("ZIP", "Failed to inflate file");
      free(fileReadBuffer);
      free(data);
      return nullptr;
    }
    free(fileReadBuffer);

    // Continue out of block with data set
  } else {
    LOG_ERR("ZIP", "Unsupported compression method");
    free(data);
    return nullptr;
  }

  // CRC32 value zero is data, not an "unset" sentinel in a central-directory
  // record.  Every complete read is verified, including a declared 0 CRC.
  const uint32_t actual = crc32Update(0, data, inflatedDataSize);
  if (actual != fileStat.crc32) {
    LOG_ERR("ZIP", "CRC mismatch for %s: central %08x, computed %08x", filename, fileStat.crc32, actual);
    free(data);
    return nullptr;
  }

  if (trailingNullByte) data[allocSize - 1] = '\0';   // allocSize carries the +1, never the wrapped form
  if (size) *size = inflatedDataSize;
  return data;
}

bool ZipFile::readFileToStream(const char* filename, Print& out, const size_t chunkSize,
                               const bool allowEarlyStop, const size_t maxOutputBytes) {
  const ScopedOpenClose zip{*this};
  if (!zip) return false;

  FileStatSlim fileStat = {};
  if (!loadFileStatSlim(filename, &fileStat)) return false;

  // Reject BEFORE the member expands: the sink here is memory-backed storage, so a "streaming" read of a
  // hostile member still ends with the whole uncompressed payload resident.
  if (static_cast<uint64_t>(fileStat.uncompressedSize) > static_cast<uint64_t>(maxOutputBytes)) {
    LOG_ERR("ZIP", "entry declares %u bytes, above the caller's %zu byte budget", fileStat.uncompressedSize,
            maxOutputBytes);
    return false;
  }

  const long fileOffset = getDataOffset(fileStat, filename);
  if (fileOffset < 0) return false;

  file.seek(fileOffset);
  const auto deflatedDataSize = fileStat.compressedSize;
  const auto inflatedDataSize = fileStat.uncompressedSize;

  if (fileStat.method == ZIP_METHOD_STORED) {
    // no deflation, just read content
    const auto buffer = static_cast<uint8_t*>(malloc(chunkSize));
    if (!buffer) {
      LOG_ERR("ZIP", "Failed to allocate memory for buffer");
      return false;
    }

    uint32_t crc = 0;
    size_t remaining = inflatedDataSize;
    while (remaining > 0) {
      const size_t want = remaining < chunkSize ? remaining : chunkSize;
      // Signed check BEFORE the conversion: `-1` here would become SIZE_MAX and be handed to out.write().
      const int got = file.read(buffer, want);
      if (got <= 0) {
        LOG_ERR("ZIP", "Could not read more bytes (STORED entry)");
        free(buffer);
        return false;
      }
      const size_t dataRead = static_cast<size_t>(got);
      if (dataRead > want) {
        free(buffer);
        return false;
      }

      crc = crc32Update(crc, buffer, dataRead);
      if (out.write(buffer, dataRead) != dataRead) {
        free(buffer);
        if (allowEarlyStop) return true;  // sink has what it needs; the member is left UNVERIFIED on purpose
        LOG_ERR("ZIP", "Failed to write all output bytes to stream");
        return false;
      }
      remaining -= dataRead;
    }

    free(buffer);
    // The whole member was consumed, so the central directory's CRC is meaningful and must match.
    const uint32_t actual = crc32Update(crc, nullptr, 0);
    if (actual != fileStat.crc32) {
      LOG_ERR("ZIP", "CRC mismatch for %s: central %08x, computed %08x", filename, fileStat.crc32, actual);
      return false;
    }
    return true;
  }

  if (fileStat.method == ZIP_METHOD_DEFLATED) {
    auto* fileReadBuffer = static_cast<uint8_t*>(malloc(chunkSize));
    if (!fileReadBuffer) {
      LOG_ERR("ZIP", "Failed to allocate memory for zip file read buffer");
      return false;
    }

    auto* outputBuffer = static_cast<uint8_t*>(malloc(chunkSize));
    if (!outputBuffer) {
      LOG_ERR("ZIP", "Failed to allocate memory for output buffer");
      free(fileReadBuffer);
      return false;
    }

    ZipInflateCtx ctx;
    ctx.file = &file;
    ctx.fileRemaining = deflatedDataSize;
    ctx.readBuf = fileReadBuffer;
    ctx.readBufSize = chunkSize;

    InflateStream inflate;
    if (!inflate.init(true)) {
      LOG_ERR("ZIP", "Failed to init inflate stream");
      free(outputBuffer);
      free(fileReadBuffer);
      return false;
    }
    inflate.setFill(zipFillCallback, &ctx);

    bool success = false;
    size_t totalProduced = 0;
    uint32_t crc = 0;

    while (true) {
      size_t produced;
      const InflateStream::Status status = inflate.readAtMost(outputBuffer, chunkSize, &produced);

      totalProduced += produced;
      // Integrity is accumulated as the bytes are produced, so a full decode can be verified without a
      // second pass. An early-stop probe leaves this partial and is deliberately NOT verified (see below).
      crc = crc32Update(crc, outputBuffer, produced);
      if (totalProduced > static_cast<size_t>(inflatedDataSize)) {
        LOG_ERR("ZIP", "Decompressed size exceeds expected (%zu > %zu)", totalProduced,
                static_cast<size_t>(inflatedDataSize));
        break;
      }

      if (produced > 0) {
        if (out.write(outputBuffer, produced) != produced) {
          if (allowEarlyStop) {
            success = true;  // sink has what it needs
          } else {
            LOG_ERR("ZIP", "Failed to write all output bytes to stream");
          }
          break;
        }
      }

      if (status == InflateStream::Status::Done) {
        if (totalProduced != static_cast<size_t>(inflatedDataSize)) {
          LOG_ERR("ZIP", "Decompressed size mismatch (expected %zu, got %zu)", static_cast<size_t>(inflatedDataSize),
                  totalProduced);
          break;
        }
        LOG_DBG("ZIP", "Decompressed %d bytes into %d bytes", deflatedDataSize, inflatedDataSize);
        success = true;
        break;
      }

      if (status == InflateStream::Status::Error) {
        LOG_ERR("ZIP", "Decompression failed");
        break;
      }
      // InflateStream::Status::Ok: output buffer full, continue
    }

    free(outputBuffer);
    free(fileReadBuffer);
    // Only a COMPLETE decode can be verified: with allowEarlyStop the sink stopped the read on purpose, so
    // the uncompressed member was never fully inspected and claiming CRC coverage for it would be a lie.
    if (success && totalProduced == static_cast<size_t>(inflatedDataSize)) {
      if (crc != fileStat.crc32) {
        LOG_ERR("ZIP", "CRC mismatch for %s: central %08x, computed %08x", filename, fileStat.crc32, crc);
        success = false;
      }
    }
    return success;  // inflate destructor frees the decompressor state + window
  }

  LOG_ERR("ZIP", "Unsupported compression method");
  return false;
}
