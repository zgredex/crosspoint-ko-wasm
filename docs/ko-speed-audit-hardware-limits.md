# Speed from removing the original hardware's limits — audit

Every item below was found by grepping the tree, not recalled. Tags: **[V]** verified by reading
the code or by measurement in this session; **[D]** derived (reasoning or earlier measurement),
still to be measured on the target machine. Nothing here is estimated as if measured.

## What the device actually still imposes

### 1. [V] `delay()` is a no-op and `millis()` returns 0 in wasm

`stubs/Arduino.h`:

    inline uint32_t millis() { return 0; }
    inline void delay(uint32_t) {}

So the two `delay(50)` retry pauses (`ChapterHtmlSlimParser.cpp:644`, `Section.cpp:306`) cost
nothing, and anything keyed on elapsed time is inert. Consequence worth noting: the container's
millis field still varies run to run (`11f1 ab6a` -> `44f3 ab6a`), so that value does **not** come
from `millis()` — it comes from another clock. Chasing determinism means chasing that clock, not
this stub.

### 2. [V] Size caps that REFUSE work rather than clamp it

    PngToBmpConverter.cpp:451    MAX_IMAGE_WIDTH  = 2048   MAX_IMAGE_HEIGHT  = 3072
    JpegToBmpConverter.cpp:546   same
    Bitmap.cpp:125               same -> BmpReaderError::ImageTooLarge

    if (width > MAX_IMAGE_WIDTH || ...) { LOG_ERR("Image too large"); return false; }

These are cover-path limits: a cover wider than 2048 px fails to render entirely. The PNG
framebuffer path had the same shape of bug at 1281 px (`PNG_MAX_BUFFERED_PIXELS/2`), which is why
`wide_scaling_test.png` (1807x736) never appeared in output. There is no device buffer to defend
any more.

### 3. [V] Heap gates and allocation plumbing

Counts in `src` + vendored engine: `getMaxAllocHeap` 9, `MIN_FREE_HEAP_FOR_PNG` 5,
`MIN_FREE_HEAP_FOR_CSS` 4, `std::nothrow` 9, `makeUniqueNoThrow` 9.

These exist because a fragmented ~48 KB ESP32 heap could not fit a decoder. Their real cost is not
the check but the **refusals** (item 2 is one of them), and the constraint they impose on buffer
strategy: with a 2 GB heap there is no reason to decode images in bands.

### 4. [V] The band/chunk machinery is still the architecture

46 `band`, 43 `chunk`, 8 `CHUNK`, 6 `BandBlock`, 3 `bandRows` references. The renderer still draws
in bands, which is why the image callback owned monotonic `lastDstY` state and why a mid-decode
hiccup left a white hole (five passes to diagnose). Removing bands deletes that bug class and the
cache layers that existed to hide band costs.

### 5. [V] Build flags: the multipliers are unclaimed

`-O3` is on. `-msimd128`, `-flto` and `-pthread` are absent.

### 6. [V] The device ABI is still the interface

`HalStorage` 14 references plus the `stubs/` shim layer (`Arduino.h`, `freertos`, `HalStorage`,
`esp_shim`). The book is read through that abstraction, so the input EPUB is copied into the wasm
filesystem rather than parsed in place.

### 7. [V] Image decoders still scale down on purpose

`chooseScaleDenom` DCT scaling plus the `smoothUpscale` band hack exist to fit images into device
RAM. The progressive-JPEG blur (DC-only 1/8, measured 21.76 grey / RMSE 51.91) was one symptom and
is already fixed for JPEG; the machinery that caused it remains.

## Ranked by value per unit of effort

**A. `-msimd128` + `-flto` [D] — first because it is a build flag, not a refactor.**
2-4x on the vectorisable inner loops: the engine compose bit expansion (already 1.419 ms/page
[V]), dither/quantise, writer packing, grey conversion. Measure with the existing oracle gates.

**B. Remove the refusal caps and heap gates [V finding, D gain].** Correctness first: covers
above 2048 px render again. Then it unblocks whole-image buffers, which is what makes C and D
possible.

**C. Multi-worker threads [D] — the biggest single multiplier, 6-8x on wall clock** (earlier
estimate), bounded by cores. Requires SharedArrayBuffer, so `Cross-Origin-Opener-Policy` +
`Cross-Origin-Embedder-Policy` headers — and `web/_headers` already exists, so it is a header edit
plus a per-page-range worker split. A book is embarrassingly parallel: each worker renders a spine
independently.

**D. RAM caches, now free [D].** The 2 GB heap is already configured. A packed page is 96 KB, so a
2,000-page book is ~192 MB: entirely viable, and it makes preview navigation O(1) instead of
re-render. Same argument for decode-once image buffers, replacing the device-era PXC cache that was
removed because its storage was never activated.

