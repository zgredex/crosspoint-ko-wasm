// HalStorage.h — host/WASM shim: in-memory filesystem exposing the same
// interface the CrossPoint-KO engine expects. An EPUB can be injected as a
// single blob at a virtual path; all other files (section caches, extracted
// HTML) live in a RAM map and vanish when the module is torn down.
#pragma once

#include <Arduino.h>  // Print, String
#include <climits>   // INT_MAX for the HalFile int contract

#include <algorithm>
#include <chrono>   // range-window read timing
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

// ---- External source statistics -------------------------------------------------------------
// Counters for the range-backed mount. `calls` is how many times C++ asked the outside world for bytes;
// `bytes` how many bytes actually crossed; `misses` how many windows had to be fetched. A ZIP walk that
// reads a whole 80 MB archive through this should show a handful of calls, not thousands — if it does
// not, the window is too small for the parser's access pattern and the cache is not doing its job.
struct ExternalStats {
  // `calls` is window fetches: how many times the outside world was asked for bytes. `hits` is the reads
  // that did NOT need one — the cache working. `bytes` is what actually crossed, and the number to hold
  // against the file size: an 80 MB archive whose first page needs 1 MB must not show 80 MB here.
  size_t calls = 0;
  size_t bytes = 0;
  size_t hits = 0;
  double readMs = 0.0;
  void reset() { calls = bytes = hits = 0; readMs = 0.0; }
};
ExternalStats& externalStats();

// Files generated while a book is open (inflated XHTML/images and parser
// caches) share one allocated-capacity budget.  Charging vector capacity—not
// just logical length—means geometric growth cannot jump over the ceiling.
// 768 MiB leaves 1.25 GiB of the
// module's 2 GiB memory32 ceiling for the mounted book, renderer and export
// buffers while still allowing the documented 256 MiB spine and 128 MiB image
// member ceilings to coexist.  Source blobs are not charged here: they are
// separately bounded at the public EPUB/font boundaries.
struct StorageBudget {
  static constexpr size_t kDefaultLimit = 768u * 1024u * 1024u;
  size_t used = 0;
  size_t limit = kDefaultLimit;
};

// ---- In-memory file handle ------------------------------------------------
// One in-memory file. THREE shapes, because they have different costs:
//   * writable  — a std::vector, used by everything the engine GENERATES (section caches, extracted
//                 HTML, metadata cache). Unchanged from before.
//   * owned     — a view over a buffer somebody else allocated, with a shared_ptr that frees it. This
//                 exists for the mounted EPUB: the browser streams it straight into the wasm heap, so
//                 the bytes the ZIP reader walks ARE the bytes the page wrote, with no C++ copy. A
//                 100 MB book used to be copied again here (vector<uint8_t>(data, data + size)).
//   * external  — the bytes are NOT in this address space. `readFn` fetches an aligned window on demand
//                 from whatever holds them (the page's File, a real file on the host). This exists so a
//                 large EPUB does not have to be made resident before page 1 is parsed: the reader walks
//                 the ZIP directory and the OPF, and nothing else is ever touched. On the 80 MB fixture
//                 the bulk path pays 44 ms of read + 14 ms of copy before parsing starts.
struct Blob {
  const uint8_t* data = nullptr;
  size_t size = 0;
  std::shared_ptr<void> owner;                     // frees `data` when the last reference goes away
  std::shared_ptr<std::vector<uint8_t>> writable;  // non-null iff this file may be written/extended
  std::shared_ptr<StorageBudget> budget;            // non-null for derived/cache files
  size_t budgetBytes = 0;                           // allocated capacity charged once per Blob
  bool zipDirectoryValidated = false;               // invalidated by every writable mutation

  ~Blob() {
    if (budget) {
      budget->used = budgetBytes <= budget->used ? budget->used - budgetBytes : 0;
    }
  }

