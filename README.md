# XTCKO — KO 포크 렌더링 규칙을 따르는 EPUB → XTC 변환기

**Live:** https://crosspoint-ko-wasm.pages.dev

An EPUB to XTC converter that follows the KO fork rendering rules — the site is Korean-first
(the engine and the rendering rules are unchanged; see the note below for the engineering write-up).

---

# KO-Fork EPUB → XTCH WASM Module — Build & API

Status: **DONE — verified end-to-end** (2026-09-09)

The CrossPoint-KO Korean fork renderer (`crosspoint-reader-ko`, branch `release/korean`,
1.5.0-ko.3, **MIT** — not GPL like upstream CREngine) is ported to a host binary AND an
Emscripten WASM module. It renders EPUB pages with the device's exact 3-pass grayscale
pipeline (BW → LSB → MSB) and packs them into XTCH containers byte-compatible with the
on-device XTC/XTH decode contract.

- Korean EPUB rendered: `생각에 관한 생각` (test-3.epub) → **1677 pages**
- Host output `/tmp/think.xtch` and WASM output `/tmp/wasm-out.xtch`:
  **SHA-256 identical** `b005d3bfb6f224d5f23460c21a8cdd8fe269879ceec123fa352d204531d29c92`
  → cross-toolchain determinism proven.

---

## Build

### Prereqs (macOS)
- Homebrew `emscripten` (`brew install emscripten`) — needs `python3 >= 3.10` on PATH:
  `export PATH="/opt/homebrew/bin:$PATH"` before any `emcmake`.
- Emscripten config must point at the brew-bundled LLVM/binaryen
  (`~/.emscripten`: `LLVM_ROOT`/`BINARYEN_ROOT` → `/opt/homebrew/opt/emscripten/libexec/...`);
  run `emcc --generate-config` once and edit if the defaults are `/usr/bin`/`/usr/local`.

### Host build
```sh
cmake -S . -B build && cmake --build build -j8
./build/ko_xtch_host /abs/path/book.epub /abs/path/out.xtch
```
> Pass **absolute** paths. The engine builds an internal virtual FS rooted at
> `/.crosspoint/`, so a relative book path fails with
> `Could not find or size META-INF/container.xml` even though the file is right there.

### WASM build
```sh
export PATH="/opt/homebrew/bin:$PATH"
emcmake cmake -S . -B build-wasm -DCMAKE_BUILD_TYPE=Release
cmake --build build-wasm -j8
# → build-wasm/ko_xtch_wasm.{js,wasm}  (createKoEngine modularized factory)
```

### Node smoke test
```sh
node scripts/wasm_smoke.js /path/to/book.epub
```

---

## Layout

| Path | Purpose |
|---|---|
| `vendor-lib/` | KO-fork engine sources (Epub, GfxRenderer, EpdFont, MiniBidi, Utf8, ZipFile, expat, miniz, uzlib) — unmodified |
| `stubs/` | Host/WASM shims: `HalDisplay.h` (RAM planes), `HalStorage.h/.cpp` (in-memory FS), `Arduino.h` (Print/String/ESP shims), `Logging.h` |
| `src/ko_engine_driver.h` | Shared headless render driver: load EPUB blob → per-spine `Section::createSectionFile(spec)` → `loadPage` → 3-pass render → plane capture |
| `src/xtch_writer.h` | XTC/XTCH container + XTG/XTH page encoder (1-bit & 2-bit) |
| `src/host_main.cpp` | Host CLI (EPUB path → XTCH file) |
| `src/wasm_api.cpp` | WASM C API (see below) |
| `third_party/lz4` | BSD-2 LZ4 block codec — XTCZ (`.xtcz`) container wrapping |
| `scripts/wasm_smoke.js` | Node end-to-end smoke test |
| `test-3.epub` reference | `~/.hermes/desktop-attachments/test-3.epub` |

---

## WASM API (C exports, `EMSCRIPTEN_KEEPALIVE`)

All functions exported on the module instance (`createKoEngine()` → `Module`).

