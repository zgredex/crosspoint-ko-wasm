# HANDOFF — ko-wasm (Xteink X4 EPUB → XTC/XTCH converter)

**Date:** 2026-09-18 · **Repo:** https://github.com/zgredex/crosspoint-ko-wasm (public, MIT)
**Working dir:** `/Users/patryk/krxtc/ko-wasm` · **Branch:** `main`

---

## 0. TL;DR for the next session

- **Live:** https://crosspoint-ko-wasm.pages.dev — deployment `38f9a63f`, wasm sha256 `eaac8614…`,
  verified equal to `dist/`. (Deploy is now **behind** the local `dist/`: the default face and the
  reference geometry changed after that deployment.)
- **Tree is clean and green.** Host and wasm both build; every gate passes.
- **The oracle is enforced, not described.** `crosspoint-reader-ko @ release/korean
  84a39194dfce1ebd772ac9163df0a59daa0d72dc` is the pinned reference; the gate **compiles it and runs
  it** on the same books. Conformance is three layers: exact layout, perceptual raster, and
  plane bytes as a diagnostic only. See `docs/ko-oracle-conformance.md`.
- **Do NOT externalize KoPub.** The 48% figure that justified it assumed RIDIBatang was the default
  face. KoPub *is* the default (`getReaderFontId()`), so the split saves 3.4% for an extra request on
  the critical path. Measured: `docs/ko-font-payload-measurement.md`. **RIDIBatang is the face worth
  externalizing** (268,627 brotli bytes off every default load, asset 236,244, and its EPD2 path is
  now proven end-to-end) — but only after a *navigation start → first readable page* benchmark.
  Nothing is deployed that would need reverting: the shipped worker never fetches a font.
- **Parked:** ED dither wiring, cover-path verification, force-push, SKILL.md size fix, README
  session note, rotation 4-mode. Single-giant-XHTML warm preflight still has no stress fixture;
  `HalStorage::mountBlob()`'s extra copy and the export high-water mark are still open.

---

## 1. Done and verified this session

| work | commit | evidence |
|---|---|---|
| libjpeg-turbo replaces JPEGDEC (framebuffer path) | `45aac93` | byte-identical books; progressive fixed (was DC-only 1/8) |
| dither rewrite + default profile = ko fork `45/70/140` | `7fb9b3c` | tone error 1.372 vs 11.6 for `master` profile, on 3 independent sources |
| PXC pixel cache removed (device-only dead code) | `a3559d2` | 14 + 2,034 pages, **0 differing pixels** |
| JPEGDEC removed entirely (cover path → libjpeg) | `ca0d2c7` | 2,048 pages, 0 differing pages; 0 JPEGDEC refs in build inputs |
| 1-bit blue-noise text dither, **default ON** | `21b4ee2` | ~25% lower block-mean error vs hard threshold; 2-bit path byte-identical |
| wasm `eaac8614` built + deployed live | `3a718c5` | deployment `10b60e35`, MIME fixed, sha verified |
| live UI label corrected (1-bit option no longer claims "no AA greys") | `486df7b` | live `38f9a63f`, verified in served HTML |
| libpng wired into **both** toolchains | `2472b6e` | host `Built target ko_xtch_host`; wasm +33 KB, proving it links |
| **PNG stage 1** — dimension probe on libpng | `48dd25f` | `demo-png.epub`: identical 1,345,804 B, 14 pages, **0 differing pixels** |
| PNG stage-2 docs + design-C script | `463ffda`, `3f8c6a2` | see `docs/ko-png-libpng-port.md` |

**Why libpng is NOT vendored** (unlike libjpeg-turbo): JPEG implementations legitimately diverge in
IDCT rounding, hence vendored libjpeg-turbo with SIMD off. PNG decoding is exact integer arithmetic
with deterministic inflate, so the toolchain's own libpng is bit-identical.

---

## 2. ACTIVE — PNG stage 2: PNGdec → libpng scanline decode

File: `vendor-lib/Epub/Epub/converters/PngToFramebufferConverter.cpp`
Script: `scripts/png-port/stage2c_design_c.py` (a copy of `/tmp/ab/png_stage2c.py`)

