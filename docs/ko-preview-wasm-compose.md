# Preview compose in WASM — implementation spec

> Current profile/orientation note: this document records the original X4 portrait fast-path work.
> The shipped API is now `ko_compose_rgba(int mono)`: it reads the engine-owned page planes and
> returns upright logical RGBA for X4 (480×800 or 800×480) and X3 (528×792 or 792×528).
> Profile/orientation preview/file equivalence is enforced by
> `scripts/verify/orientation_preview_vs_file.js`; see `docs/ko-landscape-modes.md`.

Target: **zero per-pixel JS work** in the preview. The worker stops unpacking planes and
only wraps engine memory in an `ImageData` and calls `putImageData` once.

## Why this is the right move (measured)

The JS compose was optimised first (commit `b3b2184`): loop-invariant offsets, one 32-bit
RGBA store per pixel, LUT for the 4-level decision.

| path | before | after | speedup |
|---|---|---|---|
| `composeMono` (1-bit, 1 plane) | 2.02 ms/page | 0.385 ms/page | 5.25x |
| `composePage` (2-bit, 3 planes) | 3.71 ms/page | 2.88 ms/page | **1.29x only** |

The 1.29x is the finding that matters: the 2-bit path is **memory-bound, not
arithmetic-bound**. Three planes, 384,000 reads each, gathered at **100-byte stride**, so
each access pulls a whole cache line to use one bit. No further JS tuning can fix that.

## What already exists (recon)

- Exports live in `src/wasm_api.cpp`, inside `extern "C"` (lines 67-538),
  macro `KO_EXPORT` = `EMSCRIPTEN_KEEPALIVE`.
- The worker already calls `_ko_plane_ptr` / `_ko_plane_size` (the three planes) **and**
  `_ko_xtch_ptr` / `_ko_xtch_size` (the packed page = the artifact).
  So no new plumbing is required — only a compose routine and a buffer accessor.

## Implementation

### 1. `src/wasm_api.cpp` (inside `extern "C"`, both `KO_EXPORT`)

    uint8_t* ko_rgba_ptr();                        // selected profile's logical RGBA buffer
    int ko_compose_rgba(const uint8_t* bw,         // fills that buffer
                        const uint8_t* lsb,
                        const uint8_t* msb,
                        int mono);                 // mono: 1-bit, BW plane only

Plane layout: `100` bytes per physical row x `480` rows = 48,000 bytes per plane.
Mapping: `logical(x,y) <- physical(phyX = y, phyY = 479 - x)`;
`idx = phyY * 100 + (phyX >> 3)`, bit = `7 - (phyX & 7)`.

Value contract (byte-identical to the verified JS):
`v = !ink ? 0 : lsb ? 1 : msb ? 2 : 3` where `ink = bit == 0`;
`GRAY32 = {0xFFFFFFFF, 0xFF808080, 0xFFCDCDCD, 0xFF000000}` (little-endian RGBA);
mono: `MONO32 = {0xFF000000, 0xFFFFFFFF}` indexed by the plane bit (0 = black).

### 2. The 8-bit grouping (this is the actual speedup)

For a fixed `byteCol`, eight **consecutive** `y` values occupy **eight different bit
positions of the same byte**. So process `y` in groups of 8:

- one strided byte fetch serves 8 logical rows;
- strided reads drop **8x** (3 x 384,000 reads -> 3 x 48,000);
- each of the 8 output rows is still written sequentially, so the writes stay dense.

That directly targets the measured bottleneck instead of merely moving it into C++.
`-msimd128` can vectorise the bit expansion afterwards; a full SIMD bit-transpose would
make it sequential end to end.

### 3. Worker (`web/ko.worker.js`)

    const ptr = api._ko_rgba_ptr();
    api._ko_compose_rgba(bwPtr, lsbPtr, msbPtr, wantMono ? 1 : 0);
    const img = new ImageData(new Uint8ClampedArray(api.HEAPU8.buffer, ptr, 1536000), 480, 800);

then one `putImageData`. Both JS compose functions, both LUTs, and the `bitAt` remnant are
deleted. The buffer is engine-owned and reused, so there is no per-frame allocation; the
only copy left is the browser's own upload in `putImageData`.

Preview-equals-artifact: the planes the preview composes from are the same planes
`addGrayPage`/`addMonoPage` pack into the file, so the preview is the artifact by
construction rather than by two implementations agreeing.

## Verification (the bar: no half-measures)

1. **Oracle.** The JS implementation is now proven byte-identical to its predecessor
   (8/8 comparisons, 1,536,000 bytes each) - so it is frozen as the reference.
2. **Cross-implementation gate.** Dump one real page's three planes from
   `_ko_plane_ptr`, then compute RGBA (a) in C++ via a native test built from the same
   source file, and (b) in node from the frozen JS. Byte-compare; must be identical
   (1,536,000 bytes).
3. **Native microbench** of the C++ routine (pages/sec), for a measured number rather
   than an estimate.
4. **Browser measurement**: `performance.now()` around compose + `putImageData` in the
   worker, reported through the existing profile output.

Only after 2 passes does the JS compose get deleted.

