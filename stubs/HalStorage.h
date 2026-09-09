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
class HalFile : public Print {
  friend class HalStorage;

 public:
  HalFile() = default;
  ~HalFile() override = default;
  HalFile(HalFile&& o) noexcept { *this = std::move(o); }
  HalFile& operator=(HalFile&& o) noexcept {
    if (this != &o) {
      path_ = std::move(o.path_);
      data_ = std::move(o.data_);
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
  size_t size() const { return data_ ? data_->size() : 0; }
  size_t fileSize() const { return size(); }
  uint64_t fileSize64() const { return size(); }
  bool seek(size_t pos) {
    if (!data_ || pos > data_->size()) return false;
    pos_ = pos;
    return true;
  }
  bool seek64(uint64_t pos) { return seek(static_cast<size_t>(pos)); }
  bool seekCur(int64_t offset) {
    if (!data_) return false;
    int64_t np = static_cast<int64_t>(pos_) + offset;
    if (np < 0 || np > static_cast<int64_t>(data_->size())) return false;
    pos_ = static_cast<size_t>(np);
    return true;
  }
  bool seekSet(size_t offset) { return seek(offset); }
  int available() const { return data_ ? static_cast<int>(data_->size() - pos_) : 0; }
  size_t position() const { return pos_; }
  int read(void* buf, size_t count) {
    if (!data_ || pos_ >= data_->size()) return -1;
    size_t n = std::min(count, data_->size() - pos_);
    memcpy(buf, data_->data() + pos_, n);
    pos_ += n;
    return static_cast<int>(n);
  }
  int read() {
    uint8_t b;
    if (read(&b, 1) != 1) return -1;
    return b;
  }
  size_t write(const void* buf, size_t count) {
    if (!data_) return 0;
    if (pos_ + count > data_->size()) data_->resize(pos_ + count);
    memcpy(data_->data() + pos_, buf, count);
    pos_ += count;
    return count;
  }
  size_t write(uint8_t b) override { return write(&b, 1); }
  bool isDirectory() const { return false; }
  bool close() { return true; }
  bool isOpen() const { return static_cast<bool>(data_); }
  explicit operator bool() const { return isOpen(); }

  const std::string& path() const { return path_; }

 private:
  explicit HalFile(std::string path, std::shared_ptr<std::vector<uint8_t>> data)
      : path_(std::move(path)), data_(std::move(data)) {}

  std::string path_;
  std::shared_ptr<std::vector<uint8_t>> data_;
  size_t pos_ = 0;
};

// ---- In-memory storage -----------------------------------------------------
class HalStorage {
 public:
  HalStorage() = default;
  bool begin() { return true; }
  bool ready() const { return true; }

  // Inject a whole file blob (e.g. the EPUB) at a virtual path.
  void mountBlob(const std::string& path, const uint8_t* data, size_t size);
  void mountBlob(const std::string& path, const std::vector<uint8_t>& data) {
    mountBlob(path, data.data(), data.size());
  }

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

 private:
  std::map<std::string, std::shared_ptr<std::vector<uint8_t>>> files_;
};

#define Storage HalStorage::getInstance()
#define SdMan Storage  // compatibility alias used by some paths

// oflag helpers not provided by our enum
inline HalFile openFileForReadCompat(const char* p) { return Storage.open(p, O_RDONLY); }
