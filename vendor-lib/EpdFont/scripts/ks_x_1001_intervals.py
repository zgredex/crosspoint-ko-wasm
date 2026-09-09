#!/usr/bin/env python3
"""Print fontconvert.py --additional-intervals args for the KS X 1001 Hangul set.

The UI font (Pretendard) used to carry all 11,172 precomposed Hangul syllables,
which cost 1,070 KB of flash on its own. Restricting it to the 2,350 syllables of
KS X 1001 (the classic wansung set) drops that to 226 KB and is what keeps the
release under the 6.25 MB stock OTA app partition -- see
docs/firmware-size-budget.md.

KS X 1001's Hangul block is the EUC-KR two-byte area where BOTH bytes fall in
0xA1..0xFE. Python's euc_kr codec also accepts the 8,822 UHC/CP949 extension
syllables, so encodability alone is not the test; the byte range is.

The syllables are scattered through U+AC00..U+D7A3 (Unicode orders them by
jamo composition, not by usage), so they collapse into ~1,600 short runs rather
than one range. That costs ~20 KB of interval table, still a large net win.

Usage:
    python3 ks_x_1001_intervals.py            # one "--additional-intervals X,Y" per line
    python3 ks_x_1001_intervals.py --count    # just report the totals
"""

import sys

HANGUL_FIRST = 0xAC00
HANGUL_LAST = 0xD7A3


def ks_x_1001_syllables() -> list[int]:
    """Code points of the 2,350 KS X 1001 precomposed Hangul syllables."""
    out = []
    for cp in range(HANGUL_FIRST, HANGUL_LAST + 1):
        try:
            encoded = chr(cp).encode("euc_kr")
        except UnicodeEncodeError:
            continue
        # Reject the UHC/CP949 extension area, which uses lead or trail bytes
        # outside 0xA1..0xFE. Only the KS X 1001 body has both in range.
        if len(encoded) == 2 and 0xA1 <= encoded[0] <= 0xFE and 0xA1 <= encoded[1] <= 0xFE:
            out.append(cp)
    return out


def runs(codepoints: list[int]) -> list[tuple[int, int]]:
    """Collapse a sorted code point list into (first, last) inclusive runs."""
    if not codepoints:
        return []
    out = []
    start = prev = codepoints[0]
    for cp in codepoints[1:]:
        if cp == prev + 1:
            prev = cp
        else:
            out.append((start, prev))
            start = prev = cp
    out.append((start, prev))
    return out


def main() -> int:
    syllables = ks_x_1001_syllables()
    intervals = runs(syllables)

    if "--count" in sys.argv:
        print(f"syllables: {len(syllables)}")
        print(f"intervals: {len(intervals)}")
        return 0

    for first, last in intervals:
        print(f"--additional-intervals 0x{first:04X},0x{last:04X}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
