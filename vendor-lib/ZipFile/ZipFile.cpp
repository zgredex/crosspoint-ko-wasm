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

constexpr uint32_t ZIP_SIG_EOCD = 0x06054b50;
constexpr uint32_t ZIP_SIG_CENTRAL = 0x02014b50;
constexpr uint32_t ZIP_SIG_LOCAL = 0x04034b50;
constexpr size_t ZIP_EOCD_MIN = 22;
constexpr size_t ZIP_MAX_COMMENT = 65535;    // a ZIP comment is a uint16_t length
constexpr size_t ZIP_CD_MIN_ENTRY = 46;      // smallest possible central-directory record

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

  uint32_t sig;
  char itemName[256];
  fileStatSlimCache.clear();
  fileStatSlimCache.reserve(zipDetails.totalEntries);

  // Exactly totalEntries records, every read verified, and never past the directory the EOCD declared.
  // `while (file.available())` let the archive dictate when its own directory ended.
  for (uint32_t i = 0; i < zipDetails.totalEntries; ++i) {
    const size_t here = file.position();
    if (here > cdEnd || cdEnd - here < ZIP_CD_MIN_ENTRY) break;   // ran off the declared directory
    if (file.read(&sig, 4) != 4 || sig != ZIP_SIG_CENTRAL) break;

    FileStatSlim fileStat = {};

    if (!file.seekCur(6)) break;
    if (file.read(&fileStat.method, 2) != 2) break;
    if (!file.seekCur(8)) break;
    if (file.read(&fileStat.compressedSize, 4) != 4) break;
    if (file.read(&fileStat.uncompressedSize, 4) != 4) break;
    uint16_t nameLen, m, k;
    if (file.read(&nameLen, 2) != 2) break;
    if (file.read(&m, 2) != 2) break;
    if (file.read(&k, 2) != 2) break;
    if (!file.seekCur(8)) break;
    if (file.read(&fileStat.localHeaderOffset, 4) != 4) break;

    if (nameLen < sizeof(itemName)) {
      if (file.read(itemName, nameLen) != static_cast<int>(nameLen)) break;
      itemName[nameLen] = '\0';
      fileStatSlimCache.emplace(itemName, fileStat);
    } else {
      // Skip over oversized entry names to avoid writing past fixed buffer.
      if (!file.seekCur(nameLen)) break;
    }

    // Skip the rest of this entry (extra field + comment)
    if (!file.seekCur(m + k)) break;
  }

  // Set cursor to start of central directory for sequential access
  lastCentralDirPos = zipDetails.centralDirOffset;
  lastCentralDirPosValid = true;

  return true;
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

  uint32_t sig;
  char itemName[256];

  while (true) {
    // A hostile central directory must not be able to keep this loop running: it is bounded by the entry
    // count the EOCD declared AND by the end of the declared directory.
    if (++walked > zipDetails.totalEntries + 1u) break;
    const size_t here = file.position();
    if (here > cdEnd || cdEnd - here < ZIP_CD_MIN_ENTRY) break;
    uint32_t entryStart = file.position();

    if (file.read(&sig, 4) != 4 || sig != 0x02014b50) {
      // End of central directory
      if (!wrapped && lastCentralDirPosValid && startPos != zipDetails.centralDirOffset) {
        // Wrap around to beginning
        file.seek(zipDetails.centralDirOffset);
        wrapped = true;
        continue;
      }
      break;
    }

    // If we've wrapped and reached our start position, stop
    if (wrapped && entryStart >= startPos) {
      break;
    }

    if (!file.seekCur(6)) break;
    if (file.read(&fileStat->method, 2) != 2) break;
    if (!file.seekCur(8)) break;
    if (file.read(&fileStat->compressedSize, 4) != 4) break;
    if (file.read(&fileStat->uncompressedSize, 4) != 4) break;
    uint16_t nameLen, m, k;
    if (file.read(&nameLen, 2) != 2) break;
    if (file.read(&m, 2) != 2) break;
    if (file.read(&k, 2) != 2) break;
    if (!file.seekCur(8)) break;
    if (file.read(&fileStat->localHeaderOffset, 4) != 4) break;

    if (nameLen < 256) {
      if (file.read(itemName, nameLen) != static_cast<int>(nameLen)) break;
      itemName[nameLen] = '\0';

      if (strcmp(itemName, filename) == 0) {
        // Found it! Update cursor to next entry
        file.seekCur(m + k);
        lastCentralDirPos = file.position();
        lastCentralDirPosValid = true;
        found = true;
        break;
      }
    } else {
      // Name too long, skip it
      file.seekCur(nameLen);
    }

    // Skip extra field + comment
    file.seekCur(m + k);
  }

  return found;
}

