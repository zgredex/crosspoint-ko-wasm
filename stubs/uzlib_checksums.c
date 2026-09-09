// uzlib_checksums.c — provides uzlib_adler32/uzlib_crc32 referenced from
// tinflate.c's uzlib_uncompress_chksum() (unused by our inflate path, but the
// symbols must resolve). Standard public-domain implementations.
#include <stdint.h>
#include <stddef.h>

uint32_t uzlib_adler32(const void* data, unsigned int length, uint32_t prev_sum) {
  const uint8_t* buf = (const uint8_t*)data;
  uint32_t s1 = prev_sum & 0xFFFF;
  uint32_t s2 = (prev_sum >> 16) & 0xFFFF;
  for (unsigned int i = 0; i < length; i++) {
    s1 = (s1 + buf[i]) % 65521;
    s2 = (s2 + s1) % 65521;
  }
  return (s2 << 16) | s1;
}

uint32_t uzlib_crc32(const void* data, unsigned int length, uint32_t crc) {
  static uint32_t table[256];
  static int init = 0;
  if (!init) {
    for (uint32_t i = 0; i < 256; i++) {
      uint32_t c = i;
      for (int k = 0; k < 8; k++) c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
      table[i] = c;
    }
    init = 1;
  }
  const uint8_t* buf = (const uint8_t*)data;
  crc ^= 0xFFFFFFFFu;
  for (unsigned int i = 0; i < length; i++) crc = table[(crc ^ buf[i]) & 0xFF] ^ (crc >> 8);
  return crc ^ 0xFFFFFFFFu;
}
