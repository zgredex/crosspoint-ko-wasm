# KO WASM engine — parity audit against upstream firmware

**Date:** 2026-09-17
**Upstream of record:** `crosspoint-reader-ko/crosspoint-reader-ko` @ `release/korean`
(HEAD = `1.5.0-ko.3`, released 2026-08-19)
**Scope:** every firmware-side setting that can affect text — indentation, margins,
line breaking, spacing, fonts. Device-only UI (sleep, buttons, themes, clock
hardware) is out of scope by design; see "Deliberate divergence".

---

## 1. Method — diff the whole engine, don't spot-check settings

The port vendors the firmware's own sources under `vendor-lib/`, so parity is a
file-by-file diff against upstream HEAD — with a control test, so a failed fetch
can never masquerade as a match.

```sh
R=crosspoint-reader-ko/crosspoint-reader-ko
gh api "repos/$R/git/trees/HEAD?recursive=1" \
  --jq '.tree[] | select(.type=="blob") | select(.path|test("^lib/(Epub|EpdFont|GfxRenderer)/")) | .path' \
  > up_files.txt
# fetch each path, diff against vendor-lib/<path minus lib/>
# CONTROL: a mismatched pair must report >0 diff lines; an identical pair must match on md5.
# Also count files upstream has that the port lacks entirely.
```

Control results: `Section.cpp` vs `ParsedText.cpp` → 2,657 diff lines;
`Section.cpp` md5 `e7181a03455d80f82835292084842262` identical on both sides.

## 2. Result

| | Count |
|---|---|
| Upstream files under `lib/{Epub,EpdFont,GfxRenderer}` | 168 |
| Missing from the port | **0** |
| Byte-identical | **167** |
| Divergent | **1** |

All typesetting sources are exact copies: `Section.cpp/.h`, `ParsedText.cpp/.h`,
`blocks/TextBlock.*`, `blocks/BlockStyle.h`, `css/CssParser.cpp`, `css/CssStyle.h`,
`parsers/ChapterHtmlSlimParser.*`, `Page.*`, `VisibleTextUtils.h`, and the whole
`EpdFont` family. `builtinFonts/kopub_14_regular.h` — the compiled device face —
is **sha256-identical** (`40aea8e71252934308c4c6f0a7e78601d80f1225c895b8004fe59adf719ac367`),
so glyph advances and vertical metrics match the device exactly.

## 3. The divergence — `getLineHeight()` truncation

`vendor-lib/GfxRenderer/GfxRenderer.cpp`:

```cpp
// upstream HEAD
return static_cast<int>(getLineHeight(fontId) * compression + 0.5f);   // ROUNDS

// port before the fix
return static_cast<int>(getLineHeight(fontId) * compression);          // truncated
```

The port's comment claimed "the Korean fork truncates the scaled advance",
citing a document that no longer exists upstream. It landed on the port's
**default** font at the **default** spacing: RIDIBatang 14 (advanceY 38) × 1.20 =
45.6 px → upstream 46 px, truncating build 45 px → 17 vs 16 lines per 778 px page.

### Evidence — single-variable A/B

Only that one line was toggled; incremental rebuild; same book, same settings.

| Build | Pages | Container bytes |
|---|---|---|
| truncating (before) | 1,981 | 190,257,350 |
| rounding (upstream behaviour, after) | **2,034** | **195,347,364** |

**+53 pages / +2.7 %.** The 1,981 figure reproduces the value recorded before any
of this work, which confirms nothing else moved. An image-dominated book
(`demo-images.epub`) was unchanged at 14 pages both ways.

## 4. Settings parity matrix

| Firmware setting | Authoritative source | Port | Status |
|---|---|---|---|
| `lineCompression` | `getReaderLineCompression()` → 1.00 / 1.20 / 1.40, identical for every KO face | same | exact |
| `paragraphAlignment` (0–4) | `PARAGRAPH_ALIGNMENT`; `BOOK_STYLE=4` → `BlockStyle::fromCssStyle` | same | exact |
| `paragraphIndent` (default 0) | `applyParagraphIndent()` prefixes **U+3000** | same | exact |
| `extraParagraphSpacing` (default 1) | participates in the double-indent guard | same | exact |
| `characterWrap` (default 1) | KO break-anywhere | same | exact |
| `hyphenationEnabled` | gated: `hyphenation && characterWrap == 0` | **added this pass** | exact |
| `embeddedStyle` (default 1) | gates the CSS parser (`Section.cpp:377`) | same | exact |
| `textAntiAliasing` (default 1) | `CrossPointSettings` field | same | exact |
| `imageRendering` (0/1/2) | `IMAGE_RENDERING` | same | exact |
| `screenMargin` (5–40 step 5, default 5) | `SCREEN_MARGIN_*` | same | exact |
| `focusReadingEnabled` | forced **0** in the KO build | hardcoded 0 | exact |
| `fontId` | `hasCustomFont() ? CUSTOM : KOPUB_14_FONT_ID` | same — KoPub Batang 14 | exact (RIDIBatang 14 kept as an XTCKO extra) |
| `orientation` (4 modes) | remaps viewable margins; swaps 480×800 ↔ 800×480 | portrait only | **parked** |
| `fontPointSize`, `fontFamily` | inert in the KO build (font comes from `getReaderFontId()`) | n/a | exact |
| status bar, sleep, buttons, clock, theme, tilt, touch | device UI / hardware | absent | exact (see §5) |

