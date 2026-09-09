// Logging.h — host/WASM stub: replaces the Arduino/FreeInk logging header.
// Same macro contract (LOG_ERR/LOG_INF/LOG_DBG(origin, fmt, ...)) writing to stderr.
#pragma once

#include <cstdarg>
#include <cstdio>
#include <cstring>

#ifndef LOG_LEVEL
#define LOG_LEVEL 1
#endif

#ifdef ENABLE_SERIAL_LOG
#undef ENABLE_SERIAL_LOG
#endif
#define ENABLE_SERIAL_LOG 1

inline void logPrintf(const char* level, const char* origin, const char* format, ...) {
  fprintf(stderr, "[%s][%s] ", level, origin);
  va_list args;
  va_start(args, format);
  vfprintf(stderr, format, args);
  va_end(args);
}

#if LOG_LEVEL >= 0
#define LOG_ERR(origin, format, ...) logPrintf("ERR", origin, format "\n", ##__VA_ARGS__)
#else
#define LOG_ERR(origin, format, ...) \
  do {                               \
  } while (0)
#endif

#if LOG_LEVEL >= 1
#define LOG_INF(origin, format, ...) logPrintf("INF", origin, format "\n", ##__VA_ARGS__)
#else
#define LOG_INF(origin, format, ...) \
  do {                               \
  } while (0)
#endif

#if LOG_LEVEL >= 2
#define LOG_DBG(origin, format, ...) logPrintf("DBG", origin, format "\n", ##__VA_ARGS__)
#else
#define LOG_DBG(origin, format, ...) \
  do {                               \
  } while (0)
#endif
