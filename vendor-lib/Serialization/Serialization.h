#pragma once
#include <HalStorage.h>

#include <iostream>

namespace serialization {
template <typename T>
void writePod(std::ostream& os, const T& value) {
  os.write(reinterpret_cast<const char*>(&value), sizeof(T));
}

template <typename T>
void writePod(HalFile& file, const T& value) {
  file.write(reinterpret_cast<const uint8_t*>(&value), sizeof(T));
}

template <typename T>
bool readPod(std::istream& is, T& value) {
  T decoded{};
  is.read(reinterpret_cast<char*>(&decoded), sizeof(T));
  if (!is) {
    value = T{};
    return false;
  }
  value = decoded;
  return true;
}

template <typename T>
bool readPod(HalFile& file, T& value) {
  T decoded{};
  if (file.read(reinterpret_cast<uint8_t*>(&decoded), sizeof(T)) != static_cast<int>(sizeof(T))) {
    value = T{};
    return false;
  }
  value = decoded;
  return true;
}

inline void writeString(std::ostream& os, const std::string& s) {
  const uint32_t len = s.size();
  writePod(os, len);
  os.write(s.data(), len);
}

inline void writeString(HalFile& file, const std::string& s) {
  const uint32_t len = s.size();
  writePod(file, len);
  file.write(reinterpret_cast<const uint8_t*>(s.data()), len);
}

static bool readString(std::istream& is, std::string& s, uint32_t maxLen = 4096) {
  uint32_t len = 0;
  if (!readPod(is, len)) {
    s.clear();
    return false;
  }
  if (len > maxLen) {
    s.clear();
    return false;
  }
  s.resize(len);
  if (len > 0) {
    is.read(&s[0], len);
    if (!is) {
      s.clear();
      return false;
    }
  }
  return true;
}

static bool readString(HalFile& file, std::string& s, uint32_t maxLen = 4096) {
  uint32_t len = 0;
  if (!readPod(file, len)) {
    s.clear();
    return false;
  }
  if (len > maxLen) {
    s.clear();
    return false;
  }
  s.resize(len);
  if (len > 0) {
    if (file.read(&s[0], len) != static_cast<int>(len)) {
      s.clear();
      return false;
    }
  }
  return true;
}
}  // namespace serialization