### Lifecycle
| Function | Description |
|---|---|
| `ko_init(vw, vh)` | Construct display/renderer/fonts; viewport = 480×800 minus default margins |
| `ko_close()` | Teardown |
| `ko_version()` → `const char*` | Version string (read with `UTF8ToString`) |
| `ko_error()` → `const char*` | Last error message (empty if none) |

### EPUB loading
| Function | Description |
|---|---|
| `ko_load_epub(ptr, size, virtualPath)` → int | Inject EPUB bytes into in-memory FS; returns spine count |
| `ko_spine_count()` → int | Spine count |
| `ko_get_title(buf, len)` | UTF-8 title into caller buffer |

### Korean typography knobs (each maps to a `ReaderRenderSpec`/CrossPointSetting field)
| Function | Range / meaning |
|---|---|
| `ko_set_line_compression(float)` | 1.00 TIGHT / 1.20 NORMAL (default) / 1.40 WIDE |
| `ko_set_paragraph_indent(0/1)` | First-line indent (들여쓰기) |
| `ko_set_character_wrap(0/1)` | KO default ON — break at any character |
| `ko_set_paragraph_alignment(0-4)` | 0 JUSTIFIED (default) 1 LEFT 2 CENTER 3 RIGHT 4 BOOK_STYLE |
| `ko_set_extra_paragraph_spacing(0/1)` | Paragraph gap |
| `ko_set_hyphenation(0/1)` | Inert unless character_wrap=0 |
| `ko_set_embedded_style(0/1)` | Honor book CSS (default ON) |
| `ko_set_image_rendering(0-2)` | 0 DISPLAY 1 PLACEHOLDER 2 SUPPRESS |
| `ko_set_focus_reading(0/1)` | Focus reading mode |
| `ko_set_margins(t,r,b,l)` | Logical margins; recomputes viewport |
| `ko_viewport_width/height()` | Current viewport |
| `ko_logical_width/height()` | 480 × 800 |

### Pagination & per-page plane capture
| Function | Description |
|---|---|
| `ko_build_spine(i)` → int | Paginate spine i with current spec; returns page count (−1 error) |
| `ko_render_page(p)` → int | Render page p of current spine (all 3 passes) |
| `ko_plane_ptr(kind)` → ptr | 0=BW 1=LSB(dark-grey) 2=MSB(light+dark); 48000 bytes each, physical 800×480 |
| `ko_plane_size(kind)` | 48000 |

### Full-book XTCH
| Function | Description |
|---|---|
| `ko_render_xtch()` → int | Convert whole book with current spec; returns total pages |
| `ko_xtch_ptr()` → ptr | Container bytes (after `ko_render_xtch`) |
| `ko_xtch_size()` → size_t | Container size |
| `ko_xtch_release()` | Free the module-lifetime container buffer (JS keeps its own copy) |
| `ko_xtcz_wrap()` | Re-wrap the finished container in XTZ4 (LZ4) → `.xtcz` (firmware ≥ 5.1.6) |

### Incremental export (worker-driven, whole book = one file)
| Function | Description |
|---|---|
| `ko_export_set_mode(0\|1)` | 0 = 1-bit XTC ("XTC\0", XTG pages); 1 = 2-bit XTCH ("XTCH", XTH pages) |
| `ko_export_begin()` → spines | Reset writer; export the whole book with the current spec |
| `ko_export_spine(s)` → added | Build + render one spine into the container (per-spine progress) |
| `ko_export_finish()` → pages | Finalize container (chapters assembled from the EPUB **TOC**) |

Chapters come from the EPUB table of contents (title + anchor→page resolution via the
engine's `Section::findAnchor`), capped at 100 entries like the official converter,
deduped by start page; books without a usable TOC fall back to per-spine names.

---

## Format ground truth (encoder semantics)

Ground truth = the official converter at https://epub2xtc.xteink.cn/ (openly based on
bigbag/epub-to-xtc-converter; its minified `encodeXTG`/`encodeXTH`/`buildXTCContainer`/
`compressXtczLz4` were reverse-read and cross-checked against this encoder).

- **XTH page** (22B header + 2 planes): `"XTH\0"`, w u16, h u16, 2 zero bytes, u32 dataSize,
  8B digest(=0). Planes 48000 B each. Column-major **right→left** (`colIndex = w-1-x`),
  8 vertical px/byte **MSB=topmost**.