  bool reserveWritableSize(size_t wanted) {
    if (!budget || !writable || wanted <= writable->capacity()) return true;
    if (budgetBytes > budget->used || budget->used > budget->limit) return false;
    const size_t otherBytes = budget->used - budgetBytes;
    if (otherBytes > budget->limit || wanted > budget->limit - otherBytes) return false;

    const size_t available = budget->limit - otherBytes;
    size_t target = wanted;
    const size_t current = writable->capacity();
    if (current != 0) {
      const size_t doubled = current > SIZE_MAX - current ? SIZE_MAX : current * 2;
      target = std::max(target, doubled);
    }
    target = std::min(target, available);
    writable->reserve(target);  // resize below can no longer trigger an unaccounted growth allocation
    const size_t actual = writable->capacity();
    if (actual > available) {
      // The standard permits reserve() to allocate more than requested.  A
      // conforming implementation that does so fails closed, including its
      // partial cache file, rather than retaining memory above the quota.
      std::vector<uint8_t>().swap(*writable);
      data = nullptr;
      size = 0;
      budget->used = otherBytes;
      budgetBytes = 0;
      return false;
    }
    budget->used = otherBytes + actual;
    budgetBytes = actual;
    return true;
  }

  // EXTERNAL backing. data stays null and every read is served by readFn through the window below.
  bool external = false;
  size_t externalSize = 0;
  int (*readFn)(void* ctx, size_t offset, uint8_t* dst, size_t len) = nullptr;
  void* readCtx = nullptr;

  // ONE aligned window, shared by every handle on this file (they share the Blob). The ZIP reader's
  // access pattern is many small reads around a moving position: per-window rather than per-read
  // fetching is the whole difference between a few fetches and tens of thousands.
  static constexpr size_t kWindow = 256 * 1024;
  std::vector<uint8_t> windowBytes;
  size_t windowStart = 0;
  size_t windowValid = 0;
  bool windowFilled = false;

  // A writable file's vector can be reallocated by resize(), so the view is re-derived on demand.
  // BOTH fields are re-read every time: a vector that grows or shrinks WITHIN ITS CAPACITY keeps the
  // same data() pointer, so comparing pointers alone leaves `size` stale — and a stale size is read as
  // real content. That bug was caught by the spine count dropping from 10 to 6 with a parse error,
  // because reads were running past the bytes actually written.
  void refresh() {
    if (external) { size = externalSize; return; }
    if (!writable) return;
    data = writable->data();
    size = writable->size();
  }

  // Fetch the aligned window containing absolute offset `at`. Returns false when the source refuses.
  bool fillWindow(size_t at) {
    if (!readFn || externalSize == 0) return false;
    const size_t aligned = (at / kWindow) * kWindow;
    size_t len = externalSize - aligned;
    if (len > kWindow) len = kWindow;
    if (windowBytes.size() < len) windowBytes.resize(len);
    // A reader is allowed to return a SHORT positive read; the old code took the first positive result as
    // the whole window and would then serve the gap as if it were file content. Loop until `len` is
    // satisfied, and treat a short final window as a failure — `len` is already clipped to externalSize,
    // so there is no legitimate short window at the end.
    size_t total = 0;
    while (total < len) {
      ExternalStats& stats = externalStats();     // counted here, once, for both hosts
      const auto t0 = std::chrono::steady_clock::now();
      stats.calls++;
      const int got = readFn(readCtx, aligned + total, windowBytes.data() + total, len - total);
      stats.readMs +=
          std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
      if (got <= 0) return false;
      const size_t took = static_cast<size_t>(got);
      if (took > len - total) return false;       // a reader must never report more than it was asked for
      stats.bytes += took;
      total += took;
    }
    windowStart = aligned;
    windowValid = total;
    windowFilled = true;
    return true;
  }

