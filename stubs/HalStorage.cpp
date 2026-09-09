// HalStorage.cpp — in-memory FS implementation for the KO-engine port.
#include "HalStorage.h"

#include <algorithm>
#include <set>

void HalStorage::mountBlob(const std::string& path, const uint8_t* data, size_t size) {
  std::string p = path;
  if (p.empty() || p[0] != '/') p = "/" + p;
  auto buf = std::make_shared<std::vector<uint8_t>>(data, data + size);
  files_[p] = std::move(buf);
  // Also register under the SD-ish root view if caller used a bare filename
  if (p.find('/') != std::string::npos) {
    // keep a flat alias: basename
    std::string base = p.substr(p.rfind('/') + 1);
    files_["/" + base] = files_[p];
  }
}

String HalStorage::readFile(const char* path) {
  auto it = files_.find(path ? path : "");
  if (it == files_.end()) return String();
  return String(std::string(reinterpret_cast<const char*>(it->second->data()), it->second->size()));
}

bool HalStorage::readFileToStream(const char* path, Print& out, size_t chunkSize) {
  auto it = files_.find(path ? path : "");
  if (it == files_.end()) return false;
  const auto& v = *it->second;
  size_t off = 0;
  while (off < v.size()) {
    size_t n = std::min(chunkSize, v.size() - off);
    size_t w = out.write(v.data() + off, n);
    if (w != n) return false;
    off += n;
  }
  return true;
}

size_t HalStorage::readFileToBuffer(const char* path, char* buffer, size_t bufferSize, size_t maxBytes) {
  if (!buffer || bufferSize == 0) return 0;
  auto it = files_.find(path ? path : "");
  if (it == files_.end()) return 0;
  const auto& v = *it->second;
  size_t n = std::min({v.size(), bufferSize - 1, maxBytes ? maxBytes : v.size()});
  memcpy(buffer, v.data(), n);
  buffer[n] = '\0';
  return n;
}

bool HalStorage::writeFile(const char* path, const String& content) {
  const std::string& s = content.str();
  mountBlob(path, reinterpret_cast<const uint8_t*>(s.data()), s.size());
  return true;
}

bool HalStorage::ensureDirectoryExists(const char* path) {
  (void)path;
  return true;
}

HalFile HalStorage::open(const char* path, const oflag_t oflag) {
  (void)oflag;
  auto it = files_.find(path ? path : "");
  if (it == files_.end()) return HalFile();
  return HalFile(it->first, it->second);
}

bool HalStorage::exists(const char* path) { return files_.count(path ? path : "") > 0; }

bool HalStorage::remove(const char* path) { return files_.erase(path ? path : "") > 0; }

bool HalStorage::rename(const char* oldPath, const char* newPath) {
  auto it = files_.find(oldPath ? oldPath : "");
  if (it == files_.end()) return false;
  auto blob = it->second;
  files_.erase(it);
  files_[newPath ? newPath : ""] = std::move(blob);
  return true;
}

bool HalStorage::openFileForRead(const char* moduleName, const char* path, HalFile& file) {
  (void)moduleName;
  auto it = files_.find(path ? path : "");
  if (it == files_.end()) return false;
  file = HalFile(it->first, it->second);
  return true;
}

bool HalStorage::openFileForWrite(const char* moduleName, const char* path, HalFile& file) {
  (void)moduleName;
  auto buf = std::make_shared<std::vector<uint8_t>>();
  std::string p = path ? path : "";
  files_[p] = buf;  // create/truncate
  file = HalFile(p, buf);
  return true;
}

std::vector<std::string> HalStorage::debugPaths() const {
  std::vector<std::string> out;
  out.reserve(files_.size());
  for (const auto& kv : files_) out.push_back(kv.first);
  return out;
}