long ZipFile::getDataOffset(const FileStatSlim& fileStat) {
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
  return fileOffset + localHeaderSize + filenameLength + extraOffset;
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
  const uint16_t entries = le16(eocd + 10);
  const uint32_t centralDirSize = le32(eocd + 12);
  const uint32_t centralDirOffset = le32(eocd + 16);
  free(buffer);

  // ZIP64 sentinels: rejected explicitly rather than interpreted as if they were ordinary values.
  if (entries == 0xffff || centralDirSize == 0xffffffffu || centralDirOffset == 0xffffffffu) {
    LOG_ERR("ZIP", "ZIP64 archives are not supported");
    return false;
  }
  if (centralDirOffset > fileSize || centralDirSize > fileSize - centralDirOffset) {
    LOG_ERR("ZIP", "Central directory lies outside the file");
    return false;
  }
  if (entries != 0 && static_cast<size_t>(entries) > centralDirSize / ZIP_CD_MIN_ENTRY) {
    LOG_ERR("ZIP", "Entry count exceeds what the central directory can hold");
    return false;
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
  if (targets.empty()) {
    return 0;
  }

  const ScopedOpenClose zip{*this};
  if (!zip) return 0;

  if (!loadZipDetails()) return 0;

  file.seek(zipDetails.centralDirOffset);

  int matched = 0;
  const int targetCount = static_cast<int>(targets.size());
  uint32_t sig;
  char itemName[256];

  while (file.available()) {
    file.read(&sig, 4);
    if (sig != 0x02014b50) break;

    file.seekCur(6);
    uint16_t method;
    file.read(&method, 2);
    file.seekCur(8);
    uint32_t compressedSize, uncompressedSize;
    file.read(&compressedSize, 4);
    file.read(&uncompressedSize, 4);
    uint16_t nameLen, m, k;
    file.read(&nameLen, 2);
    file.read(&m, 2);
    file.read(&k, 2);
    file.seekCur(8);
    uint32_t localHeaderOffset;
    file.read(&localHeaderOffset, 4);

    if (nameLen < 256) {
      file.read(itemName, nameLen);
      itemName[nameLen] = '\0';

      uint64_t hash = fnvHash64(itemName, nameLen);
      SizeTarget key = {hash, nameLen, 0};

      auto it = std::lower_bound(targets.begin(), targets.end(), key, [](const SizeTarget& a, const SizeTarget& b) {
        return a.hash < b.hash || (a.hash == b.hash && a.len < b.len);
      });

      while (it != targets.end() && it->hash == hash && it->len == nameLen) {
        if (it->index < sizes.size()) {
          sizes[it->index] = uncompressedSize;
          matched++;
        }
        ++it;
      }

      if (matched >= targetCount) {
        break;
      }
    } else {
      file.seekCur(nameLen);
    }

    file.seekCur(m + k);
  }

  return matched;
}

uint8_t* ZipFile::readFileToMemory(const char* filename, size_t* size, const bool trailingNullByte) {
  const ScopedOpenClose zip{*this};
  if (!zip) return nullptr;

  FileStatSlim fileStat = {};
  if (!loadFileStatSlim(filename, &fileStat)) return nullptr;

  const long fileOffset = getDataOffset(fileStat);
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

  if (trailingNullByte) data[allocSize - 1] = '\0';   // allocSize carries the +1, never the wrapped form
  if (size) *size = inflatedDataSize;
  return data;
}

bool ZipFile::readFileToStream(const char* filename, Print& out, const size_t chunkSize, const bool allowEarlyStop) {
  const ScopedOpenClose zip{*this};
  if (!zip) return false;

  FileStatSlim fileStat = {};
  if (!loadFileStatSlim(filename, &fileStat)) return false;

  const long fileOffset = getDataOffset(fileStat);
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

      if (out.write(buffer, dataRead) != dataRead) {
        free(buffer);
        if (allowEarlyStop) return true;  // sink has what it needs
        LOG_ERR("ZIP", "Failed to write all output bytes to stream");
        return false;
      }
      remaining -= dataRead;
    }

    free(buffer);
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

    while (true) {
      size_t produced;
      const InflateStream::Status status = inflate.readAtMost(outputBuffer, chunkSize, &produced);

      totalProduced += produced;
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
    return success;  // inflate destructor frees the decompressor state + window
  }

  LOG_ERR("ZIP", "Unsupported compression method");
  return false;
}
