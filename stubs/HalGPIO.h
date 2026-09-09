// HalGPIO.h — host/WASM stub (button/input layer unused headless).
#pragma once

#include <Arduino.h>

#include <cstdint>

class HalGPIO {
 public:
  enum class DeviceType : uint8_t { X4, X3 };

  void begin() {}
  void update() {}
  bool isPressed(uint8_t buttonIndex) const {
    (void)buttonIndex;
    return false;
  }
  bool wasPressed(uint8_t buttonIndex) const {
    (void)buttonIndex;
    return false;
  }
  bool wasAnyPressed() const { return false; }
  bool wasReleased(uint8_t buttonIndex) const {
    (void)buttonIndex;
    return false;
  }
  bool wasAnyReleased() const { return false; }
  bool hasTouch() const { return false; }
  bool wasTouchTap(float& nx, float& ny) const {
    (void)nx;
    (void)ny;
    return false;
  }
  bool wasTouchDown(float& nx, float& ny) const {
    (void)nx;
    (void)ny;
    return false;
  }
  bool isTouchTapCandidate(float& nx, float& ny, unsigned long& heldMs) const {
    (void)nx; (void)ny; (void)heldMs;
    return false;
  }
  bool isTouchHeldAt(float& nx, float& ny) const {
    (void)nx;
    (void)ny;
    return false;
  }
  bool wasSwipe(float& nxStart, float& nyStart, float& nxEnd, float& nyEnd) const {
    (void)nxStart; (void)nyStart; (void)nxEnd; (void)nyEnd;
    return false;
  }
  bool wasTouchActivity() const { return false; }
  void setSharedConfirmPowerShortPressEmitsPower(bool enabled) { (void)enabled; }
  bool verifyPowerButtonWakeup(uint16_t requiredDurationMs, bool shortPressAllowed) const {
    (void)requiredDurationMs;
    (void)shortPressAllowed;
    return false;
  }
  bool isUsbConnected() const { return false; }
  bool wasUsbStateChanged() const { return false; }
  bool isXteinkDevice() const { return true; }
  bool deviceIsX3() const { return false; }
  bool deviceIsX4() const { return true; }
  void setDeviceType(DeviceType t) { (void)t; }
};
