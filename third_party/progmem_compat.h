// progmem_compat.h — host/WASM: memcpy_P (AVR progmem) is plain memcpy.
#pragma once
#include <cstring>
#ifndef memcpy_P
#define memcpy_P memcpy
#endif
#ifndef pgm_read_byte
#define pgm_read_byte(p) (*(const unsigned char*)(p))
#endif
