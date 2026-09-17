#!/usr/bin/env python3
"""PNG stage 2, atomic: struct + constants + PNGdec.h removal + decode swap.

Design notes:
  * The ~100-line region between the heap gate and the decode call is NEVER rewritten -
    only specific constructs are deleted or swapped, so the context/scale setup that the
    callback depends on is untouched.
  * Everything is shape-anchored with brace counting, and every deletion asserts a
    plausible span length, so a mis-anchored match aborts instead of eating code.
  * Must be atomic: PNGdec's iPixelType is an enum type taken BY TYPE by
    convertLineToGray, so our constants cannot coexist with PNGdec.h. The include goes
    away in the same edit to avoid the enumerator clash found last pass.
"""
import re
import shutil
import sys

BASE = '/Users/patryk/krxtc/ko-wasm/vendor-lib/Epub/Epub/converters/'
P = BASE + 'PngToFramebufferConverter.cpp'
BAND = BASE + 'BandBlock.h'
for f in (P, BAND):
    shutil.copy(f, '/tmp/ab/' + f.split('/')[-1] + '.pre_2b')

lines = open(P).read().split('\n')
log = []


def find(pred, what, start=0):
    for i in range(start, len(lines)):
        if pred(lines[i]):
            return i
    print(f'ABORT: anchor not found: {what}')
    sys.exit(1)


def block_end(i, what, maxspan=200):
    """Index of the line closing the block opened at line i (brace counted)."""
    depth = 0
    for k in range(i, min(len(lines), i + maxspan)):
        code = re.sub(r'"(?:[^"\\]|\\.)*"', '""', lines[k])
        code = re.sub(r"'(?:[^'\\]|\\.)*'", "''", code)
        depth += code.count('{') - code.count('}')
        if depth == 0 and k > i:
            return k
    print(f'ABORT: no closing brace for {what} within {maxspan} lines')
    sys.exit(1)


def delete_from(i, what, maxspan=200):
    end = block_end(i, what, maxspan)
    span = end - i + 1
    if span > maxspan:
        print(f'ABORT: span {span} too large for {what}')
        sys.exit(1)
    del lines[i:end + 1]
    log.append(f'deleted {what} ({span} lines)')


# --- 1. heap gates (device: contiguous-heap budget for the decoder object) ----
guard = 0
while True:
    hit = next((i for i, l in enumerate(lines) if 'ESP.getMaxAllocHeap' in l), -1)
    if hit < 0:
        break
    # walk up to the enclosing if( header, then delete the whole block
    j = hit
    while j > 0 and not lines[j].strip().startswith('if ('):
        j -= 1
    delete_from(j, 'heap gate', maxspan=12)
    guard += 1
log.append(f'heap gates removed: {guard}')

# --- 2. the decoder object allocation ----------------------------------------
i = find(lambda l: 'unique_ptr<PNG> png' in l, 'decoder alloc')
delete_from(i, 'PNGdec allocation', maxspan=12)

# --- 3. the open() call + its ScopedCleanup ----------------------------------
i = find(lambda l: 'png->open(' in l, 'png->open')
j = find(lambda l: 'png->close(); }' in l, 'cleanup', i)
if j - i > 6:
    print('ABORT: cleanup anchor too far from open')
    sys.exit(1)
del lines[i:j + 1]
log.append(f'deleted png->open + cleanup ({j - i + 1} lines)')

# --- 4. PNG_SUCCESS -> plain 0 (PNGdec constant) -----------------------------
for k, l in enumerate(lines):
    if 'PNG_SUCCESS' in l:
        lines[k] = l.replace('PNG_SUCCESS', '0')
log.append('PNG_SUCCESS references normalised to 0')

# --- 5. overflow guard (PNGdec two-scanline internal buffer) -----------------
i = next((k for k, l in enumerate(lines)
          if 'Aborting decode to avoid PNGdec internal buffer overflow' in l), -1)
if i >= 0:
    j = i
    while j > 0 and not lines[j].strip().startswith('if ('):
        j -= 1
    delete_from(j, 'PNGdec overflow guard', maxspan=10)

s = '\n'.join(lines)

