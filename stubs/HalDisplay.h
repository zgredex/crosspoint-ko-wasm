// HalDisplay.h — host/WASM stub: a RAM-backed panel with the same runtime
// geometry contract as CrossPoint-KO's FreeInk display facade. X4 is physical
// 800x480; X3 is physical 792x528. GfxRenderer applies the firmware's Portrait
// rotation, producing logical 480x800 and 528x792 respectively.
#pragma once

#include <Arduino.h>  // no-op shim, provides nothing we need but keeps parity

#include <cstdint>
#include <cstdlib>
#include <cstring>

#include "device_profile.h"

class HalDisplay {
 public:
  // Compile-time defaults retained because the Korean renderer uses these to
  // initialise fields before begin(). begin() immediately replaces them with
  // the facade's runtime getters, exactly as it does on the device.
  static constexpr uint16_t DISPLAY_WIDTH = 800;   // physical x
  static constexpr uint16_t DISPLAY_HEIGHT = 480;  // physical y
  static constexpr uint16_t DISPLAY_WIDTH_BYTES = DISPLAY_WIDTH / 8;
  static constexpr uint32_t BUFFER_SIZE = DISPLAY_WIDTH_BYTES * DISPLAY_HEIGHT;  // 48000
  static constexpr uint32_t MAX_BUFFER_SIZE = 52272;  // X3: 99 bytes x 528 rows

  enum RefreshMode {
    FULL_REFRESH,
    HALF_REFRESH,
    FAST_REFRESH
  };

  HalDisplay() {
    frameBuffer = static_cast<uint8_t*>(malloc(MAX_BUFFER_SIZE));
    lsbPlane_ = static_cast<uint8_t*>(malloc(MAX_BUFFER_SIZE));
    msbPlane_ = static_cast<uint8_t*>(malloc(MAX_BUFFER_SIZE));
    memset(frameBuffer, 0xFF, MAX_BUFFER_SIZE);
    memset(lsbPlane_, 0xFF, MAX_BUFFER_SIZE);
    memset(msbPlane_, 0xFF, MAX_BUFFER_SIZE);
  }
  ~HalDisplay() {
    free(frameBuffer);
    free(lsbPlane_);
    free(msbPlane_);
  }

  void begin(bool seamless = false) { (void)seamless; }

  void setDeviceProfile(ko::DeviceProfile profile) {
    profile_ = profile;
    const auto& g = ko::deviceGeometry(profile);
    panelWidth_ = g.physicalWidth;
    panelHeight_ = g.physicalHeight;
    panelWidthBytes_ = g.physicalRowBytes;
    bufferSize_ = static_cast<uint32_t>(g.planeBytes);
    memset(frameBuffer, 0xFF, MAX_BUFFER_SIZE);
    memset(lsbPlane_, 0xFF, MAX_BUFFER_SIZE);
    memset(msbPlane_, 0xFF, MAX_BUFFER_SIZE);
  }
  ko::DeviceProfile getDeviceProfile() const { return profile_; }

  // ---- framebuffer ops (memory only) ----
  void clearScreen(uint8_t color = 0xFF) const { memset(frameBuffer, color, bufferSize_); }
  void drawImage(const uint8_t* imageData, uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                 bool fromProgmem = false) const {
    (void)fromProgmem;
    // 1-bit row-major image blit into the framebuffer at (x,y). Used by
    // Bitmap draw paths for UI/images; page text goes through drawPixel.
    if (!imageData) return;
    for (uint16_t row = 0; row < h && (y + row) < panelHeight_; row++) {
      if (y + row >= panelHeight_) break;
      const uint8_t* src = imageData + static_cast<size_t>(row) * ((w + 7) / 8);
      for (uint16_t col = 0; col < w && (x + col) < panelWidth_; col++) {
        if (x + col >= panelWidth_) break;
        const uint8_t bit = (src[col >> 3] >> (7 - (col & 7))) & 1;
        uint8_t& px = frameBuffer[static_cast<size_t>(y + row) * panelWidthBytes_ + ((x + col) >> 3)];
        if (bit) px &= static_cast<uint8_t>(~(1 << (7 - ((x + col) & 7))));  // set black (0)
      }
    }
  }
  void drawImageTransparent(const uint8_t* imageData, uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                            bool fromProgmem = false) const {
    (void)fromProgmem;
    drawImage(imageData, x, y, w, h, false);
  }

