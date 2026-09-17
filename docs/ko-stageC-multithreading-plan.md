# Stage C — multithreading the export: analysis and plan

No code in this stage. Everything below is anchored on measured numbers from this session; the
estimates are marked and the arithmetic is shown so it can be checked.

## 1. The workflow, as measured

Per-book phases, from the host harness (same engine, same source as the wasm build):

| phase | text book (2,034 pages, 60 spines) | images / PNG book (14 pages, 10 spines) |
|---|---|---|
| load (read + container parse) | 1.8 ms | 0.2-1.1 ms |
| buildSection (parse + paginate) | 120.9 ms | 0.7-1.2 ms |
| **renderPage (glyphs + image decode + quantize)** | **1,353.5 ms (90%)** | **30.8-31.8 ms (95%)** |
| writer (pack to page bytes) | 28.1 ms | 0.2 ms |
| finish (container + TOC) | 45.2 ms | 0.1 ms |
| disk write (195 MB / 1.3 MB) | 68.3 ms | 0.2-0.4 ms |
| **total** | **1,502.4 ms** | 32.2 ms |

Two facts drive the whole design:

1. **90-95% of the time is `renderPage`**, and it is trivially parallel: a page is a pure function
   of (spine content, spec, fonts).
2. The orchestration around it is not: pagination is sequential *within* a spine, and the container
   needs pages in a defined order with a CHAPTER TABLE whose entries are byte offsets.

## 2. What parallelises, and at what grain

**The unit is the spine (chapter).** 60 spines in the text book, 10 in the test books. Inside a
spine the chain is strict - parse → paginate → render pages 1..n → pack - because page breaks
depend on the flowing layout of everything before them. Across spines there is no dependency at
all: a spine's HTML and its images are all it needs.

So: parallelise **across spines, keep the within-spine chain serial in one worker.**

## 3. The serialization point, and how to avoid it

Today there is **one engine instance** whose global state (loaded book, current spine, current
page, plane buffers, writer output) each render call mutates. That is the thing to remove - and the
cheap way to remove it is **one wasm instance per worker**, i.e. separate JS realms, which avoids
in-process global state entirely. No shared-memory engine surgery required.

The trap that follows: if every worker loads the whole book, a 195 MB input becomes 8 x 195 MB.
**Avoided by keeping the book out of the workers**: the coordinator parses the ZIP central
directory once and hands each worker only the byte ranges for its own spine (its HTML plus the
images it references - hundreds of KB, not hundreds of MB). Blob/File slices make this cheap with
no copying. This also means **SharedArrayBuffer and COOP/COEP are not needed** for the core design,
which is worth knowing before touching `_headers` and risking cross-origin loading rules.

## 4. Projected gain, with the tail counted

Amdahl, using the measured text-book split and excluding the serial tail first:

    renderPage 1,353.5 / 8  = 169 ms
    buildSection 120.9 / 8  =  15 ms
    writer        28.1 / 8  =   4 ms
    ---------------------------------
    parallel part           ~ 188 ms

Serial tail as it stands: finish 45.2 + disk 68.3 = **113.5 ms**, plus load ~2 ms.

    optimistic total = 188 + 115 = ~300 ms   ->  5.0x
    with imperfect balance (tail spine)      ->  ~350-400 ms  ->  3.8-4.3x

**Pool size: MAX MINUS ONE, by decision.** Use `navigator.hardwareConcurrency - 1` workers - every
core but one, reserved for the UI thread so preview navigation stays responsive during an export.
Do not be conservative: on the M2 Pro (10-12 cores) that is 9-11 workers, and an idle core is
throughput given away for nothing.

Recomputed at 10 workers rather than 8:

    renderPage 1,353.5 / 10 = 135 ms
    buildSection 120.9 / 10 =  12 ms
    writer        28.1 / 10 =   3 ms
    --------------------------------
    parallel part           ~ 150 ms
    total = 150 + 115       ~ 265 ms   ->  5.7x
    with tail imbalance     ~ 300-320 ms ->  4.7-5.0x

Note the diminishing return and its cause: 8 -> 10 workers moves the total only ~10-15%, because the
115 ms serial tail is already ~40% of the result. **The tail, not the core count, is the binding
constraint** - which is why C3 (streaming container) is worth more than C5 (more workers).
Max-minus-one is the right pool size; it is not where the remaining speed is.