### Margins, exactly

`EpubReaderActivity::render()` derives the text viewport as:

```
margins = GfxRenderer::getOrientedViewableTRBL()   → portrait 9/3/3/3
top   += screenMargin
left  += screenMargin
right += screenMargin
bottom += max(screenMargin, statusBarHeight)
viewport = screen − (left + right, top + bottom)
```

Default margin 5 → `14 / 8 / 22 / 8` → **464 × 764 px** (statusBarHeight = 19 at the shipped
defaults, see §5). The port now follows this exactly — `ko::geom::referenceMargins()` in
`src/ko_engine_driver.h` and `marginsFor()` in `web/ko.worker.js` are the same arithmetic.
The earlier `14 / 8 / 8 / 8` → 464 × 778 divergence is gone; see
`docs/ko-oracle-conformance.md` §6 for the measurement and why the reservation is safe to
bake into page geometry.

## 5. Status-bar lane — reserved in geometry, never drawn

The device's EPUB reader reserves a status-bar lane (`statusBarHeight = 19` for the shipped
defaults, via `UITheme::getStatusBarHeight()` → `metrics.statusBarVerticalMargin`, identical
in all shipped themes), and the exporter now reserves the same 19 px so its line breaks land
where the reader's would. Nothing of the UI is drawn into a page: an XTC/XTCH page is a
finished bitmap and the device composites its own chrome at read time
(`XtcReaderActivity` + `xtcStatusBarMode`, whose default is `XTC_STATUS_BAR_HIDE`).

The reservation is a constant, not a function of a UI setting, so one book cannot paginate
two ways because a device has its status bar configured differently.

## 6. Open items

1. ~~**Default face**~~ — closed: the port now ships `KOPUB_14_FONT_ID` as its default
   (`getReaderFontId()`), with RIDIBatang 14 kept as a selectable extra.
2. **Orientation** — the 4 firmware modes remain parked.
3. **Deployment** — the live Pages site still carries the pre-fix engine and the
   earlier broken `_headers`; one redeploy of `dist/` resolves both.

## 7. Reproduce

```sh
# host (page-count A/B) — ABSOLUTE paths required
cmake -S . -B build && cmake --build build -j8
./build/ko_xtch_host /abs/path/book.epub /abs/path/out.xtch

# wasm (the site)
export PATH="/opt/homebrew/bin:$PATH"
emcmake cmake -S . -B build-wasm -DCMAKE_BUILD_TYPE=Release && cmake --build build-wasm -j8
cp build-wasm/ko_xtch_wasm.{js,wasm} web/ && bash scripts/build_dist.sh
```

Current engine artifact: `6d34d72e1e99cbe40f9a65492a6595ae98d3e0ae32d2d672b2c48370361b09a3`.

---
---

# KO WASM 엔진 — 업스트림 펌웨어 대비 동등성 감사

**날짜:** 2026-09-17
**기준 업스트림:** `crosspoint-reader-ko/crosspoint-reader-ko` @ `release/korean`
(HEAD = `1.5.0-ko.3`, 2026-08-19 릴리스)
**범위:** 텍스트에 영향을 줄 수 있는 모든 펌웨어 측 설정 — 들여쓰기, 여백, 줄바꿈,
행간, 글꼴. 기기 전용 UI(슬립, 버튼, 테마, 시계 하드웨어)는 설계상 범위 밖이다.
"의도적 차이" 항목 참고.

---

## 1. 방법 — 설정 몇 개를 확인하는 것이 아니라 엔진 전체를 비교한다

이 포트는 펌웨어 소스를 그대로 `vendor-lib/` 아래에 벤더링하고 있으므로, 동등성
판정의 기준은 업스트림 HEAD와의 **파일 단위 전체 diff**다. 이때 대조군(control)을
반드시 함께 돌려서, 가져오기에 실패한 파일이 "일치"로 위장하지 못하게 한다.