## Note

`web/app.js` has the same per-pixel pattern around lines 209-216 (2-bpp loop with
`bytesPerRow` / `rowPad`, cover path). Same treatment applies afterwards.

---

# 미리보기 compose를 WASM으로 이전 — 구현 명세

목표: 미리보기에서 **JS 픽셀 단위 작업을 0으로** 만든다. 워커는 더 이상 plane을 풀지
않고, 엔진 메모리를 `ImageData`로 감싸 `putImageData`를 한 번 호출할 뿐이다.

## 왜 이것이 옳은가 (측정 근거)

먼저 JS compose를 최적화했다(커밋 `b3b2184`): 루프 불변 오프셋, 픽셀당 32비트 RGBA 저장
1회, 4레벨 판정 LUT.

| 경로 | 이전 | 이후 | 배속 |
|---|---|---|---|
| `composeMono` (1비트, plane 1개) | 2.02 ms/페이지 | 0.385 ms/페이지 | 5.25x |
| `composePage` (2비트, plane 3개) | 3.71 ms/페이지 | 2.88 ms/페이지 | **1.29x뿐** |

중요한 것은 1.29x라는 사실이다: 2비트 경로는 **연산이 아니라 메모리에 병목**이다. plane
3개, 각 384,000회 읽기, **스트라이드 100바이트** 수집이므로 1비트를 쓰기 위해 캐시 라인
전체를 가져온다. JS 튜닝으로는 더 이상 해결되지 않는다.

## 이미 존재하는 것 (사전 조사)

- export는 `src/wasm_api.cpp`의 `extern "C"`(67–538행) 안에 있고, 매크로 `KO_EXPORT`
  = `EMSCRIPTEN_KEEPALIVE`이다.
- 워커는 이미 `_ko_plane_ptr` / `_ko_plane_size`(plane 3개)와 `_ko_xtch_ptr` /
  `_ko_xtch_size`(패킹된 페이지 = 아티팩트)를 호출한다.
  따라서 새 배관은 필요 없고, compose 루틴과 버퍼 접근자만 추가하면 된다.

## 구현

### 1. `src/wasm_api.cpp` (`extern "C"` 안, 둘 다 `KO_EXPORT`)

    uint8_t* ko_rgba_ptr();                        // 선택한 기기 프로파일의 논리 RGBA 버퍼
    int ko_compose_rgba(const uint8_t* bw,         // 그 버퍼를 채운다
                        const uint8_t* lsb,
                        const uint8_t* msb,
                        int mono);                 // mono: 1비트, BW plane만

plane 배치: 물리적 행당 `100`바이트 x `480`행 = plane당 48,000바이트.
매핑: `logical(x,y) <- physical(phyX = y, phyY = 479 - x)`;
`idx = phyY * 100 + (phyX >> 3)`, 비트 = `7 - (phyX & 7)`.

값 규약 (검증된 JS와 바이트 단위로 동일):
`v = !ink ? 0 : lsb ? 1 : msb ? 2 : 3`, `ink = bit == 0`;
`GRAY32 = {0xFFFFFFFF, 0xFF808080, 0xFFCDCDCD, 0xFF000000}` (리틀엔디안 RGBA);
mono: `MONO32 = {0xFF000000, 0xFFFFFFFF}`를 plane 비트로 인덱싱 (0 = 검정).

### 2. 8비트 묶기 (실제 속도 향상 지점)

고정된 `byteCol`에 대해, **연속된** `y` 8개는 **같은 바이트의 서로 다른 8개 비트**에
위치한다. 따라서 `y`를 8개씩 묶어 처리한다:

- 스트라이드 바이트 읽기 1회가 논리 행 8개를 담당한다;
- 스트라이드 읽기가 **8배 감소**한다 (3 x 384,000회 -> 3 x 48,000회);
- 8개 출력 행은 각각 여전히 순차적으로 쓰이므로 쓰기는 밀집 상태를 유지한다.

이는 병목을 C++로 옮기는 데 그치지 않고 측정된 병목 자체를 겨냥한다. 이후 `-msimd128`로
비트 확장을 벡터화할 수 있고, 완전한 SIMD 비트 전치를 하면 처음부터 끝까지 순차 접근이
된다.

### 3. 워커 (`web/ko.worker.js`)

    const ptr = api._ko_rgba_ptr();
    api._ko_compose_rgba(bwPtr, lsbPtr, msbPtr, wantMono ? 1 : 0);
    const img = new ImageData(new Uint8ClampedArray(api.HEAPU8.buffer, ptr, 1536000), 480, 800);

이후 `putImageData` 1회. JS compose 함수 2개, LUT 2개, `bitAt` 잔재를 모두 삭제한다.
버퍼는 엔진 소유로 재사용되므로 프레임당 할당이 없고, 남는 복사는 `putImageData`의
브라우저 자체 업로드뿐이다.

미리보기 = 아티팩트: 미리보기가 compose하는 plane은 `addGrayPage`/`addMonoPage`가 파일에
패킹하는 바로 그 plane이므로, 두 구현이 일치해서가 아니라 **구성상** 미리보기가
아티팩트가 된다.