- **Pixel value** = `(plane1bit<<1)|plane2bit`; **0=White, 1=Dark Grey, 2=Light Grey, 3=Black**
  (confirmed from `XtcReaderActivity.cpp` decode + real device file probe). Our writer's
  plane geometry was verified byte-identical against the official encoder (device-decode space).
- **Engine plane mapping** (empirically verified, 97.6% AA-edge adjacency): engine LSB pass
  marks dark grey only; MSB marks dark+light → `lsb ⊆ msb`. Encode:
  `!ink→0; lsb→1; msb-only→2; ink-no-grey→3`.
- **XTG page** (1-bit): row-major 60 B × 800 rows, MSB first, **bit 0 = BLACK** (device
  `isBlack = !bit`); official encoder writes dark pixels as 0, light as 1 — same semantics.
- **Container**: 56B header (magic `XTCH`/`XTC\0`, version **1.0** = `[1,0]`, pageCount@6,
  flags@8..0xB, currentPage@0xC = **1**, offsets: metadata@0x10 = 56, index@0x18, data@0x20,
  chapter@0x30 = **312 always**) + 256B metadata (title 128@0, author 64@0x80, publisher 32@0xC0,
  lang 16@0xE0, createTime@0xF0 = now, coverPage@0xF4 = **0**, chapterCount@0xF6) + chapters 96B
  (name 80 + startPage u16 + endPage u16 — **1-BASED on disk**, device decrements) + index
  16B/page (u64 off, u32 size, u16 w, u16 h) + page data.
- **XTCZ (LZ4)**: `"XTZ4"` + u32LE raw total + u32LE block size (4096) + per block u32LE len
  (bit 31 ⇒ stored raw, else LZ4 block) + u32LE 0 terminator. Requires device firmware ≥ 5.1.6.
  Engine wraps via vendored lz4 (BSD-2, third_party/lz4); JS worker unwraps for file-view.

Cross-checks: official site JS encoders/container builder, bigbag `docs/xtc-format-spec.md`,
on-device `XtcReaderActivity::getPixelValue`, and real `.xtch` file byte dissection all agree.

---

## Determinism

- `millis()` stub returns constant 0 → time-based refresh heuristics frozen.
- No RNG/time in render core (grep: no `rand`/`time(null)`/`esp_random`).
- Host (clang) and WASM (emscripten/clang) produce **byte-identical** XTCH files.
- Layout verified on decoded pages: 28 px line height, uniform 38 px pitch, justified right
  edge ~465-471, paragraph indents (left edge outliers at +17/+46 px).

## Known limitations

1. Image decode relies on JPEGDEC/PNGdec (portable C); very wide/slim PNGs beyond the PNGdec
   row-buffer limit (PNG_MAX_BUFFERED_PIXELS) fail to decode on-page (matches preview).
2. Fonts: KoPub Batang 14 (reference default, `getReaderFontId()`) + RIDIBatang 14 as an
   XTCKO extra; runtime `.epdfont` via
   SdFont resident preload (whole font in RAM). Embedded TTF in EPUBs is not used by the
   device engine (no @font-face support) — all text renders in the spec fontId.
3. Output is uncompressed by design for XTC/XTCH; the XTCZ (LZ4) wrapper (`ko_xtcz_wrap`)
   provides compression for device firmware ≥ 5.1.6.
4. Whole-book export holds the finished container in the wasm heap (1,981-page book ≈ 190 MB
   XTCH / 86 MB XTCZ); MAXIMUM_MEMORY is set to 2 GB to fit giant books + EPUB + caches.
5. XTCZ file-view decoding in the worker unwraps the whole container once (190 MB ≈ 1.3 s).

---

# 한국어 요약 (Korean Summary)

크로스포인트 KO 포크 렌더러(crosspoint-reader-ko, release/korean 브랜치, 1.5.0-ko.3, **MIT 라이선스**)를
호스트 바이너리와 Emscripten WASM 모듈로 포팅했다. EPUB 페이지를 기기와 동일한 3패스 그레이스케일
파이프라인(BW → LSB → MSB)으로 렌더링한 뒤, 기기 내 XTC/XTH 디코드 규약과 바이트 호환되는 XTCH
컨테이너로 패킹한다.