### 2.1 Design — settled and working ("design C")

**Keep PNGdec's `PNGDRAW` struct as the callback's parameter and fill it synthetically from libpng.**
Exactly the trick already used for both JPEG converters (they keep `JPEGDRAW`). This avoids the
blocker found earlier: PNGdec's `iPixelType` is an **enum type taken by type** by
`convertLineToGray`, so our own `PNG_PIXEL_*` constants clash with PNGdec's enumerators and the call
site stops type-checking. Therefore PNGdec.h **stays** for stage 2 and goes in stage 3.

The callback is **already per-scanline** (`int srcY = pDraw->y;`) and does all of its own
up/downscale row mapping, so libpng delivers one synthetic block per row and the callback body is
untouched. Fields the callback actually reads (complete set): `y`, `pPixels`, `iBpp`, `iPixelType`,
`iHasAlpha`, `pPalette`, `pUser`.

### 2.2 What the script does

Deletes only well-anchored spans (brace-counted, with span assertions); it does **not** rewrite the
~100-line region, so the context/scale setup survives: PNGdec allocation (5 lines), `png->open` +
`ScopedCleanup` (3), PNGdec's two-scanline overflow guard (8). Replaces the dimension getters with
an inline libpng header probe, and replaces `rc = png->decode(&ctx, 0);` with a libpng row loop.

Three compile fixes are needed afterwards (the script does not include them):
1. `int rc = 0;` must be declared — its declaration lived on the deleted `png->open` line.
2. `png->getPixelType()` → `PNG_PIXEL_GRAYSCALE`, `png->getBpp()` → `8` (post-normalisation values).
3. `PngContext` has **no `error` member** — so `if (pngDrawCallback(&draw) == 0) break;` and
   `rc = ctx.error ? 1 : 0;` → `rc = 0;`.

**Do NOT repeat the mistake already made once:** if you re-run `stage2c_design_c.py`, it restores
the *script's* text, so a follow-up fix script must anchor on the script's original strings
(`pngDrawCallback(&draw);\n if (ctx.error) break;`), not on the already-patched version.

**Hazard found and avoided:** deleting the heap gates by walking up to "the nearest `if (`" is
wrong — `maxAlloc` is declared *before* its `if`, so it anchors on an unrelated block. The heap gates
are therefore deliberately left in place for stage 2; they are inert on host/wasm and can only go in
stage 3, when `MIN_FREE_HEAP_FOR_PNG` (defined via a PNGdec constant) goes too.

### 2.3 The failing gate — measured, not guessed

```
demo-png.epub: 1345804 bytes, 14 pages   (identical to baseline)
pages: 14   differing pages: 1
differing pixels: 3556  -> bbox x 209-471, y 393-469   (263 wide x 77 tall, every row+col)
value transitions (PNGdec -> libpng): (2->0) 3295, (0->3) 189, (2->3) 42, (1->2) 30
```

Interpretation, in order of what was actually established:

- **All 8 PNGs in that book are plain 8-bit greyscale** (`colortype=0`, no alpha, no palette, no
  `tRNS`, no 16-bit, no `gAMA`). So alpha compositing, palette ordering and 16-bit rounding are all
  **eliminated** — my normalisation chain is a no-op for these files.
- The dominant transition is PNGdec-light-grey → **white** across a solid rectangle. White is an
  untouched framebuffer ⇒ the region was **never drawn**, it was not misdecoded.
- The rect is the **bottom ~77 of ~197 rows** of a **downscaled** 400-wide image (400 × 0.6575 ≈ 263).
- The callback's emission rule is monotonic and cannot backfill:

  ```cpp
  int firstDstY = (srcY * ctx->dstHeight) / ctx->srcHeight;
  int endDstY = firstDstY + 1;                       // downscale: one output row per call
  if (firstDstY <= ctx->lastDstY) firstDstY = ctx->lastDstY + 1;
  if (firstDstY >= endDstY || firstDstY >= ctx->dstHeight) return 1;
  ```

  On a downscale each callback emits at most one output row, so **all 197 output rows appear only if
  all 300 source rows are delivered**. The last ~117 source rows never reached the callback.
