// Device geometry used by the Korean fork's runtime-selected Xteink profiles.
//
// CrossPoint-KO calls EInkDisplay::setDisplayX3() before display.begin(); the
// FreeInk facade then reports the selected panel through getDisplayWidth(),
// getDisplayHeight(), getDisplayWidthBytes(), and getBufferSize().  Keep the
// same distinction here: these are PHYSICAL framebuffer dimensions.  The
// renderer turns them into portrait logical dimensions by swapping the axes.
#pragma once

#include <cstddef>
#include <cstdint>

namespace ko {

enum class DeviceProfile : uint8_t {
  X3 = 3,
  X4 = 4,
};

struct DeviceGeometry {
  DeviceProfile profile;
  const char* name;
  uint16_t physicalWidth;
  uint16_t physicalHeight;
  uint16_t portraitWidth;
  uint16_t portraitHeight;
  uint16_t physicalRowBytes;
  size_t planeBytes;
};

inline constexpr DeviceGeometry kX4Geometry{
    DeviceProfile::X4, "x4", 800, 480, 480, 800, 100, 48000};
inline constexpr DeviceGeometry kX3Geometry{
    DeviceProfile::X3, "x3", 792, 528, 528, 792, 99, 52272};

inline constexpr bool isDeviceProfile(int value) {
  return value == static_cast<int>(DeviceProfile::X3) ||
         value == static_cast<int>(DeviceProfile::X4);
}

inline constexpr const DeviceGeometry& deviceGeometry(DeviceProfile profile) {
  return profile == DeviceProfile::X3 ? kX3Geometry : kX4Geometry;
}

inline constexpr const DeviceGeometry& deviceGeometry(int profile) {
  return deviceGeometry(profile == static_cast<int>(DeviceProfile::X3)
                            ? DeviceProfile::X3
                            : DeviceProfile::X4);
}

}  // namespace ko