# --- 6. the file callbacks + global pointer (dead once decode is libpng) -----
lines = s.split('\n')
i = next((k for k, l in enumerate(lines) if 'pngOpenWithHandle(const char*' in l), -1)
if i >= 0:
    j = next((k for k, l in enumerate(lines) if 'pngSeekWithHandle' in l), -1)
    if j > i:
        end = block_end(j, 'pngSeekWithHandle', maxspan=40)
        span = end - i + 1
        if span > 60:
            print(f'ABORT: callback span {span} implausible')
            sys.exit(1)
        del lines[i:end + 1]
        log.append(f'deleted PNGdec file callbacks ({span} lines)')
s = '\n'.join(lines)

# --- 7. PNGdec.h out, BandBlock.h in -----------------------------------------
s = s.replace('#include <PNGdec.h>  // remaining: the scanline decode path\n', '')
s = s.replace('#include <PNGdec.h>\n', '')
if '#include "BandBlock.h"' not in s:
    s = s.replace('#include "DirectPixelWriter.h"', '#include "BandBlock.h"\n#include "DirectPixelWriter.h"', 1)

# --- 8. callback takes our block ---------------------------------------------
n = s.count('PNGDRAW')
s = s.replace('PNGDRAW', 'PngBlock')
log.append(f'callback signature: {n} PNGDRAW -> PngBlock')

# --- 9. dimensions from libpng, and the decode itself ------------------------
old_validate = 'if (!validateImageDimensions(png->getWidth(), png->getHeight(), "PNG")) {'
if s.count(old_validate) != 1:
    print('ABORT: validate call not found verbatim')
    sys.exit(1)
new_validate = '''  // Whole-file read, then two short libpng sessions: one for the header (dimensions),
  // one for the pixel rows. The buffer stays in scope for both, so there is no decoder
  // object to size against a heap budget and no streaming state to leak on early return.
  std::vector<uint8_t> file;
  if (!readWholeFilePng(imagePath, file)) return false;

  const int pngW = pngReadDimensions(file, imagePath);
  const int pngH = pngReadHeight(file, imagePath);

  if (!validateImageDimensions(pngW, pngH, "PNG")) {'''
s = s.replace(old_validate, new_validate, 1)

s = s.replace('ctx.srcWidth = png->getWidth();', 'ctx.srcWidth = pngW;')
s = s.replace('ctx.srcHeight = png->getHeight();', 'ctx.srcHeight = pngH;')

old_decode = 'rc = png->decode(&ctx, 0);'
if s.count(old_decode) != 1:
    print('ABORT: decode call not found verbatim')
    sys.exit(1)
new_decode = '''  // libpng row-by-row, fed to the UNCHANGED callback: it is already per-scanline
  // (pDraw->y) and does all of its own up/downscale row mapping.
  {
    PngDecodeSession session;
    if (!session.begin(file, imagePath, pngW, pngH)) {
      ctx.grayLineBuffer = nullptr;
      return false;
    }
    std::vector<uint8_t> rowBuf(static_cast<size_t>(pngW) * (session.hasAlpha() ? 2 : 1));
    for (int pass = 0; pass < session.passes(); pass++) {
      for (int y = 0; y < pngH; y++) {
        session.readRow(rowBuf.data());
        if (pass != session.passes() - 1) continue;
        PngBlock block{};
        block.pUser = &ctx;
        block.y = y;
        block.iBpp = 8;
        block.iHasAlpha = session.hasAlpha() ? 1 : 0;
        block.iPixelType = session.hasAlpha() ? PNG_PIXEL_GRAY_ALPHA : PNG_PIXEL_GRAYSCALE;
        block.pPalette = nullptr;
        block.pPixels = rowBuf.data();
        pngDrawCallback(&block);
        if (ctx.error) break;
      }
    }
    rc = ctx.error ? 1 : 0;
  }'''
s = s.replace(old_decode, new_decode, 1)

open(P, 'w').write(s)
print('stage 2 applied:')
for l in log:
    print('  -', l)
left = [l.strip()[:90] for l in s.split('\n') if 'PNGdec' in l or 'png->' in l or 'PNG_SUCCESS' in l]
print('remaining PNGdec references:', len(left))
for l in left[:6]:
    print('   ', l)