- **Eliminated by experiment:** ignoring the callback's return value changes nothing (identical
  3,471/3,556), so the callback never returns 0 benignly.

### 2.4 Next experiment — ONE run, then the fix is probably obvious

The leading hypothesis is that **libpng raises an error mid-decode** and my
`setjmp(png_jmpbuf(pngRead))` branch logs `"PNG decode failed"` and abandons the rest of the image.
Previous runs grepped only `DONE` and **never looked at stderr** — that is the missing observation.

1. Apply the script + the three fixes (§2.2). Build: `cmake --build build -j8`.
2. Run **with stderr visible**: `./build/ko_xtch_host $PWD/dist/demo-png.epub /tmp/ab/png_2e.xtch`
   and read *all* output, especially any `[ERR][PNG]` line.
3. Gate: `env -i /usr/bin/python3 /tmp/ab/plane_split.py /tmp/ab/png_before.xtch /tmp/ab/png_2e.xtch`
   → target **0 differing pages, 0 differing pixels**.

If the error line **is** there: fix whatever trips it. For plain 8-bit greyscale nothing should be
normalised at all, so suspect the row buffer (`pngW * (hasAlpha ? 2 : 1)`) or a
`png_read_row`/interlace interaction.

If the error line is **not** there: instrument instead of guessing. Add a temporary log in the
callback printing `y / firstDstY / endDstY / lastDstY` per invocation, capture the sequence under
both decoders for that image, and diff the two sequences. That shows directly which rows PNGdec
delivers that libpng's loop does not.

### 2.5 Stage 3 (after stage 2 passes)

1. Port `vendor-lib/PngToBmpConverter/PngToBmpConverter.cpp` (845 lines, PNGdec file API with its
   own callbacks, dims around line 580) — same synthetic-block treatment.
2. Delete PNGdec: its include dir in `CMakeLists.txt` (~line 50), its 7 sources (~lines 82–88,
   which include its bundled zlib), and `third_party/PNGdec`. Then the heap gates can go.
3. Gate: both toolchains build, and `demo-png.epub` stays identical.

---

## 3. Remaining work (not PNG)

1. **Default reader face:** KoPub Batang 14 (upstream) vs RIDIBatang 14 (ours) — **decision pending**.
2. **ED dither wiring** (FS/Atkinson/JJN/Stucki/Burkes): implemented and measured but not wired into
   the image decoders; needs row-error state plumbing, which dropping MCU bands may now allow.
3. **Cover path end-to-end:** covers are library-UI only and the host harness never renders one, so
   cover output is unverified (could differ ~1 LSB after the JPEGDEC→libjpeg and PNGdec→libpng swaps).
4. **Force-push:** harness denies destructive pushes. Run
   `git push --force-with-lease origin main` yourself (remote `origin/main` is `a01be75`, diverged).
5. **`esp32-ereader-firmware-re` SKILL.md is 101,117 chars — over the 100,000 limit, so description
   patches are rejected.** It needs splitting.
6. **README session-log append** still pending.
7. **`master`-profile dither anomaly** — unexplained 6–9× worse tone than `kofork`; suspicion is its
   `[30,50,140]` bin / `[15,30,80,210]` level pair. Not shipped. Low priority.
8. **1-bit mono weight — CLOSED as "keep as is" (user decision 2026-09-17).** Do not re-open.
   Measurement: the dither matches the 4-level reference within 0.1% (12.51% vs 12.58% ink mass) and
   is **~9% lighter** than the hard threshold it replaced. The perceived thickness comes from
   (a) `mono/core = 1.50×` — half the ink is edge, not stroke; and (b) the font's coverage→level
   thresholds (≥25% coverage → dark grey, ≥75% → black). Any future change belongs in the font
   thresholds, ideally calibrated from a real test pattern, since the panel anchors (15/30/80/210)
   remain **unverified on hardware**.

---

## 4. Operational knowledge (hard-won)

- **macOS: always `env -i /usr/bin/python3`** — plain `python3` is the broken Hermes venv.
- **`npx` is NOT blocked** (an earlier claim was wrong). Deploy:
  `npx -y wrangler@latest pages deploy dist --project-name=crosspoint-ko-wasm --branch=main`
  (the stale OAuth token self-refreshes).
