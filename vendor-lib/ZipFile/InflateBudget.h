#pragma once

#include <cstdint>

// Shared, monotonic work budget for ZIP member expansion. Callers choose the
// policy limit; ZipFile charges the member's declared uncompressed size after
// validating its local header and before reading or inflating any payload.
// Failed/early-stopped reads intentionally do not refund the charge: repeated
// hostile attempts are work too.
class InflateBudget final {
 public:
  explicit InflateBudget(const uint64_t limitBytes) : limitBytes_(limitBytes) {}

  bool charge(const uint64_t bytes) {
    if (bytes > limitBytes_ - usedBytes_) return false;
    usedBytes_ += bytes;
    return true;
  }

  uint64_t usedBytes() const { return usedBytes_; }
  uint64_t limitBytes() const { return limitBytes_; }
  uint64_t remainingBytes() const { return limitBytes_ - usedBytes_; }

 private:
  uint64_t limitBytes_ = 0;
  uint64_t usedBytes_ = 0;
};
