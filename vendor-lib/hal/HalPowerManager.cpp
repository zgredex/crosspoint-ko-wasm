#include "HalPowerManager.h"

#include <BoardConfig.h>
#include <Logging.h>
#include <PowerManager.h>
#include <WiFi.h>
#include <esp_sleep.h>
#include <soc/soc_caps.h>

#include <cassert>

#include "HalGPIO.h"

HalPowerManager powerManager;  // Singleton instance

namespace {

// Korean-build correction for the ADC battery path (X4). The SDK's voltage->percent polynomial
// only returns 100% at >= 4.136 V, but a fully charged cell read through the 2:1 divider on GPIO0
// measures ~4.06 V because of the ESP32-C3 ADC gain error, so a full device tops out near 91%.
//
// Only the top of the curve is stretched onto 100%; anything at or below the knee is passed
// through untouched so low-battery reporting keeps the SDK's calibration. To retune, charge to
// full, read the raw percentage from the "PWR" log line, and set OBSERVED_FULL to it.
//
// Not applied to the I2C gauge path (X3, X4 Pro, LilyGo): those report true SoC from a fuel gauge.
constexpr uint16_t BATTERY_SCALE_KNEE = 80;
constexpr uint16_t BATTERY_OBSERVED_FULL = 91;

uint16_t scaleAdcBatteryPercent(const uint16_t raw) {
  static_assert(BATTERY_OBSERVED_FULL > BATTERY_SCALE_KNEE, "knee must sit below the observed full");
  if (raw <= BATTERY_SCALE_KNEE) return raw;
  if (raw >= BATTERY_OBSERVED_FULL) return 100;
  constexpr uint32_t inSpan = BATTERY_OBSERVED_FULL - BATTERY_SCALE_KNEE;
  constexpr uint32_t outSpan = 100 - BATTERY_SCALE_KNEE;
  const uint32_t scaled = ((raw - BATTERY_SCALE_KNEE) * outSpan + inSpan / 2) / inSpan;
  return static_cast<uint16_t>(BATTERY_SCALE_KNEE + scaled);
}

}  // namespace

void HalPowerManager::begin() {
  if (BoardConfig::ACTIVE.batteryAdc >= 0) {
    pinMode(BoardConfig::ACTIVE.batteryAdc, INPUT);
  }
  normalFreq = getCpuFrequencyMhz();
  modeMutex = xSemaphoreCreateMutex();
  assert(modeMutex != nullptr);
}

void HalPowerManager::setPowerSaving(bool enabled) {
  if (normalFreq <= 0) {
    return;  // invalid state
  }

  auto wifiMode = WiFi.getMode();
  if (wifiMode != WIFI_MODE_NULL) {
    // Wifi is active, force disabling power saving
    enabled = false;
  }

  // Note: We don't use mutex here to avoid too much overhead,
  // it's not very important if we read a slightly stale value for currentLockMode
  const LockMode mode = currentLockMode;

  if (mode == None && enabled && !isLowPower) {
    LOG_DBG("PWR", "Going to low-power mode");
    if (!setCpuFrequencyMhz(LOW_POWER_FREQ)) {
      LOG_DBG("PWR", "Failed to set CPU frequency = %d MHz", LOW_POWER_FREQ);
      return;
    }
    isLowPower = true;

  } else if ((!enabled || mode != None) && isLowPower) {
    LOG_DBG("PWR", "Restoring normal CPU frequency");
    if (!setCpuFrequencyMhz(normalFreq)) {
      LOG_DBG("PWR", "Failed to set CPU frequency = %d MHz", normalFreq);
      return;
    }
    isLowPower = false;
  }

  // Otherwise, no change needed
}