- **Never `python3 -c "..."` with raw `"` inside** — the shell terminates the string and you get a
  baffling `EOF while scanning triple-quoted string`. Use `write_file` for scripts instead.
- **Gate discipline, non-negotiable for this repo:**
  - Page-level comparison (`plane_split.py`), **never whole-file `cmp`** — the container carries a
    millis field near offset 297 that differs run to run.
  - Exact gates beat eyeballs: dimensions feed pagination (PNG stage 1), PNG is lossless (stage 2).
  - Capture the baseline *before* patching when no committed baseline exists.
- **Patch surgery rules that actually worked:** exact-text replacements with occurrence assertions;
  brace counting that strips strings/comments; **span-length assertions so a bad anchor aborts
  instead of eating code**; back up before each risky pass; commit before each risky pass.
- **Host build is the verification harness** — the wasm output cannot be run headlessly here
  (emscripten path sandbox), so all gates run on `build/ko_xtch_host`.
- `timeout` is absent on macOS (use `gtimeout` or `perl -e 'alarm N; exec @ARGV'`); browser console
  evals time out around 30 s; foreground terminal timeout caps at 600 s, so long builds go
  `background=true, notify_on_complete=true`.
- Commercial `web/demo.epub` must never be deployed: git-ignored **and** excluded by
  `scripts/build_dist.sh`, which fails hard if it leaks.

---

## 5. Artifacts

- **Docs:** `docs/ko-png-libpng-port.md` (stage 1 done, stage 2 mapped, mechanics + hazards),
  `docs/ko-jpeg-libjpeg-port.md` (JPEG recipe + the 1-bit dither decision record),
  `docs/ko-engine-upstream-parity.md`.
- **Scripts:** `scripts/png-port/stage2c_design_c.py`; scratch verification tools in `/tmp/ab/`
  (`plane_split.py`, `weight_report.py`, `mono_metric.py`, and the baselines
  `png_before.xtch` = 1,345,804 B / 14 pages, plus `mono_ab.png`).
  **`/tmp` is not durable — copy anything needed into `scripts/` or `docs/`.**
- **Live/deploy:** `web/`, `dist/`, `wrangler.toml`, `scripts/build_dist.sh`.

---

# 핸드오프 — ko-wasm (Xteink X4 EPUB → XTC/XTCH 변환기)

**날짜:** 2026-09-17 · **저장소:** https://github.com/zgredex/crosspoint-ko-wasm (public, MIT)
**작업 디렉터리:** `/Users/patryk/krxtc/ko-wasm` · **브랜치:** `main`

---

## 0. 다음 세션을 위한 요약

- **라이브:** https://crosspoint-ko-wasm.pages.dev — 배포 `38f9a63f`, wasm sha256 `eaac8614…`,
  `dist/`와 일치 검증 완료.
- **트리는 깨끗하고 정상입니다.** 호스트와 wasm 모두 빌드되고, 모든 게이트가 통과하며,
  중간에 방치된 변경은 없습니다.
- **진행 중인 작업: PNG 2단계** — PNG 스캔라인 디코드를 PNGdec에서 libpng로 옮기는 작업입니다.
  설계는 확정되었고, 스크립트도 작성되었으며, 빌드도 동작합니다. 게이트는 **이미지 하나**에서만
  실패합니다. 다음 단계는 더 이상의 설계가 아니라 **실험 한 번**(§2.4)입니다.
- **보류 항목:** 기본 서체 결정, ED 디더 배선, 표지 경로 검증, force-push, SKILL.md 크기 수정,
  README 세션 로그. 회전 4모드는 계속 보류 — 재개하지 마십시오.

---

## 1. 이번 세션에서 완료·검증된 작업

