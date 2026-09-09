// HalDisplay.h — host/WASM stub: a RAM-backed "panel" with the same geometry
// contract as the real device (physical 800x480 landscape; GfxRenderer applies
// Portrait rotation so logical space is 480x800). All refresh/display calls
// are no-ops; buffers are plain memory the driver reads out after each page.
#pragma once

#include <Arduino.h>  // no-op shim, provides nothing we need but keeps parity

#include <cstdint>
#include <cstdlib>
#include <cstring>

class HalDisplay {
 public:
  // Physical panel geometry (matches FreeInk EInkDisplay facade the KO build uses)
  static constexpr uint16_t DISPLAY_WIDTH = 800;   // physical x
  static constexpr uint16_t DISPLAY_HEIGHT = 480;  // physical y
  static constexpr uint16_t DISPLAY_WIDTH_BYTES = DISPLAY_WIDTH / 8;
  static constexpr uint32_t BUFFER_SIZE = DISPLAY_WIDTH_BYTES * DISPLAY_HEIGHT;  // 48000

  enum RefreshMode {
    FULL_REFRESH,
    HALF_REFRESH,
    FAST_REFRESH
  };

  HalDisplay() {
    frameBuffer = static_cast<uint8_t*>(malloc(BUFFER_SIZE));
    lsbPlane_ = static_cast<uint8_t*>(malloc(BUFFER_SIZE));
    msbPlane_ = static_cast<uint8_t*>(malloc(BUFFER_SIZE));
    memset(frameBuffer, 0xFF, BUFFER_SIZE);
    memset(lsbPlane_, 0xFF, BUFFER_SIZE);
    memset(msbPlane_, 0xFF, BUFFER_SIZE);
  }
  ~HalDisplay() {
    free(frameBuffer);
    free(lsbPlane_);
    free(msbPlane_);
  }

  void begin(bool seamless = false) { (void)seamless; }

  // ---- framebuffer ops (memory only) ----
  void clearScreen(uint8_t color = 0xFF) const { memset(frameBuffer, color, BUFFER_SIZE); }
  void drawImage(const uint8_t* imageData, uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                 bool fromProgmem = false) const {
    (void)fromProgmem;
    // 1-bit row-major image blit into the framebuffer at (x,y). Used by
    // Bitmap draw paths for UI/images; page text goes through drawPixel.
    if (!imageData) return;
    for (uint16_t row = 0; row < h && (y + row) < DISPLAY_HEIGHT; row++) {
      if (y + row >= DISPLAY_HEIGHT) break;
      const uint8_t* src = imageData + static_cast<size_t>(row) * ((w + 7) / 8);
      for (uint16_t col = 0; col < w && (x + col) < DISPLAY_WIDTH; col++) {
        if (x + col >= DISPLAY_WIDTH) break;
        const uint8_t bit = (src[col >> 3] >> (7 - (col & 7))) & 1;
        uint8_t& px = frameBuffer[static_cast<size_t>(y + row) * DISPLAY_WIDTH_BYTES + ((x + col) >> 3)];
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
    if (lsbBuffer) memcpy(lsbPlane_, lsbBuffer, BUFFER_SIZE);
  }
  void copyGrayscaleMsbBuffers(const uint8_t* msbBuffer) const {
    if (msbBuffer) memcpy(msbPlane_, msbBuffer, BUFFER_SIZE);
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
    if (!rows || yStart + numRows > DISPLAY_HEIGHT) return;
    memcpy(dst + static_cast<size_t>(yStart) * DISPLAY_WIDTH_BYTES, rows,
           static_cast<size_t>(numRows) * DISPLAY_WIDTH_BYTES);
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
    if (size) *size = BUFFER_SIZE;
    return frameBuffer;
  }
  void returnFrameBufferStorage() const {}

  // ---- runtime geometry ----
  uint16_t getDisplayWidth() const { return DISPLAY_WIDTH; }
  uint16_t getDisplayHeight() const { return DISPLAY_HEIGHT; }
  uint16_t getDisplayWidthBytes() const { return DISPLAY_WIDTH_BYTES; }
  uint32_t getBufferSize() const { return BUFFER_SIZE; }

  // For the driver: direct access to the gray planes
  const uint8_t* getLsbPlane() const { return lsbPlane_; }
  const uint8_t* getMsbPlane() const { return msbPlane_; }

 private:
  uint8_t* frameBuffer = nullptr;
  uint8_t* lsbPlane_ = nullptr;
  uint8_t* msbPlane_ = nullptr;
};

extern HalDisplay display;  // global instance (engine references `display` in places)
