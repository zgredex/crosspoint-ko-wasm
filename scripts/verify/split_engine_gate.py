#!/usr/bin/env python3
"""Split-engine gate: the invariants that keep the preview engine and the export engine from
disagreeing about what a book should look like.

WHY THIS EXISTS. Splitting the engines introduced a class of bug that has nothing to do with
rendering: state that ONE engine owns, or that the PAGE believes about an engine, silently going
stale. Three were real and measured, and each assertion below maps to one of them:

  1. the custom font lived only in the preview worker, so an export was built from KoPub while the
     spec said font:"custom" — a successful export of the wrong thing;
  2. a failed font apply still COMMITTED the spec, so every later applySpecIfChanged() short-circuited
     on "no change" and kept rendering the wrong face;
  3. killing the export engine left the page believing a warm container still existed in it, so the
     next export handed the user an error instead of a file.

Assertions are checked INSIDE named functions, not against the whole file. The first version of this
gate used plain "does this text appear anywhere" greps, and it happily passed with bug 3 re-typed into
the source, because `warmSpecKey = null` also appears in two other places. A pattern that matches
somewhere else is not an invariant.

Static checks cannot prove a program correct. They can refuse to let a specific, measured regression
come back — which is all this claims. The behavioural half, which needs a browser, is
scripts/verify/split_engine_probe.js.

usage: python3 scripts/verify/split_engine_gate.py
"""
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
APP = os.path.join(ROOT, 'web', 'app.js')
WORKER = os.path.join(ROOT, 'web', 'ko.worker.js')

fails = []


def body_of(src, name):
    """The body of `function name(...)` / `async function name(...)` by brace matching.

    Deliberately naive: it finds the first brace after the signature and balances. Good enough for
    these files, and it fails loudly (raises) rather than silently returning nothing when the
    function is renamed — a gate that silently stops checking is worse than no gate.
    """
    pat = re.compile(r'(?:async\s+)?function\s+' + re.escape(name) + r'\s*\(')
    m = pat.search(src)
    if not m:
        raise LookupError(f'no function named {name!r} — the gate needs updating')
    i = src.index('{', m.start())
    depth = 0
    for j in range(i, len(src)):
        if src[j] == '{':
            depth += 1
        elif src[j] == '}':
            depth -= 1
            if depth == 0:
                return src[i:j + 1]
    raise LookupError(f'unbalanced braces after {name!r}')


def in_function(src, fn, needle, why, expect=True):
    try:
        body = body_of(src, fn)
    except LookupError as exc:
        fails.append(f'{why}: {exc}')
        return False
    found = needle in body
    if found != expect:
        verb = 'missing from' if expect else 'present in'
        fails.append(f'{why}: {needle!r} is {verb} {fn}()')
        return False
    print(f'  {why:<62} PASS')
    return True


def ordered(src, fn, needles, why):
    """Assert the needles appear in `fn` in the given order — for transactional shapes."""
    try:
        body = body_of(src, fn)
    except LookupError as exc:
        fails.append(f'{why}: {exc}')
        return False
    at = -1
    for n in needles:
        k = body.find(n, at + 1)
        if k < 0:
            fails.append(f'{why}: {n!r} does not follow the previous step in {fn}()')
            return False
        at = k
    print(f'  {why:<62} PASS')
    return True


def main():
    app = open(APP, encoding='utf-8').read()
    worker = open(WORKER, encoding='utf-8').read()

    print('split-engine gate')
    print()
    print('-- the custom font must reach BOTH engines (bug 1) --')
    in_function(app, 'applyCustomFont', 'customFontAsset = {', 'the page retains a canonical .epdfont')
    in_function(app, 'applyCustomFont', "exportCall('loadFont'", 'a converted font is pushed to the export engine')
    in_function(app, 'applyCustomFont', 'retained.slice(0)', 'each engine gets a disposable copy')
    in_function(app, 'loadExportEngine', 'if (customFontAsset)', 'a new export engine restores that font')

    print()
    print('-- a requested font that cannot be applied must fail closed (bug 2) --')
    in_function(worker, 'applySpec', 'const fontOk = applyFont(currentSpec.font)', 'applySpec checks the result')
    in_function(worker, 'applySpec', 'is not available in this engine', 'applySpec throws instead of continuing')
    in_function(worker, 'applySpec', 'currentSpec = before;', 'a failed apply restores the previous spec')
    # the rollback is dead code unless the commit is INSIDE the same try, before the catch
    ordered(worker, 'applySpec',
            ['COUNTERS.specApplies += 1;', 'try {', 'return { layout: lk', 'currentSpec = before;'],
            'the spec commit is transactional')

    print()
    print('-- destroying an engine destroys what the page believed about it (bug 3) --')
    in_function(app, 'killExportEngine', 'warmSpecKey = null;', "the page's warm belief is reset on a kill")
    in_function(app, 'killExportEngine', 'exportLoaded = null;', 'the engine handle is cleared')
    in_function(app, 'killExportEngine', 'exportPending.clear()', 'pending calls are rejected, not orphaned')
    in_function(app, 'loadBook', 'currentBookBlob = blob;', 'the page retains the book itself')
    in_function(app, 'exportAndDownload', 'currentBookBlob || currentBookFile', 'recovery rebuilds from it')

    print()
    print('-- work the split made unnecessary must stay gone --')
    for needle, why in [('engine was invalidated by the warm pass', 'no preview refresh after a warm download')]:
        ok = needle not in app
        print(f'  {why:<62} {"PASS" if ok else "FAIL"}')
        if not ok:
            fails.append(f'{why}: found {needle!r}')
    for needle, why in [('WARM_RAW_LIMIT', 'no 64 MB cap'), ('preflightWarmSize', 'no whole-book preflight'),
                        ('INTERACTIVE_COOLOFF_MS =', 'no interactive cooldown')]:
        ok = needle not in worker
        print(f'  {why:<62} {"PASS" if ok else "FAIL"}')
        if not ok:
            fails.append(f'{why}: found {needle!r}')

    print()
    print('-- a reused pool must not hold a previous font (finding 1) --')
    # The pool key has to cover everything an engine keeps as state across calls: the book and the font
    # bytes. Book identity alone let a regenerated face keep exporting the old one.
    in_function(app, 'poolIdentity', 'customFontAsset ? customFontAsset.generation : 0',
                'the pool identity includes the font generation')
    in_function(app, 'ensurePool', 'poolIdentity(want)', 'ensurePool keys on that identity')
    in_function(app, 'applyCustomFont', 'killPool()', 'a new canonical face destroys the pool')
    # and it must be destroyed rather than hot-synced: N speculative engines are cheaper to rebuild
    in_function(app, 'spawnPoolEngine', 'customFontAsset.bytes.slice(0)',
                'a rebuilt pool restores the current font')

    print()
    print("-- the export engine must not race the reader's first page --")
    in_function(app, 'loadBook', 'requestIdleCallback(prepExport', 'engine preparation waits for idle time')

    print()
    if not fails:
        print('PASS — every split-engine invariant holds')
        print('Behavioural half (needs a browser): scripts/verify/split_engine_probe.js')
        return 0
    print(f'FAIL — {len(fails)} invariant(s) violated')
    for f in fails:
        print(f'  - {f}')
    return 1


if __name__ == '__main__':
    sys.exit(main())