- 한국어 EPUB `생각에 관한 생각` → **1677페이지** 렌더링 성공
- 호스트 출력 `/tmp/think.xtch`와 WASM 출력 `/tmp/wasm-out.xtch`의 **SHA-256이 동일**
  (`b005d3bf...29c92`) → 툴체인 간 결정론(determinism) 검증 완료

## 빌드
- 호스트: `cmake -S . -B build && cmake --build build -j8` → `./build/ko_xtch_host book.epub out.xtch`
- WASM: `export PATH="/opt/homebrew/bin:$PATH"` 후
  `emcmake cmake -S . -B build-wasm` → `cmake --build build-wasm -j8`
  → `build-wasm/ko_xtch_wasm.{js,wasm}`

## 디렉터리 구성
| 경로 | 용도 |
|---|---|
| `vendor-lib/` | KO 포크 엔진 소스(수정 없음) |
| `stubs/` | 호스트/WASM 대체: `HalDisplay.h`(RAM 플레인), `HalStorage`(메모리 FS), `Arduino.h`(Print/String/ESP 셰임), `Logging.h`, 이미지 디코더 스텁 |
| `src/ko_engine_driver.h` | 공용 헤드리스 렌더 드라이버 |
| `src/xtch_writer.h` | XTCH 컨테이너 + XTH 페이지 인코더 |
| `src/host_main.cpp` | 호스트 CLI |
| `src/wasm_api.cpp` | WASM C API |
| `scripts/wasm_smoke.js` | Node 스모크 테스트 |

## WASM API 주요 함수
- `ko_init(vw,vh)`, `ko_load_epub(ptr,size,path)`, `ko_build_spine(i)`, `ko_render_page(p)`,
  `ko_plane_ptr(0|1|2)` (BW/LSB/MSB), `ko_render_xtch()`, `ko_xtch_ptr()`
- 한국어 타이포그래피 노브: `ko_set_line_compression`(1.00/1.20/1.40),
  `ko_set_paragraph_indent`, `ko_set_character_wrap`, `ko_set_paragraph_alignment`(0~4),
  `ko_set_extra_paragraph_spacing`, `ko_set_hyphenation`, `ko_set_embedded_style`,
  `ko_set_image_rendering`, `ko_set_focus_reading`, `ko_set_margins`

## 포맷 근거 (인코더 의미론)
- XTH 페이지: 22B 헤더 + 플레인 2개. 값 = `(plane1<<1)|plane2`, **0=흰색, 1=진한 회색,
  2=연한 회색, 3=검정**. 컬럼은 **오른쪽→왼쪽**, 바이트당 세로 8픽셀 **MSB=위**.
- 엔진 플레인 매핑(실증 확인, AA 경계 97.6%): LSB 패스=진한 회색만, MSB=진한+연한
  → `lsb ⊆ msb`. 인코딩: `!잉크→0; lsb→1; msb만→2; 잉크&무회색→3`.
- 컨테이너: 56B 헤더(`XTCH`, pageCount@6, currentPage@0xC=1) + 256B 메타데이터 + 96B 챕터
  + 16B/페이지 인덱스 + 페이지 데이터. bigbag 스펙 + 기기 내 `XtcReaderActivity` 디코드 +
  실제 `.xtch` 파일 바이트 분석 모두 일치.

## 알려진 제한 (v0)
1. 이미지 디코딩 비활성(스텁) — 텍스트 우선. PNGdec/JPEGDEC(휴대용 C) 추후 교체 가능.
2. 챕터명은 "Chapter N"(스파인 기준) — EPUB TOC 제목 매핑은 이후 개선.
3. 폰트: 내장 KoPub Batang 14 + Pretendard 10. EPUB 내장 TTF는 기기 엔진이 미지원(@font-face 없음).
4. XTZ4/LZ4 래퍼 미적용(현재는 순수 XTCH 컨테이너 출력) — 간단한 추가 작업.
5. 12MB EPUB → 161MB 출력은 무압축 특성상 정상(XTC/XTCH는 자체 압축 없음).