| 작업 | 커밋 | 근거 |
|---|---|---|
| libjpeg-turbo가 JPEGDEC 대체 (프레임버퍼 경로) | `45aac93` | 책 바이트 동일; progressive 수정(DC-only 1/8이었음) |
| 디더 재작성 + 기본 프로파일 = ko 포크 `45/70/140` | `7fb9b3c` | 톤 오차 1.372 대 `master` 프로파일 11.6, 독립 소스 3개에서 확인 |
| PXC 픽셀 캐시 제거 (기기 전용 죽은 코드) | `a3559d2` | 14 + 2,034 페이지, **차이 픽셀 0** |
| JPEGDEC 완전 제거 (표지 경로 → libjpeg) | `ca0d2c7` | 2,048 페이지, 차이 페이지 0; 빌드 입력에 JPEGDEC 참조 0 |
| 1비트 청색잡음 텍스트 디더, **기본 ON** | `21b4ee2` | 하드 임계값 대비 블록평균 오차 약 25% 개선; 2비트 경로 바이트 동일 |
| wasm `eaac8614` 빌드 + 라이브 배포 | `3a718c5` | 배포 `10b60e35`, MIME 수정, sha 검증 |
| 라이브 UI 라벨 수정 (1비트 옵션의 "no AA greys" 문구 제거) | `486df7b` | 라이브 `38f9a63f`, 제공 HTML에서 확인 |
| libpng를 **양쪽 툴체인**에 연결 | `2472b6e` | 호스트 `Built target ko_xtch_host`; wasm +33 KB로 실제 링크 증명 |
| **PNG 1단계** — 크기 프로브를 libpng로 | `48dd25f` | `demo-png.epub`: 1,345,804 B, 14 페이지 동일, **차이 픽셀 0** |
| PNG 2단계 문서 + design-C 스크립트 | `463ffda`, `3f8c6a2` | `docs/ko-png-libpng-port.md` 참조 |

**libpng를 벤더링하지 않은 이유** (libjpeg-turbo와 다른 점): JPEG 구현은 IDCT 반올림에서 정당하게
달라지므로 SIMD를 끈 libjpeg-turbo를 벤더링했습니다. PNG 디코딩은 정확한 정수 연산이고 inflate가
결정적이므로, 툴체인 자체의 libpng가 비트 단위로 동일합니다.

---

## 2. 진행 중 — PNG 2단계: PNGdec → libpng 스캔라인 디코드

파일: `vendor-lib/Epub/Epub/converters/PngToFramebufferConverter.cpp`
스크립트: `scripts/png-port/stage2c_design_c.py` (`/tmp/ab/png_stage2c.py`의 사본)

### 2.1 설계 — 확정되었고 동작함 ("design C")

**PNGdec의 `PNGDRAW` 구조체를 콜백 매개변수로 유지하고, libpng에서 합성해 채웁니다.**
이미 두 JPEG 변환기가 `JPEGDRAW`를 유지한 것과 정확히 같은 방식입니다. 이로써 앞서 발견한
차단 요소를 피합니다: PNGdec의 `iPixelType`은 `convertLineToGray`가 **타입 그대로** 받는
**열거형**이므로, 자체 `PNG_PIXEL_*` 상수를 정의하면 PNGdec의 열거자와 충돌하고 호출부의
타입 검사가 깨집니다. 따라서 2단계에서는 PNGdec.h를 **유지**하고 3단계에서 제거합니다.

콜백은 **이미 스캔라인 단위**이며(`int srcY = pDraw->y;`) 상하 스케일 행 매핑을 스스로
처리하므로, libpng가 행마다 합성 블록 하나를 넘기면 콜백 본문은 그대로 둘 수 있습니다.
콜백이 실제로 읽는 필드(전체 목록): `y`, `pPixels`, `iBpp`, `iPixelType`, `iHasAlpha`,
`pPalette`, `pUser`.

### 2.2 스크립트가 하는 일

잘 고정된 구간만 삭제합니다(중괄호 계산 + 길이 단언). ~100줄 영역은 **다시 쓰지 않으므로**
컨텍스트/스케일 설정이 보존됩니다: PNGdec 할당(5줄), `png->open` + `ScopedCleanup`(3줄),
PNGdec의 2-스캔라인 오버플로 가드(8줄). 크기 게터는 인라인 libpng 헤더 프로브로 바꾸고,
`rc = png->decode(&ctx, 0);`는 libpng 행 루프로 교체합니다.

