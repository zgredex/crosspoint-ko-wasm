// HalStorage.cpp — in-memory FS implementation for the KO-engine port.
#include "HalStorage.h"

#include <algorithm>
#include <cstdlib>
#include <set>

namespace {
std::string normalisePath(const std::string& path) {
  if (path.empty() || path[0] != '/') return "/" + path;
  return path;
}
}  // namespace

void HalStorage::mountBlob(const std::string& path, const uint8_t* data, size_t size) {
  const std::string p = normalisePath(path);
  auto blob = std::make_shared<Blob>();
  blob->writable = std::make_shared<std::vector<uint8_t>>();
  if (size != 0) {
    if (!data) return;
    blob->writable->assign(data, data + size);
  }
  blob->refresh();
  files_[p] = std::move(blob);
}

void HalStorage::mountOwnedBlob(const std::string& path, uint8_t* data, size_t size) {
  const std::string p = normalisePath(path);
  auto blob = std::make_shared<Blob>();
  // The deleter is the ONLY owner: the caller promises not to free `data` itself.
  blob->owner = std::shared_ptr<void>(data, [](void* p) { std::free(p); });
  blob->data = data;
  blob->size = size;
  files_[p] = std::move(blob);
}

void HalStorage::mountExternalBlob(const std::string& path, size_t size,
                                   int (*readFn)(void* ctx, size_t offset, uint8_t* dst, size_t len),
                                   void* ctx, bool immutableBacking) {
  const std::string p = normalisePath(path);
  auto blob = std::make_shared<Blob>();
  blob->external = true;
  blob->externalImmutable = immutableBacking;
  blob->externalSize = size;
  blob->readFn = readFn;
  blob->readCtx = ctx;
  blob->refresh();                 // sets `size` from externalSize; data stays null on purpose
  files_[p] = std::move(blob);
}

// One process-wide counter set: the numbers describe the mount, and there is at most one mounted book.
ExternalStats& externalStats() {
  static ExternalStats stats;
  return stats;
}

// These three used to read `Blob::data` directly. That was fine while every Blob was RAM-backed, and
// became a null dereference the moment a third representation existed: an external Blob has data == null
// by design, because its bytes live outside this address space. They now go through HalFile::read(), so
// the backing type is the storage's business and no helper above it can crash on a valid representation.
String HalStorage::readFile(const char* path) {
  HalFile f = open(path, O_RDONLY);
  if (!f) return String();
  const size_t n = f.size();
  std::vector<char> buf(n, '\0');
  if (n > 0) {
    const int got = f.read(buf.data(), n);
    if (got < 0) return String();
    buf.resize(static_cast<size_t>(got));
  }
  return String(std::string(buf.begin(), buf.end()));
}

bool HalStorage::readFileToStream(const char* path, Print& out, size_t chunkSize) {
  HalFile f = open(path, O_RDONLY);
  if (!f) return false;
  if (chunkSize == 0) chunkSize = 256;
  std::vector<uint8_t> tmp(chunkSize);
  while (f.position() < f.size()) {
    const size_t want = std::min(tmp.size(), f.size() - f.position());
    const int got = f.read(tmp.data(), want);
    if (got <= 0) return false;                 // a failed read is an error, not EOF
    if (out.write(tmp.data(), static_cast<size_t>(got)) != static_cast<size_t>(got)) return false;
  }
  return true;
}

size_t HalStorage::readFileToBuffer(const char* path, char* buffer, size_t bufferSize, size_t maxBytes) {
  if (!buffer || bufferSize == 0) return 0;
  HalFile f = open(path, O_RDONLY);
  if (!f) return 0;
  const size_t limit = maxBytes ? std::min(maxBytes, bufferSize - 1) : bufferSize - 1;
  const size_t wanted = std::min(f.size(), limit);
  if (wanted == 0) { buffer[0] = '\0'; return 0; }
  const int got = f.read(buffer, wanted);
  if (got < 0) { buffer[0] = '\0'; return 0; }
  buffer[got] = '\0';
  return static_cast<size_t>(got);
}

bool HalStorage::writeFile(const char* path, const String& content) {
  const std::string& s = content.str();
  HalFile file;
  if (!openFileForWrite("writeFile", path, file)) return false;
  return s.empty() || file.write(s.data(), s.size()) == s.size();
}

bool HalStorage::ensureDirectoryExists(const char* path) {
  (void)path;
  return true;
}

HalFile HalStorage::open(const char* path, const oflag_t oflag) {
  (void)oflag;
  const std::string key = normalisePath(path ? path : "");
  auto it = files_.find(key);
  if (it == files_.end()) return HalFile();
  return HalFile(it->first, it->second);
}

bool HalStorage::exists(const char* path) {
  return files_.count(normalisePath(path ? path : "")) > 0;
}

bool HalStorage::remove(const char* path) {
  const std::string key = normalisePath(path ? path : "");
  return files_.erase(key) != 0;
}

bool HalStorage::rename(const char* oldPath, const char* newPath) {
  const std::string oldKey = normalisePath(oldPath ? oldPath : "");
  const std::string newKey = normalisePath(newPath ? newPath : "");
  auto it = files_.find(oldKey);
  if (it == files_.end()) return false;

  auto blob = std::move(it->second);
  files_.erase(it);
  files_[newKey] = std::move(blob);
  return true;
}

bool HalStorage::openFileForRead(const char* moduleName, const char* path, HalFile& file) {
  (void)moduleName;
  // normalisePath, like EVERY other accessor in this class. mountBlob/mountOwnedBlob store under the
  // normalised key, so looking up the raw argument missed the mount for any path without a leading '/':
  // the blob was mounted, the ZIP reader could not find it, and it surfaced far from here as
  // "Could not find or size META-INF/container.xml" followed by "Could not find content.opf in zip".
  //
  // Idempotent for absolute paths, so this changes behaviour ONLY for the paths that used to fail — which
  // is why a relative EPUB loaded in neither the port binary nor the reference binary while every gate,
  // every gate fixture and the browser path (which mounts "/book.epub") passed.
  const std::string p = normalisePath(path ? path : "");
  auto it = files_.find(p);
  if (it == files_.end()) return false;
  file = HalFile(it->first, it->second);
  return true;
}

bool HalStorage::openFileForWrite(const char* moduleName, const char* path, HalFile& file) {
  (void)moduleName;
  auto blob = std::make_shared<Blob>();
  blob->writable = std::make_shared<std::vector<uint8_t>>();
  blob->budget = derivedBudget_;
  blob->refresh();
  // Same key rule as the read side, so a relative write cannot create a second entry that exists()/open()
  // (both normalised) would never see.
  const std::string p = normalisePath(path ? path : "");
  files_[p] = std::move(blob);  // create/truncate
  file = HalFile(p, files_[p]);
  return true;
}

std::vector<std::string> HalStorage::debugPaths() const {
  std::vector<std::string> out;
  out.reserve(files_.size());
  for (const auto& kv : files_) out.push_back(kv.first);
  return out;
}