So the honest expectation is **4-5x wall clock, not 6-8x** - and it is the *serial tail*, not the
worker count, that caps it. That is why the plan spends a stage on shrinking the tail rather than
just adding workers.

(M2 Pro: 10-12 cores. Suggest a pool of `min(hardwareConcurrency - 1, spines)` so the UI thread
keeps a core; leaving one free also keeps the preview responsive during an export.)

## 5. Stages, each with its own gate

**C0 - measure the spine distribution.** Instrument the coordinator to log per-spine (pages, ms)
for a real book. Gate: numbers for the current serial baseline, plus the tail spine identified.
Without this, the scheduling choices in C2 are guesses.

**C1 - worker pool, spine dispatch, coordinator assembly.** One worker per core, each with its own
engine instance; coordinator parses the ZIP once, slices per spine, collects packed pages, and
assembles the container in spine order. **Gate: N=1 vs N=4 produce page-level identical output**
(the same comparison used for every change today: 0 differing pages, 0 differing pixels), plus a
wall-clock measurement. Nothing else matters if this gate fails.

**C2 - scheduling.** Largest-spine-first (LPT) instead of arrival order, with stealing so a worker
that finishes early takes the next spine. Gate: identical output; measure the makespan improvement
against C1 to justify it.

**C3 - shrink the serial tail.** Stream the container while pages arrive: the coordinator emits
pages in spine order and records chapter offsets as it goes, instead of buffering everything and
finishing at the end. This also drops peak memory (today the finished container is held whole -
195 MB for the text book). Gate: identical output; tail re-measured.

**C4 - pool sizing.** Default is **max-minus-one** (`hardwareConcurrency - 1`), always. Reduce it
only when either (a) there are fewer spines than workers, so the extra workers would sit idle, or
(b) measurement shows instantiation cost dominating for a tiny book - at ~7 MB module plus fonts per
worker, the pool can cost more to start than it saves. Both reductions must be measured, never
assumed. (Superseded text follows.) For small books the
8 x wasm instantiation (~7 MB module + fonts each) costs more than it saves. Measure the
startup-vs-pages crossover and encode it. Gate: identical output at N = 1/2/4/8; crossover number.

**C5 (only if measured to matter) - SharedArrayBuffer.** If C1's per-worker materialisation shows
up in the profile, revisit SAB with COOP/COEP. Not before: it is a header-level change with its own
risks and the slices design may make it unnecessary.

## 6. Risks to measure, not assume

- **Peak memory** with 4 and 8 instances (each has its own heap and its own materialised slice).
  Note the 2 GB heap is a *limit* per instance, not a reservation - but the emscripten initial
  memory setting should be checked, since 8 x a large initial memory is a real cost.
- **Startup**: 8 instantiations, fonts loaded per worker. Overlapped, but not free.
- **Determinism**: output must not depend on worker count. The assembly is ordered by spine, so it
  should not - but the container carries a millis-derived field that varies run to run, so the gate
  must stay **page-level**, never whole-file.