이후 컴파일 수정 3건이 필요합니다(스크립트에 포함되지 않음):
1. `int rc = 0;` 선언이 필요합니다 — 선언이 삭제된 `png->open` 줄에 있었습니다.
2. `png->getPixelType()` → `PNG_PIXEL_GRAYSCALE`, `png->getBpp()` → `8` (정규화 이후 값).
3. `PngContext`에 **`error` 멤버가 없습니다** — 따라서
   `if (pngDrawCallback(&draw) == 0) break;` 와 `rc = ctx.error ? 1 : 0;` → `rc = 0;`.

**이미 한 번 저지른 실수를 반복하지 마십시오:** `stage2c_design_c.py`를 다시 실행하면 파일이
*스크립트의* 원문으로 돌아가므로, 후속 수정 스크립트는 이미 패치된 문자열이 아니라 스크립트
원본 문자열(`pngDrawCallback(&draw);\n if (ctx.error) break;`)에 앵커를 걸어야 합니다.

**발견하고 회피한 위험:** 힙 게이트를 "가장 가까운 `if (`"까지 거슬러 올라가 삭제하는 방식은
잘못입니다 — `maxAlloc`이 해당 `if`보다 *먼저* 선언되므로 엉뚱한 블록에 앵커가 걸립니다.
따라서 2단계에서는 힙 게이트를 의도적으로 남겨 둡니다. 호스트/wasm에서는 무해하며,
`MIN_FREE_HEAP_FOR_PNG`(PNGdec 상수 기반)가 사라지는 3단계에서 함께 제거해야 합니다.

### 2.3 실패한 게이트 — 추측이 아니라 측정

```
demo-png.epub: 1345804 바이트, 14 페이지   (기준과 동일)
pages: 14   가 다른 페이지: 1
차이 픽셀: 3556  -> bbox x 209-471, y 393-469   (263 x 77, 모든 행·열 포함)
값 전이 (PNGdec -> libpng): (2->0) 3295, (0->3) 189, (2->3) 42, (1->2) 30
```

실제로 확인된 순서대로 해석하면:

- **해당 책의 PNG 8개는 모두 순수 8비트 그레이스케일**입니다 (`colortype=0`, 알파·팔레트·
  `tRNS`·16비트·`gAMA` 없음). 따라서 알파 합성, 팔레트 순서, 16비트 반올림 가능성은 **모두
  배제**됩니다 — 이 파일들에 대해 제 정규화 체인은 아무 일도 하지 않습니다.
- 지배적인 전이는 PNGdec의 밝은 회색 → **흰색**이며, 단색 사각형 전체에 걸쳐 나타납니다.
  흰색은 손대지 않은 프레임버퍼이므로, 이 영역은 **그려지지 않은 것**이지 잘못 디코드된 것이
  아닙니다.
- 사각형은 400폭 **다운스케일** 이미지(400 × 0.6575 ≈ 263)의 **아래 약 77행 / 전체 약 197행**에
  해당합니다.
- 콜백의 출력 규칙은 단조 증가하며 되메우기가 불가능합니다:

  ```cpp
  int firstDstY = (srcY * ctx->dstHeight) / ctx->srcHeight;
  int endDstY = firstDstY + 1;                       // 다운스케일: 호출당 출력 행 1개
  if (firstDstY <= ctx->lastDstY) firstDstY = ctx->lastDstY + 1;
  if (firstDstY >= endDstY || firstDstY >= ctx->dstHeight) return 1;
  ```

  다운스케일에서 각 콜백은 출력 행을 최대 하나만 방출하므로, **197개 출력 행 전부가 나오려면
  300개 소스 행 전부가 전달되어야 합니다.** 마지막 약 117개 소스 행이 콜백에 도달하지
  않았습니다.
- **실험으로 배제:** 콜백 반환값을 무시해도 결과가 동일합니다(3,471/3,556 동일). 따라서 콜백이
  정상 경로에서 0을 반환하지는 않습니다.

### 2.4 다음 실험 — 실행 한 번, 그러면 수정은 거의 자명합니다