```sh
R=crosspoint-reader-ko/crosspoint-reader-ko
gh api "repos/$R/git/trees/HEAD?recursive=1" \
  --jq '.tree[] | select(.type=="blob") | select(.path|test("^lib/(Epub|EpdFont|GfxRenderer)/")) | .path' \
  > up_files.txt
# 각 경로를 받아 vendor-lib/<lib/ 제거한 경로> 와 diff
# 대조군: 서로 다른 파일 쌍은 반드시 0줄 초과, 동일한 파일 쌍은 md5가 일치해야 한다.
# 업스트림에는 있는데 포트에 없는 파일 개수도 함께 센다.
```

대조군 결과: `Section.cpp` vs `ParsedText.cpp` → diff 2,657줄;
`Section.cpp` md5 `e7181a03455d80f82835292084842262` 양쪽 동일.

## 2. 결과

| | 개수 |
|---|---|
| `lib/{Epub,EpdFont,GfxRenderer}` 아래 업스트림 파일 | 168 |
| 포트에 없는 파일 | **0** |
| 바이트 단위 완전 일치 | **167** |
| 차이 있음 | **1** |

조판 관련 소스는 전부 완전히 동일한 사본이다: `Section.cpp/.h`, `ParsedText.cpp/.h`,
`blocks/TextBlock.*`, `blocks/BlockStyle.h`, `css/CssParser.cpp`, `css/CssStyle.h`,
`parsers/ChapterHtmlSlimParser.*`, `Page.*`, `VisibleTextUtils.h`, 그리고 `EpdFont`
계열 전체. 컴파일된 기기 글꼴인 `builtinFonts/kopub_14_regular.h`는
**sha256까지 동일**하므로
(`40aea8e71252934308c4c6f0a7e78601d80f1225c895b8004fe59adf719ac367`),
글리프 전진폭과 수직 메트릭이 기기와 정확히 일치한다.

## 3. 발견된 차이 — `getLineHeight()` 절사

`vendor-lib/GfxRenderer/GfxRenderer.cpp`:

```cpp
// 업스트림 HEAD
return static_cast<int>(getLineHeight(fontId) * compression + 0.5f);   // 반올림

// 수정 전 포트
return static_cast<int>(getLineHeight(fontId) * compression);          // 절사
```

포트의 주석은 "한국어 포크는 스케일된 전진폭을 절사한다"고 주장하며 업스트림에 더는
존재하지 않는 문서를 근거로 들고 있었다. 이 차이는 포트의 **기본 글꼴**과 **기본
행간**에서 그대로 드러난다: RIDIBatang 14(advanceY 38) × 1.20 = 45.6 px →
업스트림은 46 px, 절사 빌드는 45 px → 778 px 페이지당 17줄 대 16줄.

### 증거 — 단일 변수 A/B

해당 한 줄만 토글하고 증분 재빌드한 뒤, 같은 책·같은 설정으로 실행했다.

| 빌드 | 페이지 수 | 컨테이너 바이트 |
|---|---|---|
| 절사 (수정 전) | 1,981 | 190,257,350 |
| 반올림 (업스트림 동작, 수정 후) | **2,034** | **195,347,364** |

**+53페이지 / +2.7 %.** 1,981은 이 작업 이전에 기록해 둔 값과 정확히 일치하며, 이는
다른 요소가 움직이지 않았음을 확인해 준다. 이미지 위주 도서
(`demo-images.epub`)는 양쪽 모두 14페이지로 변화가 없었다.

## 4. 설정 동등성 매트릭스