  // Copy `n` bytes at absolute `off` out of the external source. Straddles windows when it has to, and
  // never reads past externalSize.
  bool readExternal(size_t off, void* dst, size_t n) {
    if (!external || !readFn) return false;
    if (off > externalSize || n > externalSize - off) return false;   // never past the end
    uint8_t* out = static_cast<uint8_t*>(dst);
    size_t done = 0;
    while (done < n) {
      const size_t at = off + done;
      if (windowFilled && at >= windowStart && at < windowStart + windowValid) {
        externalStats().hits++;
      } else if (!fillWindow(at)) {
        return false;
      }
      const size_t inWindow = at - windowStart;
      if (inWindow >= windowValid) return false;      // short window at EOF
      size_t take = windowValid - inWindow;
      if (take > n - done) take = n - done;
      memcpy(out + done, windowBytes.data() + inWindow, take);
      done += take;
    }
    return true;
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
  // The HalFile ABI reports sizes and counts as int. A Blob can exceed INT_MAX (a large book on the
  // wasm32 side), so the contract is explicit here: clamp to INT_MAX instead of truncating into a
  // negative "plainly not available" answer, which is what an unchecked cast produces.
  int available() const {
    if (!blob_ || pos_ >= blob_->size) return 0;
    const size_t remaining = blob_->size - pos_;
    return remaining > static_cast<size_t>(INT_MAX) ? INT_MAX : static_cast<int>(remaining);
  }
  size_t position() const { return pos_; }
  int read(void* buf, size_t count) {
    if (!blob_ || pos_ >= blob_->size) return -1;
    size_t n = std::min(count, blob_->size - pos_);
    // A single read above INT_MAX cannot be reported through an int return, so clamp what is transferred
    // rather than let the count truncate. Callers loop on the returned count (see the external read
    // helpers), so a short read is progress, not a failure.
    n = std::min(n, static_cast<size_t>(INT_MAX));
    if (blob_->external) {
      // Range-backed: position alone decides what is fetched, so the caller's read pattern is
      // unchanged. A window fetch that fails is an I/O error, not EOF — report it as -1 the way the
      // memory path never has to.
      if (!blob_->readExternal(pos_, buf, n)) return -1;
      pos_ += n;
      return static_cast<int>(n);
    }
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
    if (count > SIZE_MAX - pos_) return 0;                 // a wrap here would resize to a small buffer
    const size_t wanted = pos_ + count;
    if (wanted > blob_->writable->size()) {
      if (!blob_->reserveWritableSize(wanted)) return 0;
      blob_->writable->resize(wanted);
    }
    blob_->refresh();
    memcpy(blob_->writable->data() + pos_, buf, count);
    blob_->zipDirectoryValidated = false;
    pos_ += count;
    return count;
  }
  size_t write(uint8_t b) override { return write(&b, 1); }
  bool isDirectory() const { return false; }
  bool close() { return true; }
  bool isOpen() const { return static_cast<bool>(blob_); }
  explicit operator bool() const { return isOpen(); }

  const std::string& path() const { return path_; }
  bool isZipDirectoryValidated() const { return blob_ && blob_->zipDirectoryValidated; }
  void markZipDirectoryValidated() {
    if (blob_) blob_->zipDirectoryValidated = true;
  }

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
  HalStorage() : derivedBudget_(std::make_shared<StorageBudget>()) {}
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

  // Mount a file whose bytes live OUTSIDE this address space, fetched a window at a time by `readFn`.
  // Nothing is made resident: the ZIP directory and one spine's CSS/XHTML are all that a first page
  // needs, so an 80 MB archive costs a few hundred KB of windows instead of 80 MB of heap plus the
  // 44 ms read and 14 ms copy it takes to get there. `readFn` returns the bytes written, or <= 0 on
  // failure; it must not read past `size`.
  void mountExternalBlob(const std::string& path, size_t size,
                         int (*readFn)(void* ctx, size_t offset, uint8_t* dst, size_t len),
                         void* ctx);

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

  size_t derivedBytes() const { return derivedBudget_->used; }
  size_t derivedByteLimit() const { return derivedBudget_->limit; }
  bool setDerivedByteLimit(size_t limit) {
    if (limit < derivedBudget_->used) return false;
    derivedBudget_->limit = limit;
    return true;
  }

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
      size_t add = 0;
      if (blob->external) {
        // Only the window is actually resident — the book itself is not in this heap. Counting the
        // whole file here was the shape of claim this accounting exists to refuse.
        add = blob->windowBytes.capacity();
      } else {
        add = blob->writable ? blob->writable->capacity() : blob->size;
      }
      total = add > SIZE_MAX - total ? SIZE_MAX : total + add;
    }
    return total;
  }

 private:
  std::map<std::string, std::shared_ptr<Blob>> files_;
  std::shared_ptr<StorageBudget> derivedBudget_;
};

#define Storage HalStorage::getInstance()
#define SdMan Storage  // compatibility alias used by some paths

// oflag helpers not provided by our enum
inline HalFile openFileForReadCompat(const char* p) { return Storage.open(p, O_RDONLY); }
