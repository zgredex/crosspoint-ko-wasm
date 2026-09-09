#include "I18n.h"

#include <HalStorage.h>
#include <Logging.h>
#include <Serialization.h>

#include <cstddef>
#include <cstring>

#include "I18nStrings.h"

using namespace i18n_strings;

// Settings file path
static constexpr const char* SETTINGS_FILE = "/.crosspoint/language.bin";
static constexpr uint8_t SETTINGS_VERSION = 2;
// v1 stored Language as a raw enum index. Upstream 1.2.0 inserted Belarusian at
// position 11 (alphabetical tie-break with korean.yaml _order="11"), shifting
// Korean from 11 to 12. Users upgrading from 1.1.x would otherwise see Cyrillic
// (Belarusian) instead of Korean. v2 bumps the version and remaps lang==11.
static constexpr uint8_t V1_KOREAN_INDEX = 11;

I18n& I18n::getInstance() {
  static I18n instance;
  static bool loaded = false;
  if (!loaded) {
    loaded = true;
    HalFile file;
    if (Storage.openFileForRead("I18N", SETTINGS_FILE, file)) {
      uint8_t version;
      serialization::readPod(file, version);
      if (version > SETTINGS_VERSION) {
        LOG_DBG("I18N", "Settings version %u newer than supported %u, using default", version, SETTINGS_VERSION);
        file.close();
      } else {
        uint8_t lang;
        serialization::readPod(file, lang);
        file.close();

        // v1 -> v2 migration: Korean shifted from enum index 11 to 12 when upstream
        // 1.2.0 inserted Belarusian. Remap to the current KOREAN index so v1 users
        // don't boot into Cyrillic.
        if (version == 1 && lang == V1_KOREAN_INDEX) {
          lang = static_cast<uint8_t>(Language::KOREAN);
          LOG_DBG("I18N", "Migrated v1 Korean index %u -> %u", V1_KOREAN_INDEX, lang);
        }

        if (lang < static_cast<uint8_t>(Language::_COUNT)) {
          instance._language = static_cast<Language>(lang);
          LOG_DBG("I18N", "Loaded language: %d", static_cast<int>(instance._language));
        }

        // Persist with current version so migration runs only once per device.
        if (version != SETTINGS_VERSION) {
          Storage.mkdir("/.crosspoint");
          HalFile saveFile;
          if (Storage.openFileForWrite("I18N", SETTINGS_FILE, saveFile)) {
            serialization::writePod(saveFile, SETTINGS_VERSION);
            serialization::writePod(saveFile, static_cast<uint8_t>(instance._language));
            saveFile.close();
            LOG_DBG("I18N", "Migrated settings to version %u", SETTINGS_VERSION);
          }
        }
      }
    } else {
      LOG_DBG("I18N", "No settings file, using default");
    }
  }
  return instance;
}

const char* I18n::get(StrId id) const {
  const auto index = static_cast<size_t>(id);
  if (index >= static_cast<size_t>(StrId::_COUNT)) {
    return "???";
  }

  // Use generated helper function - no hardcoded switch needed!
  const LangStrings lang = getLanguageStrings(_language);

  // If bit 15 of the offset is set, apply the offset to the English lookup table
  const uint16_t off = lang.offsets[index];
  if (off & 0x8000) return STRINGS_EN_DATA + (off & 0x7FFF);
  return lang.data + off;
}

void I18n::setLanguage(Language lang) {
  if (lang >= Language::_COUNT) {
    return;
  }
  _language = lang;

  // Persist language selection to SD card
  Storage.mkdir("/.crosspoint");
  HalFile file;
  if (!Storage.openFileForWrite("I18N", SETTINGS_FILE, file)) {
    LOG_ERR("I18N", "Failed to save language setting");
    return;
  }
  serialization::writePod(file, SETTINGS_VERSION);
  serialization::writePod(file, static_cast<uint8_t>(_language));
  file.close();
  LOG_DBG("I18N", "Settings saved: language=%d", static_cast<int>(_language));
}

const char* I18n::getLanguageName(Language lang) const {
  const auto index = static_cast<size_t>(lang);
  if (index >= static_cast<size_t>(Language::_COUNT)) {
    return "???";
  }
  return LANGUAGE_NAMES[index];
}

Language I18n::languageFromCode(const char* code) {
  for (uint8_t i = 0; i < getLanguageCount(); i++) {
    if (strcmp(code, LANGUAGE_CODES[i]) == 0) return static_cast<Language>(i);
  }
  return Language::EN;
}

// Generate character set for a specific language
const char* I18n::getCharacterSet(Language lang) {
  const auto langIndex = static_cast<size_t>(lang);
  if (langIndex >= static_cast<size_t>(Language::_COUNT)) {
    lang = Language::EN;  // Fallback to first language
  }

  return CHARACTER_SETS[static_cast<size_t>(lang)];
}
