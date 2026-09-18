// The 1-bit writer's ink densities must follow the quantization model too: they ARE the
// thresholds that turn a level into black/white coverage.
#include <cstdio>
#include "../../src/xtch_writer.h"
// build: clang++ -std=c++17 -O2 -I vendor-lib/Epub/Epub/converters \
//          scripts/verify/mono_model_parity.cpp -o /tmp/ab/mono_parity && /tmp/ab/mono_parity
int main() {
  const uint8_t want[4] = {0, static_cast<uint8_t>(255 - kProfileNominal.ditherLevels[1]),
                           static_cast<uint8_t>(255 - kProfileNominal.ditherLevels[2]), 255};
  int fails = 0;
  for (int i = 0; i < 4; ++i)
    if (kMonoInkDensity[i] != want[i]) {
      printf("FAIL kMonoInkDensity[%d] = %u, want %u\n", i, kMonoInkDensity[i], want[i]); ++fails;
    }
  printf(fails ? "FAILED\n" : "PASS — 1-bit ink densities = {0, 170, 85, 255} = 255 - nominal levels\n");
  return fails;
}