가장 유력한 가설은 **libpng가 디코드 도중 오류를 발생**시키고 제
`setjmp(png_jmpbuf(pngRead))` 분기가 `"PNG decode failed"`를 기록한 뒤 나머지 이미지를
포기한다는 것입니다. 이전 실행들은 `DONE`만 grep했고 **stderr를 전혀 보지 않았습니다** —
그것이 빠진 관측입니다.

1. 스크립트 + 수정 3건(§2.2) 적용. 빌드: `cmake --build build -j8`.
2. **stderr가 보이도록** 실행:
   `./build/ko_xtch_host $PWD/dist/demo-png.epub /tmp/ab/png_2e.xtch`
   그리고 `[ERR][PNG]`를 포함한 *모든* 출력을 확인합니다.
3. 게이트:
   `env -i /usr/bin/python3 /tmp/ab/plane_split.py /tmp/ab/png_before.xtch /tmp/ab/png_2e.xtch`
   → 목표는 **차이 페이지 0, 차이 픽셀 0**.

오류 줄이 **있다면**: 그 원인을 수정합니다. 순수 8비트 그레이스케일에서는 정규화가 아무것도
하지 않아야 하므로, 행 버퍼(`pngW * (hasAlpha ? 2 : 1)`)나 `png_read_row`/인터레이스 상호작용을
의심하십시오.

오류 줄이 **없다면**: 추측 대신 계측하십시오. 콜백에 `y / firstDstY / endDstY / lastDstY`를
호출마다 출력하는 임시 로그를 넣고, 해당 이미지에 대해 두 디코더의 시퀀스를 각각 캡처해
비교하십시오. 그러면 PNGdec가 전달하고 libpng 루프가 전달하지 않는 행이 무엇인지 직접
드러납니다.

### 2.5 3단계 (2단계 통과 후)

1. `vendor-lib/PngToBmpConverter/PngToBmpConverter.cpp`(845줄, PNGdec 파일 API와 자체 콜백,
   크기 부분 약 580행)를 이식 — 동일한 합성 블록 방식.
2. PNGdec 삭제: `CMakeLists.txt`의 include 디렉터리(약 50행), 소스 7개(약 82–88행, 번들 zlib
   포함), `third_party/PNGdec`. 그런 다음 힙 게이트도 제거할 수 있습니다.
3. 게이트: 양쪽 툴체인 빌드 성공 + `demo-png.epub` 동일 유지.

---

## 3. 남은 작업 (PNG 외)

1. **기본 서체:** KoPub Batang 14(업스트림) 대 RIDIBatang 14(자체) — **결정 보류 중**.
2. **ED 디더 배선**(FS/Atkinson/JJN/Stucki/Burkes): 구현·측정은 완료했으나 이미지 디코더에
   연결되지 않았습니다. 행 오차 상태 전달이 필요하며, MCU 밴드 제거로 이제 가능해졌을 수 있습니다.
3. **표지 경로 종단 검증:** 표지는 라이브러리 UI 전용이고 호스트 하네스가 렌더링하지 않으므로
   표지 출력은 미검증입니다(JPEGDEC→libjpeg, PNGdec→libpng 교체 후 약 1 LSB 차이 가능).
4. **force-push:** 하네스가 파괴적 푸시를 거부합니다. 직접
   `git push --force-with-lease origin main` 실행(원격 `origin/main`은 `a01be75`, 분기 상태).
5. **`esp32-ereader-firmware-re` SKILL.md가 101,117자로 100,000자 한도 초과** → description 패치가
   거부됩니다. 분할이 필요합니다.
6. **README 세션 로그 추가** 아직 미완.
7. **`master` 프로파일 디더 이상 현상** — `kofork`보다 톤이 6–9배 나쁜 원인 미규명; `[30,50,140]`
   bin / `[15,30,80,210]` 레벨 쌍이 의심됩니다. 배포되지 않았습니다. 우선순위 낮음.