- **Preview responsiveness**: reserve a worker (or the coordinator's engine) for navigation so an
  export does not freeze the UI.

## 7. What this stage does not fix

The `disk write` / Blob download at the end is I/O and stays serial; for the 195 MB book it is
68 ms and in the browser it is a download. Band removal, in-memory book parsing and
full-resolution decode (E-G) are untouched here.

---

# C 단계 — 내보내기 멀티스레딩: 분석과 계획

이 단계에는 코드가 없습니다. 아래 내용은 모두 이번 세션에서 측정한 수치에 근거하며, 추정치는
그렇게 표시하고 계산 과정을 함께 적었습니다.

## 1. 측정된 워크플로

책 단위 단계 (호스트 하네스 측정, wasm 빌드와 동일한 소스·엔진):

| 단계 | 텍스트 책 (2,034페이지, 60 스파인) | 이미지/PNG 책 (14페이지, 10 스파인) |
|---|---|---|
| load (읽기 + 컨테이너 파싱) | 1.8 ms | 0.2-1.1 ms |
| buildSection (파싱 + 페이지네이션) | 120.9 ms | 0.7-1.2 ms |
| **renderPage (글리프 + 이미지 디코딩 + 양자화)** | **1,353.5 ms (90%)** | **30.8-31.8 ms (95%)** |
| writer (페이지 바이트로 패킹) | 28.1 ms | 0.2 ms |
| finish (컨테이너 + TOC) | 45.2 ms | 0.1 ms |
| disk write (195 MB / 1.3 MB) | 68.3 ms | 0.2-0.4 ms |
| **합계** | **1,502.4 ms** | 32.2 ms |

설계를 결정하는 두 가지 사실:

1. **시간의 90-95%가 `renderPage`**이며 이는 완벽하게 병렬화 가능합니다: 페이지는 (스파인 내용,
   스펙, 폰트)의 순수 함수입니다.
2. 주변 오케스트레이션은 그렇지 않습니다: 페이지네이션은 *스파인 내부에서* 순차적이고,
   컨테이너는 정해진 순서의 페이지와 바이트 오프셋으로 이루어진 **챕터 테이블**을 필요로 합니다.

## 2. 무엇이 병렬화되는가, 어떤 입자로

**단위는 스파인(챕터)입니다.** 텍스트 책에 60개, 테스트 책에 10개. 스파인 내부의 사슬은 엄격합니다
— 파싱 → 페이지네이션 → 1..n 페이지 렌더 → 패킹 — 왜냐하면 페이지 나눔이 앞선 모든 내용의
흐름 레이아웃에 의존하기 때문입니다. 스파인 사이에는 의존성이 전혀 없습니다: 스파인의 HTML과
그 이미지만 있으면 됩니다.

따라서: **스파인을 가로질러 병렬화하고, 스파인 내부 사슬은 한 워커에서 직렬로 유지합니다.**

## 3. 직렬화 지점과 회피 방법

현재 **엔진 인스턴스가 하나**이고, 그 전역 상태(로드된 책, 현재 스파인, 현재 페이지, 플레인
버퍼, 라이터 출력)를 각 렌더 호출이 변경합니다. 이것이 제거 대상이며, 값싼 제거 방법은
**워커당 wasm 인스턴스 하나**입니다. 즉 JS 렐름을 분리하면 프로세스 내 전역 상태 문제를 피할 수
있습니다. 공유 메모리 엔진 수술은 필요하지 않습니다.

뒤따르는 함정: 워커마다 책 전체를 로드하면 195 MB 입력이 8 × 195 MB가 됩니다. **워커에 책을
넣지 않으면 회피됩니다**: 코디네이터가 ZIP 중앙 디렉터리를 한 번 파싱하고 각 워커에 자기
스파인에 해당하는 바이트 범위(HTML + 참조 이미지 — 수백 MB가 아니라 수백 KB)만 넘깁니다.
Blob/File 슬라이스로 복사 없이 저렴하게 처리됩니다. 이는 또한 핵심 설계에 **SharedArrayBuffer와
COOP/COEP가 필요 없다**는 뜻이며, `_headers`를 건드려 교차 출처 로딩 규칙을 위험에 빠뜨리기 전에
알아둘 가치가 있습니다.

## 4. 꼬리까지 포함한 예상 이득

측정된 텍스트 책 분할을 사용한 Amdahl (직렬 꼬리를 먼저 제외):

    renderPage 1,353.5 / 8  = 169 ms
    buildSection 120.9 / 8  =  15 ms
    writer        28.1 / 8  =   4 ms
    ---------------------------------
    병렬 부분               ~ 188 ms

현재 직렬 꼬리: finish 45.2 + disk 68.3 = **113.5 ms**, load 약 2 ms.

    낙관적 합계 = 188 + 115 = ~300 ms   ->  5.0배
    불균형(꼬리 스파인) 포함            ->  ~350-400 ms  ->  3.8-4.3배

따라서 정직한 기대치는 **6-8배가 아니라 4-5배**이며, 이를 제한하는 것은 워커 수가 아니라
**직렬 꼬리**입니다. 그래서 계획에 워커를 늘리는 단계가 아니라 꼬리를 줄이는 단계를 넣었습니다.

(M2 Pro: 10-12코어. UI 스레드가 코어를 유지하도록 `min(hardwareConcurrency - 1, 스파인 수)` 풀을
권장하며, 코어를 하나 남기면 내보내기 중에도 미리보기가 반응성을 유지합니다.)

## 5. 단계와 각 단계의 게이트

**C0 — 스파인 분포 측정.** 코디네이터에 스파인별 (페이지 수, ms) 로그를 넣고 실제 책으로 측정.
게이트: 현재 직렬 기준선 수치 + 꼬리 스파인 식별. 이것 없이는 C2의 스케줄링 선택이 추측입니다.

**C1 — 워커 풀, 스파인 디스패치, 코디네이터 조립.** 코어당 워커 하나, 각자 엔진 인스턴스 보유.
코디네이터가 ZIP을 한 번 파싱하고 스파인별로 슬라이스하며, 패킹된 페이지를 모아 스파인 순서로
컨테이너를 조립. **게이트: N=1과 N=4의 출력이 페이지 단위로 동일**해야 합니다(오늘 모든 변경에
사용한 비교와 동일: 차이 페이지 0, 차이 픽셀 0) + 벽시계 측정. 이 게이트가 실패하면 나머지는
무의미합니다.

**C2 — 스케줄링.** 도착 순서 대신 큰 스파인 우선(LPT), 일찍 끝난 워커가 다음 스파인을 가져가는
stealing. 게이트: 출력 동일 + C1 대비 makespan 개선 측정으로 필요성 입증.

**C3 — 직렬 꼬리 축소.** 페이지가 도착하는 대로 컨테이너를 스트리밍 출력: 코디네이터가 스파인
순서로 페이지를 내보내며 챕터 오프셋을 기록하고, 마지막에 한꺼번에 마무리하지 않습니다. 이는
최대 메모리도 줄입니다(현재는 완성된 컨테이너를 통째로 보유 — 텍스트 책 195 MB).
게이트: 출력 동일 + 꼬리 재측정.

**C4 — 풀 크기 최적화.** 워커 수 = `min(코어-1, 스파인 수)`에 하한 적용: 작은 책에서는
8 × wasm 인스턴스화(~7 MB 모듈 + 폰트) 비용이 절약보다 큽니다. 시작 비용 대 페이지 수 교차점을
측정해 반영. 게이트: N = 1/2/4/8에서 출력 동일 + 교차점 수치.

**C5 (측정상 필요할 때만) — SharedArrayBuffer.** C1의 워커별 자료화 비용이 프로파일에 드러나면
COOP/COEP와 함께 재검토. 그 전에는 하지 않습니다: 자체 위험이 있는 헤더 수준 변경이며 슬라이스
설계로 불필요할 수 있습니다.

## 6. 가정하지 말고 측정할 위험

- **최대 메모리**: 4개, 8개 인스턴스에서(각자 힙과 자료화한 슬라이스 보유). 2 GB 힙은 인스턴스당
  *상한*이지 예약이 아니지만, emscripten 초기 메모리 설정은 확인해야 합니다 — 8 × 큰 초기 메모리는
  실질 비용입니다.
- **시작 비용**: 인스턴스화 8회 + 워커별 폰트 로드. 겹치지만 공짜는 아닙니다.
- **결정성**: 출력이 워커 수에 의존하면 안 됩니다. 조립이 스파인 순서이므로 그럴 리 없지만,
  컨테이너에 실행마다 달라지는 millis 파생 필드가 있으므로 게이트는 **페이지 단위**를 유지해야
  합니다(절대 전체 파일 비교 금지).
- **미리보기 반응성**: 내보내기가 UI를 멈추지 않도록 워커 하나(또는 코디네이터 엔진)를 탐색용으로
  남겨 둡니다.

## 7. 이 단계가 해결하지 않는 것

마지막의 `disk write` / Blob 다운로드는 I/O이며 직렬로 남습니다; 195 MB 책에서 68 ms이고
브라우저에서는 다운로드입니다. 밴드 제거, 메모리 내 책 파싱, 항상 전체 해상도 디코딩(E-G)은
여기서 다루지 않습니다.

---

## 갱신: 풀 크기는 MAX MINUS ONE (2026-09-17 결정)

코어를 하나만 남기고 `navigator.hardwareConcurrency - 1` 워커를 사용합니다. 남긴 한 코어는 UI 스레드용이며,
내보내기 중에도 미리보기 탐색이 반응성을 유지하도록 하는 덱입니다. 보수적일 필요는 없습니다: M2 Pro(10-12코어)에서
9-11 워커이며, 놀리는 코어는 아무것도 얻지 못한 채 처리량을 버리는 것입니다.

8이 아니라 10 워커로 재계산:

    renderPage 1,353.5 / 10 = 135 ms
    buildSection 120.9 / 10 =  12 ms
    writer        28.1 / 10 =   3 ms
    --------------------------------
    병렬 부분               ~ 150 ms
    합계 = 150 + 115        ~ 265 ms   ->  5.7배
    꼬리 불균형 포함        ~ 300-320 ms ->  4.7-5.0배

수익 체감과 원인: 8 -> 10 워커는 총합을 10-15%만 움직입니다. 115 ms 직렬 꼬리가 이미 결과의 약
40%이기 때문입니다. **구속 조건은 코어 수가 아니라 꼬리입니다** — 그래서 C3(컨테이너 스트리밍)이
C5(워커 추가)보다 가치 있습니다. Max-minus-one은 올바른 풀 크기이지만, 남은 속도가 있는
곳은 아닙니다. C4의 기본값도 max-minus-one이며, 스파인 수가 워커보다 적거나 아주 작은 책에서
인스턴시에이션 비용이 지배적임이 측정될 때만 줄이며, 그 축소도 측정으로 정합니다.


---

## Two-phase scheduling: parallel pagination, then one global page queue

**Decision: do not dispatch at spine granularity alone. Split the export into two parallel phases.**

### Phase 1 - paginate every spine (spine granularity)

Each worker takes a whole spine, parses it and lays out its pages.

    120.9 ms / 10 = 12.1 ms

This phase is deliberately spine-granular because pagination is sequential *within* a spine - each
page break depends on how everything before it flowed - so it cannot be split further without
changing the layout. Splitting it is not a scheduling question, it is a correctness one.

### Phase 2 - render from ONE global page queue (page granularity)

Every laid-out page from every spine goes into a single queue; any worker takes the next page.

    1,353.5 ms / 10 = 135.4 ms

A worker asked for page P of spine S builds S itself if it has not already. Parse + paginate is
~0.06 ms/page, so a shared spine costs ~2 ms to re-parse - cheap against the imbalance it removes.

### Why the split matters

Spine-granular dispatch ends the export when the **largest remaining spine** ends, so the tail is
"the biggest chapter" (measured tail spine distribution pending C0). With a page queue the tail is
**one page**: ~0.7 ms for text, ~2.3 ms for an image page. That is the difference between the
~300-320 ms projection and ~150 ms.

### What this requires

- **The coordinator owns** the spine list, the global page queue, the output ordering and the
  container assembly. Workers own only their own engine instance and its state.
- **Ordering must be by index, not arrival.** Each queue item carries (spineIndex, pageIndex) and
  results land in a preallocated per-page slot, so worker completion order cannot reach the output.
  This is what keeps the N=1 vs N=max-minus-one gate meaningful - without it, a page queue would
  make the output order depend on scheduling.
- **Phase 1 output must be shared with phase 2**: the page list per spine. One entry per page, so
  it is small, but it is now needed by every worker rather than just the coordinator.
- **Building a spine is idempotent and per-worker**, so any worker may build any spine it is asked
  to render. No cross-worker state, no locking.
- **Reserve one worker for preview navigation** (the core held back by max-minus-one), so an export
  does not freeze the UI.

### Gates for this change

- page-level identical output at N=1, at N=max-minus-one, and phase 1/2 merged vs split
- per-phase wall clock, proving phase 1 has not grown (it should stay ~12 ms)
- tail measurement: time from "last queue item handed out" to "last result in" - should be about
  one page. This is the direct test of whether the split achieved what it was for.

### Resulting projection

    load                      1.8 ms
    phase 1  paginate        12.1 ms   parallel, spine granularity
    phase 2  render         135.4 ms   parallel, page queue, near-perfect balance
    tables + patch            0.3 ms   (streaming writer, see tail section)
    write                    68.3 ms   overlapped with phase 2
    ------------------------------------
    critical path           ~150 ms    ->  10.0x

**~150 ms is the Amdahl ceiling at 10 workers**, not a waypoint: renderPage is 90% of the work, so
after this the binding constraint is the render phase itself. Further gains must come from making
renderPage cheaper (SIMD on glyph/quantise paths, dither wiring, decode decisions) - not from more
threads, and not from further scheduling.

---

## 2단계 스케줄링: 병렬 페이지네이션 후 하나의 전역 페이지 큐

**결정: 스파인 단위 디스패치만으로는 부족합니다. 내보내기를 두 개의 병렬 단계로 나눕니다.**

### 1단계 — 모든 스파인 페이지네이션 (스파인 단위)

워커가 스파인 하나를 통째로 맡아 파싱하고 페이지를 레이아웃합니다.

    120.9 ms / 10 = 12.1 ms

이 단계를 의도적으로 스파인 단위로 두는 이유는 페이지네이션이 스파인 *내부에서* 순차적이기
때문입니다 — 각 페이지 나눔이 앞선 모든 내용의 흐름에 의존합니다 — 따라서 레이아웃을 바꾸지 않고는
더 나눌 수 없습니다. 이는 스케줄링 문제가 아니라 정확성 문제입니다.

### 2단계 — 하나의 전역 페이지 큐에서 렌더 (페이지 단위)

모든 스파인의 레이아웃된 모든 페이지가 하나의 큐에 들어가고, 아무 워커나 다음 페이지를 가져갑니다.

    1,353.5 ms / 10 = 135.4 ms

스파인 S의 페이지 P를 요청받은 워커는 아직 없다면 S를 직접 빌드합니다. 파싱+페이지네이션은
페이지당 약 0.06 ms이므로 공유 스파인 재파싱 비용은 약 2 ms이며, 제거되는 불균형에 비하면
저렴합니다.

### 이 분할이 중요한 이유

스파인 단위 디스패치는 **남은 가장 큰 스파인**이 끝날 때 내보내기가 끝나므로 꼬리가 '가장 큰
챕터'입니다. 페이지 큐에서는 꼬리가 **페이지 하나**입니다: 텍스트 약 0.7 ms, 이미지 페이지 약
2.3 ms. 이것이 ~300-320 ms 예상과 ~150 ms의 차이입니다.

### 필요한 것

- **코디네이터가 소유**: 스파인 목록, 전역 페이지 큐, 출력 순서, 컨테이너 조립. 워커는 자기 엔진
  인스턴스와 그 상태만 소유합니다.
- **순서는 도착이 아니라 인덱스로.** 각 큐 항목이 (spineIndex, pageIndex)를 지니고 결과가 미리
  할당된 페이지 슬롯에 들어가므로, 워커 완료 순서가 출력에 도달할 수 없습니다. 이것이
  N=1 대 N=max-minus-one 게이트를 유효하게 유지합니다 — 이것이 없으면 페이지 큐는 출력 순서를
  스케줄링에 의존하게 만듭니다.
- **1단계 결과를 2단계와 공유**: 스파인별 페이지 목록. 페이지당 한 항목이라 작지만, 이제
  코디네이터만이 아니라 모든 워커가 필요로 합니다.
- **스파인 빌드는 멱등이며 워커별**: 어떤 워커든 자기가 렌더하라는 스파인을 빌드할 수 있습니다.
  워커 간 상태도, 잠금도 없습니다.
- **미리보기 탐색용으로 워커 하나를 남겨 둡니다**(max-minus-one이 남긴 코어). 내보내기가 UI를
  멈추지 않게 하기 위함입니다.

### 게이트

- N=1, N=max-minus-one, 그리고 1/2단계 병합 대 분할에서 페이지 단위 출력 동일
- 단계별 벽시계 시간: 1단계가 커지지 않았음을 증명(약 12 ms 유지)
- 꼬리 측정: '마지막 큐 항목 배포'부터 '마지막 결과 도착'까지 — 페이지 하나 정도여야 하며,
  이것이 분할의 목적 달성 여부를 직접 검증합니다.

### 결과 예상

    load                      1.8 ms
    1단계  페이지네이션       12.1 ms   병렬, 스파인 단위
    2단계  렌더             135.4 ms   병렬, 페이지 큐, 거의 완벽한 균형
    테이블 + 패치             0.3 ms   (스트리밍 라이터)
    write                    68.3 ms   2단계와 중첩
    ------------------------------------
    임계 경로               ~150 ms    ->  10.0배

**~150 ms는 10워커에서의 Amdahl 상한**이며 경유지가 아닙니다: renderPage가 전체의 90%이므로 이후의
구속 조건은 렌더 단계 자체입니다. 추가 이득은 renderPage를 더 싸게 만드는 데서 나와야 하며
(글리프/양자화 경로의 SIMD, 디더 연결, 디코딩 결정), 스레드를 늘리거나 스케줄링을 더 다듬는
데서 나오지 않습니다.
