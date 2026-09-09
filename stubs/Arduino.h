// Minimal Arduino-compat shim for the CrossPoint-KO render-core WASM/host port.
// Provides just enough of the Arduino surface the engine libs actually touch:
//   Print (abstract byte sink), min/max/abs, millis, delay, String (small subset).
#pragma once

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>

// ---- Print (abstract byte sink; parsers & HalFile derive from it) ----
class Print {
 public:
  virtual ~Print() = default;
  virtual size_t write(uint8_t b) = 0;
  virtual size_t write(const uint8_t* buffer, size_t size) { return writeBytes(buffer, size); }
  size_t write(const char* str) { return writeBytes(reinterpret_cast<const uint8_t*>(str), strlen(str)); }
  size_t write(const char* buffer, size_t size) {
    return writeBytes(reinterpret_cast<const uint8_t*>(buffer), size);
  }
  virtual size_t writeBytes(const uint8_t* buffer, size_t size) {
    size_t n = 0;
    while (n < size && write(buffer[n])) n++;
    return n;
  }
  int available() { return 0; }
  // Common helpers used by parser code paths
  size_t println(const char* s = "") { return write(s) + write("\n", 1); }
  size_t print(const char* s) { return write(s); }
};

// ---- Arduino String subset (only what engine headers reference) ----
class String {
 public:
  String() = default;
  String(const char* s) : s_(s ? s : "") {}
  explicit String(const std::string& s) : s_(s) {}
  String(int v) : s_(std::to_string(v)) {}
  String(long v) : s_(std::to_string(v)) {}
  String(unsigned long v) : s_(std::to_string(v)) {}
  String(double v, int /*decimals*/ = 2) : s_(std::to_string(v)) {}
  String(char c) : s_(1, c) {}

  const char* c_str() const { return s_.c_str(); }
  const char* begin() const { return s_.c_str(); }
  size_t length() const { return s_.size(); }
  size_t len() const { return s_.size(); }
  bool isEmpty() const { return s_.empty(); }
  bool equals(const char* o) const { return s_ == (o ? o : ""); }
  bool operator==(const String& o) const { return s_ == o.s_; }
  bool operator==(const char* o) const { return s_ == (o ? o : ""); }
  bool operator!=(const String& o) const { return !(*this == o); }
  bool operator!=(const char* o) const { return !(*this == o); }
  String& operator+=(const char* o) {
    s_ += o ? o : "";
    return *this;
  }
  String& operator+=(char c) {
    s_ += c;
    return *this;
  }
  String& operator+=(const String& o) {
    s_ += o.s_;
    return *this;
  }
  String operator+(const char* o) const { return String(s_ + (o ? o : "")); }
  String operator+(const String& o) const { return String(s_ + o.s_); }
  char operator[](unsigned i) const { return i < s_.size() ? s_[i] : '\0'; }
  int indexOf(char c) const {
    auto p = s_.find(c);
    return p == std::string::npos ? -1 : static_cast<int>(p);
  }
  int indexOf(const char* sub) const {
    auto p = s_.find(sub ? sub : "");
    return p == std::string::npos ? -1 : static_cast<int>(p);
  }
  int lastIndexOf(char c) const {
    auto p = s_.rfind(c);
    return p == std::string::npos ? -1 : static_cast<int>(p);
  }
  String substring(unsigned from, unsigned to = std::string::npos) const {
    return String(s_.substr(from, to == std::string::npos ? std::string::npos : to - from));
  }
  void toLowerCase() { std::transform(s_.begin(), s_.end(), s_.begin(), [](unsigned char c) { return std::tolower(c); }); }
  void toUpperCase() { std::transform(s_.begin(), s_.end(), s_.begin(), [](unsigned char c) { return std::toupper(c); }); }
  long toInt() const { return strtol(s_.c_str(), nullptr, 10); }
  double toDouble() const { return strtod(s_.c_str(), nullptr); }
  bool startsWith(const char* p) const { return s_.rfind(p ? p : "", 0) == 0; }
  bool endsWith(const char* p) const {
    if (!p) return false;
    size_t l = strlen(p);
    return s_.size() >= l && s_.compare(s_.size() - l, l, p) == 0;
  }
  explicit operator bool() const { return !s_.empty(); }
  bool operator>(const String& o) const { return s_ > o.s_; }
  bool operator<(const String& o) const { return s_ < o.s_; }
  const std::string& str() const { return s_; }

 private:
  std::string s_;
};

inline String operator+(const char* a, const String& b) { return String(a) + b; }

// ---- Free functions ----
inline uint32_t millis() { return 0; }
inline uint32_t micros() { return 0; }
inline void delay(uint32_t) {}
inline void yield() {}
using min = int;  // never used; min() comes from <algorithm> via std::min

// Arduino stringify macro
#define STR(x) #x

// pgmspace-ish no-op (bitmap fonts are plain const arrays here)
#define PROGMEM
#define pgm_read_byte(p) (*(const uint8_t*)(p))
#define pgm_read_word(p) (*(const uint16_t*)(p))
#define pgm_read_dword(p) (*(const uint32_t*)(p))
#define pgm_read_ptr(p) (*(void* const*)(p))

// HWCDC etc. unused in engine
typedef void* StreamPtr;

// ---- ESP32 heap API shim: host/WASM builds always report ample heap -------
class EspClass {
 public:
  uint32_t getFreeHeap() const { return 1024u * 1024 * 512; }
  uint32_t getMaxAllocHeap() const { return 1024u * 1024 * 256; }
  uint32_t getMinFreeHeap() const { return 1024u * 1024 * 128; }
  uint32_t getHeapSize() const { return 1024u * 1024 * 512; }
  uint32_t getPsramSize() const { return 0; }
  uint32_t getFreePsram() const { return 0; }
  void restart() const { std::abort(); }
};
extern EspClass ESP;