**E. Band/chunk removal [V finding].** Modest throughput gain for text — measured 2.65 ms/page
end to end [V], so text is not the problem — but it removes a bug class, deletes cache layers, and
simplifies the image path. Do it for structure, not speed.

**F. Direct memory book parsing [D].** Skip the filesystem copy by handing the book's ArrayBuffer
to wasm and parsing in place. Removes a full-size copy of the input and per-read overhead.

**G. Full-resolution image decode always [D].** Drop DCT scaling: quality first, and better
downsampling because the resample starts from full resolution instead of a DC-only image.

## Honest bounds on the whole exercise

Text rendering is **already 2.65 ms/page end to end [V]**, so most remaining upside is in image
pages, in preview latency, and in multi-core parallelism — not in making ordinary text pages
faster than they already are. The preview compose is the worked example of this whole method:
measured at 3.71 ms (original JS), 2.88 ms (optimised JS), 1.419 ms (engine, grouped), with the
8-bit grouping contributing 1.43x *within one language* [V]. Each item above should be attacked the
same way — freeze an oracle, change one thing, byte-compare, then measure — because that is what has
actually produced results here.

---

# 원래 하드웨어의 제약을 제거해 얻을 수 있는 속도 — 감사

아래 항목은 모두 기억이 아니라 트리 grep으로 찾은 것입니다. 표기: **[V]** 코드 확인 또는 이번
세션 측정으로 검증, **[D]** 파생(추론 또는 이전 측정)이며 대상 환경에서 아직 측정 필요.
측정한 것처럼 추정한 항목은 없습니다.

## 기기가 실제로 아직 부과하는 것

### 1. [V] wasm에서 `delay()`는 무동작이고 `millis()`는 0을 반환

`stubs/Arduino.h`:

    inline uint32_t millis() { return 0; }
    inline void delay(uint32_t) {}

따라서 두 곳의 `delay(50)` 재시도 대기(`ChapterHtmlSlimParser.cpp:644`, `Section.cpp:306`)는
비용이 없고, 경과 시간에 의존하는 로직은 무력합니다. 주목할 결과: 컨테이너의 millis 필드는
실행마다 여전히 달라지므로(`11f1 ab6a` -> `44f3 ab6a`) 그 값은 `millis()`에서 오지 않습니다 —
다른 시계에서 옵니다. 결정성을 추구한다면 이 스텁이 아니라 그 시계를 봐야 합니다.

### 2. [V] 클램프가 아니라 작업을 거부하는 크기 상한

    PngToBmpConverter.cpp:451    MAX_IMAGE_WIDTH  = 2048   MAX_IMAGE_HEIGHT  = 3072
    JpegToBmpConverter.cpp:546   동일
    Bitmap.cpp:125               동일 -> BmpReaderError::ImageTooLarge

    if (width > MAX_IMAGE_WIDTH || ...) { LOG_ERR("Image too large"); return false; }

표지 경로의 제한입니다: 폭 2048px를 넘는 표지는 아예 렌더링되지 않습니다. PNG 프레임버퍼
경로에도 1281px(`PNG_MAX_BUFFERED_PIXELS/2`)에서 같은 형태의 버그가 있었고, 그래서
`wide_scaling_test.png`(1807x736)가 출력에 나타나지 않았습니다. 이제 방어할 기기 버퍼가 없습니다.

### 3. [V] 힙 게이트와 할당 배관

`src` + 벤더 엔진 기준 개수: `getMaxAllocHeap` 9, `MIN_FREE_HEAP_FOR_PNG` 5,
`MIN_FREE_HEAP_FOR_CSS` 4, `std::nothrow` 9, `makeUniqueNoThrow` 9.

약 48KB의 단편화된 ESP32 힙에 디코더가 들어가지 못했기 때문에 존재합니다. 실제 비용은 검사가
아니라 **거부**(항목 2가 그중 하나)이며, 버퍼 전략에 부과하는 제약입니다: 2GB 힙에서는 이미지를
밴드 단위로 디코딩할 이유가 없습니다.

### 4. [V] 밴드/청크 기계장치가 여전히 아키텍처

`band` 46, `chunk` 43, `CHUNK` 8, `BandBlock` 6, `bandRows` 3개 참조. 렌더러가 여전히 밴드
단위로 그리며, 그래서 이미지 콜백이 단조 증가 `lastDstY` 상태를 가졌고 디코딩 도중 문제가 생기면
흰 구멍이 남았습니다(진단에 다섯 번의 패스). 밴드를 제거하면 그 버그 부류와, 밴드 비용을 숨기려
존재했던 캐시 계층이 함께 사라집니다.

