// HalStorage.h — host/WASM shim: in-memory filesystem exposing the same
// interface the CrossPoint-KO engine expects. An EPUB can be injected as a
// single blob at a virtual path; all other files (section caches, extracted
// HTML) live in a RAM map and vanish when the module is torn down.
#pragma once

#include <Arduino.h>  // Print, String

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

// FreeInkSDK oflag substitute
using oflag_t = int;
constexpr int O_RDONLY = 0;
constexpr int O_WRONLY = 1;
constexpr int O_RDWR = 2;
constexpr int O_CREAT = 0x40;
constexpr int O_TRUNC = 0x200;

class HalStorage;

// ---- In-memory file handle ------------------------------------------------
// One in-memory file. Two shapes, because they have different costs:
//   * writable  — a std::vector, used by everything the engine GENERATES (section caches, extracted
//                 HTML, metadata cache). Unchanged from before.
//   * owned     — a view over a buffer somebody else allocated, with a shared_ptr that frees it. This
//                 exists for the mounted EPUB: the browser streams it straight into the wasm heap, so
//                 the bytes the ZIP reader walks ARE the bytes the page wrote, with no C++ copy. A
//                 100 MB book used to be copied again here (vector<uint8_t>(data, data + size)).
struct Blob {
  const uint8_t* data = nullptr;
  size_t size = 0;
  std::shared_ptr<void> owner;                     // frees `data` when the last reference goes away
  std::shared_ptr<std::vector<uint8_t>> writable;  // non-null iff this file may be written/extended

  // A writable file's vector can be reallocated by resize(), so the view is re-derived on demand.
  // BOTH fields are re-read every time: a vector that grows or shrinks WITHIN ITS CAPACITY keeps the
  // same data() pointer, so comparing pointers alone leaves `size` stale — and a stale size is read as
  // real content. That bug was caught by the spine count dropping from 10 to 6 with a parse error,
  // because reads were running past the bytes actually written.
  void refresh() {
    if (!writable) return;
    data = writable->data();
    size = writable->size();
  }
};

class HalFile : public Print {
  friend class HalStorage;

 public:
  HalFile() = default;
  ~HalFile() override = default;
  HalFile(HalFile&& o) noexcept { *this = std::move(o); }
  HalFile& operator=(HalFile&& o) noexcept {
    if (this != &o) {
      path_ = std::move(o.path_);
      blob_ = std::move(o.blob_);
      pos_ = o.pos_;
      o.pos_ = 0;
    }
    return *this;
  }
  HalFile(const HalFile&) = delete;
  HalFile& operator=(const HalFile&) = delete;

  void flush() {}
  size_t getName(char* name, size_t len) const {
    if (!name || len == 0) return 0;
    size_t n = std::min(path_.size(), len - 1);
    memcpy(name, path_.data(), n);
    name[n] = '\0';
    return n;
  }
  size_t size() const { return blob_ ? blob_->size : 0; }
  size_t fileSize() const { return size(); }
  uint64_t fileSize64() const { return size(); }
  bool seek(size_t pos) {
    if (!blob_ || pos > blob_->size) return false;
    pos_ = pos;
    return true;
  }
  bool seek64(uint64_t pos) { return seek(static_cast<size_t>(pos)); }
  bool seekCur(int64_t offset) {
    if (!blob_) return false;
    int64_t np = static_cast<int64_t>(pos_) + offset;
    if (np < 0 || np > static_cast<int64_t>(blob_->size)) return false;
    pos_ = static_cast<size_t>(np);
    return true;
  }
  bool seekSet(size_t offset) { return seek(offset); }
  int available() const { return blob_ ? static_cast<int>(blob_->size - pos_) : 0; }
  size_t position() const { return pos_; }
  int read(void* buf, size_t count) {
    if (!blob_ || pos_ >= blob_->size) return -1;
    size_t n = std::min(count, blob_->size - pos_);
    memcpy(buf, blob_->data + pos_, n);
    pos_ += n;
    return static_cast<int>(n);
  }
  int read() {
    uint8_t b;
    if (read(&b, 1) != 1) return -1;
    return b;
  }
  size_t write(const void* buf, size_t count) {
    // Read-only files (the mounted EPUB) REFUSE writes rather than silently growing: a write into an
    // owned view would have to reallocate somebody else's buffer.
    if (!blob_ || !blob_->writable) return 0;
    if (pos_ + count > blob_->writable->size()) blob_->writable->resize(pos_ + count);
    blob_->refresh();
    memcpy(blob_->writable->data() + pos_, buf, count);
    pos_ += count;
    return count;
  }
  size_t write(uint8_t b) override { return write(&b, 1); }
  bool isDirectory() const { return false; }
  bool close() { return true; }
  bool isOpen() const { return static_cast<bool>(blob_); }
  explicit operator bool() const { return isOpen(); }