## 검증 (기준: 어설픈 타협 없음)

1. **오라클.** JS 구현은 이제 이전 구현과 바이트 단위 동일이 증명되었으므로(8/8 비교,
   각 1,536,000바이트) 기준 구현으로 동결한다.
2. **교차 구현 게이트.** 실제 페이지 하나의 plane 3개를 `_ko_plane_ptr`에서 덤프한 뒤,
   (a) 동일 소스 파일로 빌드한 네이티브 테스트로 C++에서, (b) node에서 동결된 JS로
   RGBA를 계산한다. 바이트 비교 결과가 동일해야 한다(1,536,000바이트).
3. **네이티브 마이크로벤치**: 추정이 아니라 측정값으로 C++ 루틴의 처리량(페이지/초)을
   기록한다.
4. **브라우저 측정**: 워커에서 compose + `putImageData` 구간을 `performance.now()`로
   재고 기존 프로파일 출력으로 보고한다.

2번이 통과한 뒤에만 JS compose를 삭제한다.

## 참고

`web/app.js`의 209–216행 부근에 동일한 픽셀 단위 패턴이 있다(`bytesPerRow` / `rowPad`를
다루는 2bpp 루프, 표지 경로). 같은 처리를 이어서 적용한다.

---

## Stage 1 DONE - algorithm ported, verified and measured (2026-09-17)

Standalone source:  (the same function body moves
into `src/wasm_api.cpp` in stage 2). It is native-testable precisely so the algorithm is
proven before it touches the engine.

### Verification - cross-implementation byte gate

The frozen JS compose (itself proven byte-identical to its predecessor) is the oracle. Both
implementations ran on identical dumped planes (seed 42, 3 x 48,000 bytes):

    cmp rgba_js_2bit.bin  rgba_cpp_2bit.bin  -> IDENTICAL (1,536,000 bytes)
    cmp rgba_js_mono.bin  rgba_cpp_mono.bin  -> IDENTICAL (1,536,000 bytes)

### Measured

| implementation | 2-bit | 1-bit |
|---|---|---|
| original JS (per-pixel helper) | 3.71 ms/page | 2.02 ms/page |
| optimised JS (LUT + u32 stores) | 2.88 ms/page | 0.385 ms/page |
| **C++ grouped (stage 1)** | **1.419 ms/page** | **0.088 ms/page** |
| C++ naive, same file | 2.033 ms/page | - |

- vs optimised JS: **2.03x** (2-bit), **4.4x** (1-bit).
- vs the original JS: **2.6x** and **23x**.
- Grouped beats naive **within C++ by 1.43x**, which confirms the diagnosis: the win comes
  from the 8-bit grouping (one strided fetch per eight logical rows), not from the language.

### Stage 2 (next)

Add `ko_rgba_ptr()` + `ko_compose_rgba()` to `src/wasm_api.cpp` (inside `extern \C\`,
`KO_EXPORT`), point the worker at them with the zero-copy `ImageData` view, delete both JS
compose functions and their LUTs, rebuild wasm, and re-run the gate against the same oracle.
Browser timing via `performance.now()` around compose + `putImageData` still outstanding.

---

## Stage 2 gate CLOSED on real engine page data (2026-09-17)

The gate did not need a browser. The engine's own planes are recoverable from a page the
engine produced: invert the writer's packing (v = 0 if !ink, 1 if lsb, 2 if msb, else 3)
out of the XTCH page bytes and rebuild the three 48,000-byte planes. Page 7 of png_3.xtch
was used, whose four-level histogram proves real content across all shades:

    v=0 white 302,274 | v=1 dark grey 6,503 | v=2 light grey 49,597 | v=3 black 25,626

Results:

    cmp rgba_cpp_real.bin rgba_js_real.bin    -> IDENTICAL (1,536,000 bytes)
    cmp rgba_cpp_real.bin rgba_jsold_real.bin -> IDENTICAL (1,536,000 bytes)

* C++ engine compose == frozen JS oracle, on real page content.
* C++ engine compose == ORIGINAL JS as well, which independently re-verifies the JS
  optimisation (b3b2184) on real content rather than only on synthetic planes.

The only link left to inspection rather than bytes is the wiring itself: ko_compose_rgba
reads ko_plane_ptr(0/1/2), the same accessors the export path already uses, each returning
48,000 bytes per the layout above.

## Browser timing - snippet, not yet run

The compose is measured (1.419 ms/page 2-bit, 0.088 ms/page 1-bit, native, 300 iters). The
canvas upload cannot be timed outside a browser, so it is NOT estimated here. Drop this into
the worker around the compose + blit and report the numbers:

    const t0 = performance.now();
    api._ko_compose_rgba(mono ? 1 : 0);
    const t1 = performance.now();
    const img = new ImageData(new Uint8ClampedArray(api.HEAPU8.buffer, api._ko_rgba_ptr(), 480 * 800 * 4), 480, 800);
    ctx.putImageData(img, 0, 0);
    const t2 = performance.now();
    console.log('compose', (t1 - t0).toFixed(2), 'ms  putImageData', (t2 - t1).toFixed(2), 'ms');