### 5. [V] 빌드 플래그: 배수 장치가 아직 미사용

`-O3`는 켜져 있습니다. `-msimd128`, `-flto`, `-pthread`는 없습니다.

### 6. [V] 기기 ABI가 여전히 인터페이스

`HalStorage` 14개 참조와 `stubs/` 심 계층(`Arduino.h`, `freertos`, `HalStorage`, `esp_shim`).
책을 이 추상화를 통해 읽으므로 입력 EPUB이 제자리 파싱되지 않고 wasm 파일시스템으로 복사됩니다.

### 7. [V] 이미지 디코더가 여전히 의도적으로 축소

`chooseScaleDenom` DCT 축소와 `smoothUpscale` 밴드 핵은 이미지를 기기 RAM에 맞추려 존재합니다.
progressive JPEG 흐림(DC-only 1/8, 측정 21.76 grey / RMSE 51.91)이 그 증상 중 하나였고 JPEG는 이미
수정되었지만, 원인 기계장치는 남아 있습니다.

## 노력 대비 가치 순위

**A. `-msimd128` + `-flto` [D] — 리팩터가 아니라 빌드 플래그이므로 최우선.**
벡터화 가능한 내부 루프에서 2-4배: 엔진 compose 비트 확장(이미 1.419 ms/페이지 [V]), 디더/양자화,
라이터 패킹, 그레이 변환. 기존 오라클 게이트로 측정하십시오.

**B. 거부 상한과 힙 게이트 제거 [V 근거, D 이득].** 우선 정확성: 2048px 초과 표지가 다시
렌더링됩니다. 그다음 전체 이미지 버퍼를 가능하게 하고, 그것이 C와 D의 전제입니다.

**C. 다중 워커 스레드 [D] — 최대 배수, 벽시계 기준 6-8배**(이전 추정), 코어 수에 상한.
SharedArrayBuffer가 필요하므로 `Cross-Origin-Opener-Policy` + `Cross-Origin-Embedder-Policy`
헤더가 필요하고, `web/_headers`가 이미 있으므로 헤더 수정과 페이지 범위별 워커 분할이면 됩니다.
책은 embarrassingly parallel합니다: 워커마다 스파인 하나를 독립적으로 렌더링합니다.

**D. RAM 캐시, 이제 공짜 [D].** 2GB 힙은 이미 설정되어 있습니다. 패킹된 페이지는 96KB이므로
2,000페이지 책은 약 192MB로 충분히 가능하며, 미리보기 탐색을 재렌더 대신 O(1)로 만듭니다. 기기
시대 PXC 캐시(저장소가 활성화되지 않아 제거됨)를 대체하는 이미지 1회 디코딩 버퍼도 같은 논리입니다.

**E. 밴드/청크 제거 [V 근거].** 텍스트 처리량 이득은 작습니다 — 종단 간 2.65 ms/페이지 [V]이므로
텍스트가 문제가 아닙니다 — 그러나 버그 부류를 없애고 캐시 계층을 지우며 이미지 경로를 단순화합니다.
속도가 아니라 구조를 위해 하십시오.

**F. 책을 메모리에서 직접 파싱 [D].** 파일시스템 복사를 건너뛰고 책의 ArrayBuffer를 wasm에 넘겨
제자리에서 파싱합니다. 입력 전체 크기의 복사와 읽기별 오버헤드를 제거합니다.

**G. 항상 전체 해상도 이미지 디코딩 [D].** DCT 축소를 제거합니다: 품질이 우선이며, DC-only
이미지가 아니라 전체 해상도에서 리샘플을 시작하므로 다운샘플링도 개선됩니다.

## 전체 작업에 대한 정직한 한계

텍스트 렌더링은 **이미 종단 간 2.65 ms/페이지 [V]**이므로, 남은 여지의 대부분은 일반 텍스트
페이지를 더 빠르게 하는 데가 아니라 이미지 페이지, 미리보기 지연, 멀티코어 병렬성에 있습니다.
미리보기 compose가 이 방법의 실제 예입니다: 3.71ms(원래 JS), 2.88ms(최적화 JS), 1.419ms(엔진,
그룹화)로 측정되었고, 8비트 그룹화가 *같은 언어 안에서* 1.43배를 기여했습니다 [V]. 위 항목도
같은 방식으로 공략하십시오 — 오라클을 동결하고, 하나만 바꾸고, 바이트 비교하고, 그다음 측정.
여기서 실제로 성과를 낸 방법은 그것뿐입니다.