  const std::string& path() const { return path_; }

 private:
  explicit HalFile(std::string path, std::shared_ptr<Blob> blob)
      : path_(std::move(path)), blob_(std::move(blob)) {}

  std::string path_;
  std::shared_ptr<Blob> blob_;
  size_t pos_ = 0;
};

// ---- In-memory storage -----------------------------------------------------
class HalStorage {
 public:
  HalStorage() = default;
  bool begin() { return true; }
  bool ready() const { return true; }

  // Inject a whole file blob at a virtual path, COPYING it. Used for small generated assets (the
  // .epdfont pack) where a copy costs nothing and the storage owns a writable buffer.
  void mountBlob(const std::string& path, const uint8_t* data, size_t size);
  void mountBlob(const std::string& path, const std::vector<uint8_t>& data) {
    mountBlob(path, data.data(), data.size());
  }

  // Adopt an allocation the caller already owns, WITHOUT copying. The read-only counterpart of
  // mountBlob, for the mounted EPUB: the bytes the ZIP reader walks are the bytes the page streamed
  // into the wasm heap, and `owner`'s deleter releases them when the file is dropped (clearAll on the
  // next book). A whole-book copy used to happen here on every open.
  void mountOwnedBlob(const std::string& path, uint8_t* data, size_t size);

  std::vector<String> listFiles(const char* path = "/", int maxFiles = 200) { (void)path;
    (void)maxFiles;
    return {};
  }
  String readFile(const char* path);
  bool readFileToStream(const char* path, Print& out, size_t chunkSize = 256);
  size_t readFileToBuffer(const char* path, char* buffer, size_t bufferSize, size_t maxBytes = 0);
  bool writeFile(const char* path, const String& content);
  bool ensureDirectoryExists(const char* path);

  HalFile open(const char* path, const oflag_t oflag = O_RDONLY);
  bool mkdir(const char* path, const bool pFlag = true) { (void)path;
    (void)pFlag;
    return true; }
  bool exists(const char* path);
  bool remove(const char* path);
  bool rename(const char* oldPath, const char* newPath);
  bool rmdir(const char* path) { (void)path;
    return true; }
  bool removeDir(const char* path) { return remove(path); }

  bool openFileForRead(const char* moduleName, const char* path, HalFile& file);
  bool openFileForRead(const char* moduleName, const std::string& path, HalFile& file) {
    return openFileForRead(moduleName, path.c_str(), file);
  }
  bool openFileForRead(const char* moduleName, const String& path, HalFile& file) {
    return openFileForRead(moduleName, path.c_str(), file);
  }
  bool openFileForWrite(const char* moduleName, const char* path, HalFile& file);
  bool openFileForWrite(const char* moduleName, const std::string& path, HalFile& file) {
    return openFileForWrite(moduleName, path.c_str(), file);
  }
  bool openFileForWrite(const char* moduleName, const String& path, HalFile& file) {
    return openFileForWrite(moduleName, path.c_str(), file);
  }

  static HalStorage& getInstance() {
    static HalStorage instance;
    return instance;
  }

  // Dump all paths (debugging)
  std::vector<std::string> debugPaths() const;

  // Drop every mounted blob/file (call on new book load / teardown so section
  // caches from a previous book don't accumulate in the wasm heap).
  void clearAll() { files_.clear(); }

  // Live storage accounting. heapBytes (wasm linear memory) only reports the high-water mark; this is
  // what HalStorage is actually holding right now — section caches and the mounted EPUB are all
  // RAM-backed here. Shared blobs (mountSharedBlob aliases) are counted once, and capacity() is used
  // rather than size() because a released vector's slack still occupies the heap.
  size_t totalBytes() const {
    std::vector<const void*> seen;
    size_t total = 0;
    for (const auto& kv : files_) {
      const std::shared_ptr<Blob>& blob = kv.second;
      if (!blob) continue;
      const void* p = blob.get();
      bool dup = false;
      for (const void* s : seen) { if (s == p) { dup = true; break; } }
      if (dup) continue;
      seen.push_back(p);
      // capacity() for the writable files (a released vector's slack still occupies the heap) and the
      // view size for adopted ones, which own exactly what they claim.
      const_cast<Blob&>(*blob).refresh();
      total += blob->writable ? blob->writable->capacity() : blob->size;
    }
    return total;
  }

 private:
  std::map<std::string, std::shared_ptr<Blob>> files_;
};

#define Storage HalStorage::getInstance()
#define SdMan Storage  // compatibility alias used by some paths

// oflag helpers not provided by our enum
inline HalFile openFileForReadCompat(const char* p) { return Storage.open(p, O_RDONLY); }