8. **1비트 모노 굵기 — 사용자 결정(2026-09-17)으로 "현행 유지" 종료.** 다시 열지 마십시오.
   측정: 디더는 4레벨 기준과 0.1% 이내로 일치(잉크 질량 12.51% 대 12.58%)하며, 대체한 하드
   임계값보다 **약 9% 가볍습니다**. 체감 굵기는 (a) `mono/core = 1.50×` — 잉크의 절반이 획이
   아니라 가장자리이고, (b) 폰트의 커버리지→레벨 임계값(커버리지 ≥25% → 진회색, ≥75% → 검정)
   때문입니다. 향후 변경이 있다면 폰트 임계값 쪽이며, 이상적으로는 실기기 테스트 패턴으로
   보정해야 합니다. 패널 앵커(15/30/80/210)는 **하드웨어에서 미검증**입니다.

---

## 4. 운영 지식 (시행착오로 얻은 것)

- **macOS: 항상 `env -i /usr/bin/python3`** — 그냥 `python3`는 깨진 Hermes venv입니다.
- **`npx`는 차단되지 않습니다**(이전 주장은 오류였습니다). 배포:
  `npx -y wrangler@latest pages deploy dist --project-name=crosspoint-ko-wasm --branch=main`
  (만료된 OAuth 토큰은 자동 갱신됩니다).
- **`python3 -c "..."` 안에 raw `"`를 넣지 마십시오** — 셸이 문자열을 끊어 이해하기 어려운
  `EOF while scanning triple-quoted string`이 발생합니다. 스크립트는 `write_file`로 작성하십시오.
- **이 저장소의 게이트 규율 — 필수:**
  - 페이지 단위 비교(`plane_split.py`), **전체 파일 `cmp`는 금지** — 컨테이너의 약 297 오프셋에
    실행마다 달라지는 millis 필드가 있습니다.
  - 정확한 게이트가 육안보다 낫습니다: 크기는 페이지네이션에 영향(PNG 1단계), PNG는 무손실
    (2단계).
  - 커밋된 기준이 없으면 패치 *전에* 기준을 캡처하십시오.
- **실제로 통한 패치 수술 규칙:** 발생 횟수 단언이 있는 정확 문자열 치환; 문자열/주석을 제거한
  뒤의 중괄호 계산; **잘못된 앵커가 코드를 먹지 않도록 길이 단언**; 위험한 작업 전 백업;
  위험한 작업 전 커밋.
- **호스트 빌드가 검증 하네스입니다** — 여기서 wasm 출력을 헤드리스로 실행할 수 없으므로
  (emscripten 경로 샌드박스) 모든 게이트는 `build/ko_xtch_host`에서 실행됩니다.
- macOS에는 `timeout`이 없습니다(`gtimeout` 또는 `perl -e 'alarm N; exec @ARGV'` 사용); 브라우저
  콘솔 평가는 약 30초에 타임아웃; 전경 터미널 제한은 600초이므로 긴 빌드는
  `background=true, notify_on_complete=true`를 사용하십시오.
- 상용 `web/demo.epub`는 절대 배포하면 안 됩니다: git-ignore 처리되어 있고
  `scripts/build_dist.sh`가 제외하며, 유출 시 하드 실패합니다.

---

## 5. 산출물

- **문서:** `docs/ko-png-libpng-port.md`(1단계 완료, 2단계 매핑, 메커니즘 + 위험 요소),
  `docs/ko-jpeg-libjpeg-port.md`(JPEG 레시피 + 1비트 디더 결정 기록),
  `docs/ko-engine-upstream-parity.md`.
- **스크립트:** `scripts/png-port/stage2c_design_c.py`; 스크래치 검증 도구는 `/tmp/ab/`
  (`plane_split.py`, `weight_report.py`, `mono_metric.py`, 그리고 기준 파일
  `png_before.xtch` = 1,345,804 B / 14 페이지, `mono_ab.png`).
  **`/tmp`는 영구적이지 않습니다.** 검증 도구는 이제 `scripts/verify/`에 커밋되어 있습니다
  (`plane_split.py`, `weight_report.py`, `mono_metric.py`). 게이트 기준 파일
  `png_before.xtch`는 커밋된 PNGdec 기반 소스에서 한 단계로 재생성할 수 있습니다:
  `./build/ko_xtch_host $PWD/dist/demo-png.epub /tmp/ab/png_before.xtch` (패치 전에 실행).
- **라이브/배포:** `web/`, `dist/`, `wrangler.toml`, `scripts/build_dist.sh`.