void HalPowerManager::startDeepSleep(HalGPIO& gpio) const {
#ifdef ENABLE_SERIAL_LOG
  // Tear down HWCDC so the host sees a clean disconnect and the peripheral
  // doesn't hold power domains that interfere with USB-powered GPIO wake.
  // logSerial is the raw HWCDC reference; Serial is the MySerialImpl proxy
  // (which doesn't expose end()).
  logSerial.end();
#endif

#if !SOC_PM_SUPPORT_EXT1_WAKEUP
  if (gpio.isXteinkDevice() && !gpio.deviceIsX3()) {
    // X4 GPIO13 is connected to the battery latch MOSFET. Keeping it low powers
    // the MCU off on battery, while the SDK wake source still handles USB power.
    constexpr gpio_num_t GPIO_SPIWP = GPIO_NUM_13;
    gpio_set_direction(GPIO_SPIWP, GPIO_MODE_OUTPUT);
    gpio_set_level(GPIO_SPIWP, 0);
    gpio_hold_en(GPIO_SPIWP);
  }
#endif

  // Cut the gated peripheral rails (touch/SD/EPD on boards like the Sticky) and
  // hold the enables off through deep sleep — otherwise the GT911 and SD card
  // stay powered all through "off" and drain the battery. No-op on boards with
  // no switched rails (X4/X3). Trade-off: no touch-to-wake; wake is the power
  // button. Must run after display.deepSleep() so the panel controller gets its
  // deep-sleep command while its rail is still up (enterDeepSleep() in main.cpp
  // guarantees that ordering).
  freeink::PowerManager::powerDownRailsForSleep();

  // Waits for the power button to be physically released (so holding it doesn't
  // immediately wake the device again), then arms the wake source and sleeps.
  freeink::PowerManager::deepSleepUntilPowerButton();
}

uint16_t HalPowerManager::getBatteryPercentage() const {
  static const BatteryMonitor battery;
  if (BoardConfig::ACTIVE.batteryGauge.gaugeAddr != 0) {
    const unsigned long now = millis();
    if (_batteryLastPollMs != 0 && (now - _batteryLastPollMs) < BATTERY_POLL_MS) {
      return _batteryCachedPercent;
    }

    _batteryLastPollMs = now;
    uint16_t percent = 0;
    if (!battery.readPercentageChecked(percent)) {
      return _batteryCachedPercent;
    }
    _batteryCachedPercent = percent;
    return _batteryCachedPercent;
  }

  // smooth the battery %. Scale before smoothing so the EMA runs on the corrected curve.
  const uint16_t raw = battery.readPercentage();
  const uint16_t corrected = scaleAdcBatteryPercent(raw);
  if (_batteryCachedPercent == 0) {
    _batteryCachedPercent = 10 * corrected;
  } else {
    _batteryCachedPercent = (_batteryCachedPercent * 9 + corrected * 10) / 10;
  }
  // Log only on change: this runs on every status-bar draw, several times per render.
  static int lastLogged = -1;
  if (_batteryCachedPercent / 10 != lastLogged) {
    lastLogged = _batteryCachedPercent / 10;
    LOG_DBG("PWR", "Battery: raw=%u%% corrected=%u%% shown=%d%%", raw, corrected, lastLogged);
  }
  return _batteryCachedPercent / 10;
}

HalPowerManager::Lock::Lock() {
  xSemaphoreTake(powerManager.modeMutex, portMAX_DELAY);
  // Current limitation: only one lock at a time
  if (powerManager.currentLockMode != None) {
    LOG_ERR("PWR", "Lock already held, ignore");
    valid = false;
  } else {
    powerManager.currentLockMode = NormalSpeed;
    valid = true;
  }
  xSemaphoreGive(powerManager.modeMutex);
  if (valid) {
    // Immediately restore normal CPU frequency if currently in low-power mode
    powerManager.setPowerSaving(false);
  }
}

HalPowerManager::Lock::~Lock() {
  xSemaphoreTake(powerManager.modeMutex, portMAX_DELAY);
  if (valid) {
    powerManager.currentLockMode = None;
  }
  xSemaphoreGive(powerManager.modeMutex);
}