  // ---- refresh: no-ops ----
  void displayBuffer(RefreshMode mode = RefreshMode::FAST_REFRESH, bool turnOffScreen = false) const {
    (void)mode;
    (void)turnOffScreen;
  }
  void refreshDisplay(RefreshMode mode = RefreshMode::FAST_REFRESH, bool turnOffScreen = false) const {
    (void)mode;
    (void)turnOffScreen;
  }
  void deepSleep() {}

  // ---- frame buffer access ----
  uint8_t* getFrameBuffer() const { return frameBuffer; }

  // ---- grayscale plane handling (memory) ----
  void copyGrayscaleLsbBuffers(const uint8_t* lsbBuffer) const {
    if (lsbBuffer) memcpy(lsbPlane_, lsbBuffer, bufferSize_);
  }
  void copyGrayscaleMsbBuffers(const uint8_t* msbBuffer) const {
    if (msbBuffer) memcpy(msbPlane_, msbBuffer, bufferSize_);
  }
  void copyGrayscaleBuffers(const uint8_t* lsbBuffer, const uint8_t* msbBuffer) const {
    copyGrayscaleLsbBuffers(lsbBuffer);
    copyGrayscaleMsbBuffers(msbBuffer);
  }
  void cleanupGrayscaleBuffers(const uint8_t* bwBuffer) const {
    (void)bwBuffer;
  }
  void displayGrayBuffer(bool turnOffScreen = false) const { (void)turnOffScreen; }

  // Tiled grayscale: accumulate strip rows into the matching RAM plane.
  // lsbPlane==true → "dark grey only" plane (XTH bit semantics are applied by
  // the driver/encoder later; here we just store raw mask bits as drawn).
  void writeGrayscalePlaneStrip(bool lsbPlane, const uint8_t* rows, uint16_t yStart, uint16_t numRows) const {
    uint8_t* dst = lsbPlane ? lsbPlane_ : msbPlane_;
    if (!rows || yStart + numRows > panelHeight_) return;
    memcpy(dst + static_cast<size_t>(yStart) * panelWidthBytes_, rows,
           static_cast<size_t>(numRows) * panelWidthBytes_);
  }
  bool supportsStripGrayscale() const { return true; }
  void displayGrayscaleBase(RefreshMode mode = RefreshMode::FAST_REFRESH, bool turnOffScreen = false) const {
    (void)mode;
    (void)turnOffScreen;
  }
  void preconditionGrayscale() const {}
  void preconditionGrayscale(uint16_t x, uint16_t y, uint16_t w, uint16_t h) const {
    (void)x; (void)y; (void)w; (void)h;
  }
  void grayscaleRevert() const {}

  // ---- async refresh / buffer lend: no-ops with sane storage ----
  void displayBufferAsync(RefreshMode mode = RefreshMode::FAST_REFRESH) const { (void)mode; }
  void waitRefreshComplete() const {}
  bool supportsAsyncRefresh() const { return false; }
  uint8_t* lendFrameBufferStorage(uint32_t* size) const {
    if (size) *size = bufferSize_;
    return frameBuffer;
  }
  void returnFrameBufferStorage() const {}

  // ---- runtime geometry ----
  uint16_t getDisplayWidth() const { return panelWidth_; }
  uint16_t getDisplayHeight() const { return panelHeight_; }
  uint16_t getDisplayWidthBytes() const { return panelWidthBytes_; }
  uint32_t getBufferSize() const { return bufferSize_; }

  // For the driver: direct access to the gray planes
  const uint8_t* getLsbPlane() const { return lsbPlane_; }
  const uint8_t* getMsbPlane() const { return msbPlane_; }

 private:
  uint8_t* frameBuffer = nullptr;
  uint8_t* lsbPlane_ = nullptr;
  uint8_t* msbPlane_ = nullptr;
  ko::DeviceProfile profile_ = ko::DeviceProfile::X4;
  uint16_t panelWidth_ = DISPLAY_WIDTH;
  uint16_t panelHeight_ = DISPLAY_HEIGHT;
  uint16_t panelWidthBytes_ = DISPLAY_WIDTH_BYTES;
  uint32_t bufferSize_ = BUFFER_SIZE;
};

extern HalDisplay display;  // global instance (engine references `display` in places)