| 펌웨어 설정 | 권위 있는 근거 | 포트 | 상태 |
|---|---|---|---|
| `lineCompression` | `getReaderLineCompression()` → 1.00 / 1.20 / 1.40, 모든 한국어 글꼴에 동일 | 동일 | 정확 |
| `paragraphAlignment` (0–4) | `PARAGRAPH_ALIGNMENT`; `BOOK_STYLE=4` → `BlockStyle::fromCssStyle` | 동일 | 정확 |
| `paragraphIndent` (기본 0) | `applyParagraphIndent()`가 **U+3000**을 접두 | 동일 | 정확 |
| `extraParagraphSpacing` (기본 1) | 이중 들여쓰기 방지 조건에 참여 | 동일 | 정확 |
| `characterWrap` (기본 1) | 한국어 임의 위치 줄바꿈 | 동일 | 정확 |
| `hyphenationEnabled` | 조건부: `hyphenation && characterWrap == 0` | **이번에 추가** | 정확 |
| `embeddedStyle` (기본 1) | CSS 파서 게이트 (`Section.cpp:377`) | 동일 | 정확 |
| `textAntiAliasing` (기본 1) | `CrossPointSettings` 필드 | 동일 | 정확 |
| `imageRendering` (0/1/2) | `IMAGE_RENDERING` | 동일 | 정확 |
| `screenMargin` (5–40, 간격 5, 기본 5) | `SCREEN_MARGIN_*` | 동일 | 정확 |
| `focusReadingEnabled` | 한국어 빌드에서 **0**으로 고정 | 0 하드코딩 | 정확 |
| `fontId` | `hasCustomFont() ? CUSTOM : KOPUB_14_FONT_ID` | 동일 — KoPub 바탕 14 | 정확 (RIDIBatang 14는 XTCKO 추가 글꼴) |
| `orientation` (4개 모드) | 가시 영역 여백 재매핑, 480×800 ↔ 800×480 교체 | 세로 모드만 | **보류** |
| `fontPointSize`, `fontFamily` | 한국어 빌드에서 무효 (`getReaderFontId()`가 글꼴 결정) | 해당 없음 | 정확 |
| 상태 표시줄, 슬립, 버튼, 시계, 테마, 기울기, 터치 | 기기 UI / 하드웨어 | 없음 | 정확 (§5 참고) |

### 여백은 정확하다

`EpubReaderActivity::render()`의 본문 영역 계산은 다음과 같다:

```
margins = GfxRenderer::getOrientedViewableTRBL()   → 세로 9/3/3/3
top   += screenMargin
left  += screenMargin
right += screenMargin
bottom += max(screenMargin, statusBarHeight)
viewport = screen − (left + right, top + bottom)
```

여백 기본값 5 → `14 / 8 / 22 / 8` → **464 × 764 px** (기본 설정에서 statusBarHeight = 19,
§5 참고). 포트는 이제 이 계산을 그대로 따른다 — `src/ko_engine_driver.h`의
`ko::geom::referenceMargins()`와 `web/ko.worker.js`의 `marginsFor()`가 같은 산술이다.
이전의 `14 / 8 / 8 / 8` → 464 × 778 차이는 제거되었다. 측정값과 예약이 왜 안전한지는
`docs/ko-oracle-conformance.md` §6 참고.

## 5. 상태 표시줄 영역 — 기하에는 예약, 파일에는 없음

기기 EPUB 리더는 상태 표시줄 영역을 예약하며(`출시 기본값에서 statusBarHeight = 19`,
`UITheme::getStatusBarHeight()` → `metrics.statusBarVerticalMargin`, 모든 테마 동일),
변환기도 같은 19 px를 예약해 줄바꿈 위치가 리더와 같아진다. 페이지에 UI를 그려 넣지는
않는다: XTC/XTCH 페이지는 완성된 비트맵이고 기기는 읽는 시점에 자체 크롬을 합성한다
(`XtcReaderActivity` + `xtcStatusBarMode`, 기본값 `XTC_STATUS_BAR_HIDE`).

예약은 UI 설정의 함수가 아니라 상수이므로, 기기의 상태 표시줄 설정 때문에 같은 책이
두 가지로 나뉘는 일은 없다.

## 6. 미결 사항

1. ~~**기본 서체**~~ — 종료: 포트의 기본값은 이제 `KOPUB_14_FONT_ID`이며
   (`getReaderFontId()`), RIDIBatang 14는 선택 가능한 추가 글꼴로 남는다.
2. **화면 방향** — 4개 펌웨어 모드는 여전히 보류 상태다.
3. **배포** — 라이브 Pages 사이트는 아직 수정 전 엔진과 이전의 잘못된 `_headers`를
   제공하고 있다. `dist/`를 한 번 다시 배포하면 둘 다 해결된다.

## 7. 재현 방법

```sh
# 호스트 (페이지 수 A/B) — 반드시 절대 경로를 사용한다
cmake -S . -B build && cmake --build build -j8
./build/ko_xtch_host /abs/path/book.epub /abs/path/out.xtch

# wasm (웹사이트)
export PATH="/opt/homebrew/bin:$PATH"
emcmake cmake -S . -B build-wasm -DCMAKE_BUILD_TYPE=Release && cmake --build build-wasm -j8
cp build-wasm/ko_xtch_wasm.{js,wasm} web/ && bash scripts/build_dist.sh
```

현재 엔진 산출물:
`6d34d72e1e99cbe40f9a65492a6595ae98d3e0ae32d2d672b2c48370361b09a3`.
