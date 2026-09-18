#!/usr/bin/env python3
"""Pin the reference engine and keep it pinned.

WHY: "our layout matches CrossPoint-KO" is only a claim until it names the revision and
the exact files. The port vendors the reference's engine under vendor-lib/, so the
contract is a *source* contract — and the reference moves. This tool writes that contract
to oracle/crosspoint-ko.pin.json and, on --check, re-derives it from GitHub.

Two levels, because whole-file hashes alone are the wrong instrument:

  byte-identical   every engine file the reference has and the port did not touch.
                   Compared as Git blob hashes, so a moved file, a line-ending change or
                   a whitespace edit all fail. 155 of 168 today.
  function-level   files that MUST diverge (device RAM guards, host caches, the port's
                   render plumbing). A whole-file hash says nothing there, so the file is
                   parsed and its top-level function bodies are compared one by one. Every
                   divergence has to be a member of the declared set, and every function in
                   LAYOUT_CRITICAL has to be identical — that is what makes "the divergence
                   did not touch typography" a check and not a hope.

CONTROL (the same discipline the rest of this tree uses): a fetch that fails, a tree that
comes back empty, or a comparer that has quietly stopped comparing must all fail loudly.
--check asserts the fetched tree is non-empty, that a known file is present, and that the
comparer reports differences for a pair that is known to differ. Without that, a broken
fetch reads as a pass.

usage:
  oracle_pin.py --write [--tree-json PATH]   (re)write oracle/crosspoint-ko.pin.json
  oracle_pin.py --check [--tree-json PATH]   verify the working tree against the pin
"""
import json
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO_ROOT = os.path.dirname(os.path.dirname(HERE))
PIN_PATH = os.path.join(REPO_ROOT, 'oracle', 'crosspoint-ko.pin.json')
VENDOR = os.path.join(REPO_ROOT, 'vendor-lib')

ENGINE_DIRS = ('Epub', 'EpdFont', 'GfxRenderer')
REF_PREFIX = 'lib/'
# Port-only trees: engine files under these prefixes are the port's own additions and
# have no reference counterpart. Keeping the list explicit means a *new* file appearing
# upstream cannot be missed just because it is not vendored.
PORT_ONLY = ('Epub/Epub/converters/PngToFramebufferConverter.cpp',
             'Epub/Epub/converters/JpegToFramebufferConverter.cpp')

# Functions whose behaviour IS the typography/geometry contract. Every one of them must
# come out byte-identical after comment/whitespace normalisation, in every divergent file
# that defines it. A function listed here that has vanished from the reference is an
# error, not a pass — silence is how a renamed function escapes notice.
LAYOUT_CRITICAL = {
    'GfxRenderer/GfxRenderer.cpp': [
        'getTextWidth', 'getTextAdvanceX', 'getSpaceWidth', 'getSpaceAdvance',
        'drawTextImpl', 'drawText', 'getLineHeight', 'getFontAscenderSize',
        'getOrientedViewableTRBL', 'drawPixel', 'clearScreen',
        'copyGrayscaleLsbBuffers', 'copyGrayscaleMsbBuffers',
    ],
}

# Wrapped-signature functions that MUST be visible to the parser. These are the ones the
# first version of this tool silently dropped (the declarator's first line was not part of
# the extracted text), which would have let the gate pass while checking nothing.
PROBE_LAYOUT_FUNCTIONS = ['getTextWidth', 'drawTextImpl', 'getSpaceAdvance']

sys.path.insert(0, HERE)
from oracle_functions import functions, normalise  # noqa: E402


def sh(*args, **kw):
    return subprocess.run(args, capture_output=True, text=True, **kw)


def blob_sha(path):
    """Git blob hash of a working-tree file: the identity the reference tree reports."""
    r = sh('git', 'hash-object', path)
    if r.returncode != 0:
        raise RuntimeError(f'git hash-object failed for {path}: {r.stderr.strip()}')
    return r.stdout.strip()


def fetch_tree(repo, commit, cache):
    if cache and os.path.exists(cache):
        with open(cache, encoding='utf-8') as fh:
            tree = json.load(fh)
    else:
        r = sh('gh', 'api', f'repos/{repo}/git/trees/{commit}?recursive=1')
        if r.returncode != 0:
            raise RuntimeError(f'gh api tree failed: {r.stderr.strip()}')
        tree = json.loads(r.stdout)
        if cache:
            with open(cache, 'w', encoding='utf-8') as fh:
                json.dump(tree, fh)
    # CONTROL: an empty or truncated tree must never look like agreement.
    if tree.get('truncated'):
        raise RuntimeError('fetched tree is truncated — refusing to compare against a partial tree')
    blobs = {e['path']: e['sha'] for e in tree.get('tree', []) if e.get('type') == 'blob'}
    if 'lib/Epub/Epub/Section.cpp' not in blobs:
        raise RuntimeError('fetched tree lacks lib/Epub/Epub/Section.cpp — fetch is not trustworthy')
    return blobs


def engine_blobs(blobs):
    return {p: s for p, s in blobs.items() if p.startswith(REF_PREFIX) and
            p[len(REF_PREFIX):].split('/')[0] in ENGINE_DIRS}


def port_blob_shas(paths):
    out = {}
    for p in paths:
        local = os.path.join(VENDOR, p[len(REF_PREFIX):])
        out[p] = blob_sha(local) if os.path.exists(local) else None
    return out


def repo_file(path):
    return os.path.join(REPO_ROOT, path)


def reference_source(repo, commit, ref_path, cache_dir):
    """Working copy of one reference file. Uses the extracted tarball when present,
    otherwise fetches the blob — never the port's copy, which would make the comparison
    a tautology."""
    hits = []
    for base in (cache_dir, os.environ.get('KO_ORACLE_SRC', '')):
        if base and os.path.isdir(base):
            for root, _dirs, files in os.walk(base):
                if os.path.basename(ref_path) in files and root.endswith(os.path.dirname(ref_path)):
                    hits.append(os.path.join(root, os.path.basename(ref_path)))
    if hits:
        return open(hits[0], encoding='utf-8', errors='replace').read()
    r = sh('gh', 'api', f'repos/{repo}/contents/{ref_path}?ref={commit}', '--jq', '.content')
    if r.returncode != 0:
        raise RuntimeError(f'gh api contents failed for {ref_path}: {r.stderr.strip()}')
    import base64
    return base64.b64decode(r.stdout).decode('utf-8', 'replace')


def line_changes(ref_src, port_src):
    """Which reference LINES survive into the port, whitespace-insensitively.

    Function-body comparison cannot see a header: an inline member defined at class scope
    is not extracted by the parser, yet `setRenderMode` lives exactly there. Line identity
    catches it without a parser: if every reference line is still present in the port, the
    divergence can only be additions. Anything else has to be declared as 'amended' and
    survive the function-level audit.
    """
    def norm_lines(src):
        return [ln.strip() for ln in src.split('\n') if ln.strip()]
    ref_lines = norm_lines(ref_src)
    port_multiset = {}
    for ln in norm_lines(port_src):
        port_multiset[ln] = port_multiset.get(ln, 0) + 1
    removed = []
    for ln in ref_lines:
        if port_multiset.get(ln, 0) > 0:
            port_multiset[ln] -= 1
        else:
            removed.append(ln)
    added = sum(port_multiset.values())
    return added, removed


def classify(repo, commit, blobs, cache_dir):
    """Return (identical, divergent, missing) for the reference's engine files."""
    identical, divergent, missing = {}, {}, {}
    for ref_path, sha in sorted(engine_blobs(blobs).items()):
        rel = ref_path[len(REF_PREFIX):]
        local = os.path.join(VENDOR, rel)
        if not os.path.exists(local):
            missing[ref_path] = sha
            continue
        if blob_sha(local) == sha:
            identical[ref_path] = sha
            continue
        ref_src = reference_source(repo, commit, ref_path, cache_dir)
        port_src = open(local, encoding='utf-8', errors='replace').read()
        rf = dict(functions(ref_src))
        pf = dict(functions(port_src))
        changed = sorted(n for n in rf if n in pf and normalise(rf[n]) != normalise(pf[n]))
        only_ref = sorted(set(rf) - set(pf))
        only_port = sorted(set(pf) - set(rf))
        added, removed = line_changes(ref_src, port_src)
        entry = {'blob': sha, 'port_path': f'vendor-lib/{rel}',
                 'divergence_kind': 'additive' if not removed else 'amended',
                 'added_lines': added, 'removed_lines': len(removed),
                 'changed_functions': changed, 'only_in_reference': only_ref,
                 'only_in_port': only_port,
                 'reference_function_count': len(rf), 'port_function_count': len(pf)}
        if removed:
            # Keep the first few removed reference lines: they are the entire reason a file
            # is 'amended', and the next reader of the pin should not have to re-diff to see
            # what was taken out.
            entry['removed_line_sample'] = removed[:8]
        lc = LAYOUT_CRITICAL.get(rel)
        if lc:
            entry['layout_critical_checked'] = lc
            entry['layout_critical_identical'] = sorted(
                n for n in lc if n in rf and n in pf and normalise(rf[n]) == normalise(pf[n]))
            entry['layout_critical_missing'] = sorted(n for n in lc if n not in rf or n not in pf)
        divergent[ref_path] = entry
    return identical, divergent, missing


def write_pin(repo, branch, commit, blobs, out_path):
    identical, divergent, missing = classify(repo, commit, blobs, None)
    pin = {
        'repo': repo, 'branch': branch, 'commit': commit,
        'role': ('Normative typography/layout oracle. Text rhythm, spacing, break decisions, '
                 'x/y placement, paragraph spacing, line heights and pagination are defined by '
                 'this revision — not by "Korean-looking output".'),
        'engine_scope': [f'{REF_PREFIX}{d}/' for d in ENGINE_DIRS],
        'levels': {
            'byte_identical': 'every engine file the port did not touch, compared as Git blob hashes',
            'function_level': 'files that must diverge; top-level bodies compared one by one, '
                              'with LAYOUT_CRITICAL required to be identical',
        },
        'port_only': list(PORT_ONLY),
        'identical': identical,
        'divergent': divergent,
        'missing_from_port': missing,
        'layout_critical': LAYOUT_CRITICAL,
        'counts': {'identical': len(identical), 'divergent': len(divergent),
                   'missing_from_port': len(missing), 'reference_engine_files': len(identical) + len(divergent) + len(missing)},
    }
    os.makedirs(os.path.dirname(out_path), exist_ok=True)
    with open(out_path, 'w', encoding='utf-8') as fh:
        json.dump(pin, fh, indent=2, sort_keys=False)
        fh.write('\n')
    return pin


def check_pin(repo, commit, blobs, cache_dir):
    with open(PIN_PATH, encoding='utf-8') as fh:
        pin = json.load(fh)
    failures, notes = [], []

    if pin['commit'] != commit:
        failures.append(f"pin says commit {pin['commit']}, live {commit} on {pin['branch']}")
    else:
        notes.append(f"commit matches the pin: {commit} ({pin['branch']})")

    # Analysis inputs reused by the controls below. rf_all is the reference's
    # GfxRenderer.cpp as parsed right now, which is the file the LAYOUT_CRITICAL claim
    # rests on.
    a_fns = dict(functions(open(repo_file('vendor-lib/Epub/Epub/Section.cpp'), encoding='utf-8', errors='replace').read()))
    b_fns = dict(functions(open(repo_file('vendor-lib/Epub/Epub/ParsedText.cpp'), encoding='utf-8', errors='replace').read()))
    a_names, b_names = set(a_fns), set(b_fns)
    rf_all = dict(functions(reference_source(repo, commit, 'lib/GfxRenderer/GfxRenderer.cpp', cache_dir)))

    # CONTROL 1a: the comparison reads two DIFFERENT files. A path/caching bug that made
    # every "difference" comparison compare a file with itself would otherwise show up as
    # a clean pass everywhere.
    if open(repo_file('vendor-lib/Epub/Epub/Section.cpp'), encoding='utf-8', errors='replace').read() == \
       open(repo_file('vendor-lib/Epub/Epub/ParsedText.cpp'), encoding='utf-8', errors='replace').read():
        failures.append('CONTROL: Section.cpp and ParsedText.cpp read as the same bytes — the '
                        'comparison inputs are suspect')
    else:
        notes.append('control: distinct comparison inputs (Section.cpp vs ParsedText.cpp differ on disk)')

    # CONTROL 1b: the comparer still detects differences, asserted against ground truth.
    # A function that is KNOWN to differ between the reference and the port
    # (ImageBlock::needsDecode lost its hasValidCache() term). If the parser stops seeing
    # wrapped signatures or the comparer stops comparing, this goes quiet — and a gate that
    # cannot detect a known difference certifies nothing.
    ref_img = reference_source(repo, commit, 'lib/Epub/Epub/blocks/ImageBlock.cpp', cache_dir)
    port_img = open(repo_file('vendor-lib/Epub/Epub/blocks/ImageBlock.cpp'), encoding='utf-8', errors='replace').read()
    rimg = dict(functions(ref_img))
    pimg = dict(functions(port_img))
    if 'needsDecode' not in rimg or 'needsDecode' not in pimg:
        failures.append('CONTROL: parser cannot see ImageBlock::needsDecode — wrapped or long '
                        'signatures are being dropped, so LAYOUT_CRITICAL results are unusable')
    elif normalise(rimg['needsDecode']) == normalise(pimg['needsDecode']):
        failures.append('CONTROL: ImageBlock::needsDecode reads as identical to the reference but is '
                        'not — the comparer is not looking at the right bytes')
    else:
        notes.append('control: comparer flags a known reference/port difference (ImageBlock::needsDecode)')

    if all(n in rf_all for n in PROBE_LAYOUT_FUNCTIONS):
        notes.append('control: wrapped-signature functions are visible to the parser '
                     f'({", ".join(PROBE_LAYOUT_FUNCTIONS)})')
    else:
        missing_probe = [n for n in PROBE_LAYOUT_FUNCTIONS if n not in rf_all]
        failures.append(f'CONTROL: parser cannot see {missing_probe} in the reference GfxRenderer.cpp — '
                        'the layout-critical gate would pass vacuously')

    # CONTROL 2: no silent loss of files from the fetched tree.
    live = engine_blobs(blobs)
    if len(live) < pin['counts']['reference_engine_files']:
        failures.append(f"fetched tree has {len(live)} engine files, pin expected "
                        f"{pin['counts']['reference_engine_files']} — files may have moved out of scope")
    else:
        notes.append(f'fetched tree: {len(live)} engine files under {", ".join(pin["engine_scope"])}')

    # 1. byte-identical set must still be byte-identical, and still be in the reference.
    for ref_path, sha in sorted(pin['identical'].items()):
        if ref_path not in blobs:
            failures.append(f'{ref_path}: gone from the reference tree')
            continue
        if blobs[ref_path] != sha:
            failures.append(f'{ref_path}: reference blob changed {sha[:12]} -> {blobs[ref_path][:12]}')
            continue
        local = os.path.join(VENDOR, ref_path[len(REF_PREFIX):])
        if not os.path.exists(local):
            failures.append(f'{ref_path}: missing from vendor-lib')
            continue
        h = blob_sha(local)
        if h != sha:
            failures.append(f'{ref_path}: port copy drifted from the reference (local {h[:12]} vs {sha[:12]})')
    notes.append(f'byte-identical: {len(pin["identical"])} files verified')

    # 2. divergent set: the reference side must not have moved, and the divergence must be
    #    exactly the declared one, with every layout-critical function identical.
    for ref_path, entry in sorted(pin['divergent'].items()):
        if ref_path not in blobs:
            failures.append(f'{ref_path}: gone from the reference tree')
            continue
        if blobs[ref_path] != entry['blob']:
            failures.append(f'{ref_path}: reference-side blob changed {entry["blob"][:12]} -> '
                            f'{blobs[ref_path][:12]} — the divergence must be re-audited')
            continue
        local = repo_file(entry['port_path'])
        if not os.path.exists(local):
            failures.append(f'{entry["port_path"]}: missing')
            continue
        ref_src = reference_source(repo, commit, ref_path, cache_dir)
        rf = dict(functions(ref_src))
        pf = dict(functions(open(local, encoding='utf-8', errors='replace').read()))
        changed = sorted(n for n in rf if n in pf and normalise(rf[n]) != normalise(pf[n]))
        if changed != entry['changed_functions']:
            failures.append(f'{ref_path}: function divergence changed\n'
                            f'      pin: {entry["changed_functions"]}\n'
                            f'      now: {changed}')
        # The additive invariant: every reference line must still be present. It is the only
        # thing that covers inline definitions in headers, which no function extractor sees.
        added, removed = line_changes(ref_src, open(local, encoding='utf-8', errors='replace').read())
        kind = 'additive' if not removed else 'amended'
        if kind != entry['divergence_kind']:
            failures.append(f'{ref_path}: divergence kind changed {entry["divergence_kind"]} -> {kind} '
                            f'({len(removed)} reference lines no longer present, e.g. '
                            f'{removed[0].strip()[:70] if removed else "-"})')
        elif kind == 'additive':
            if added < entry['added_lines']:
                failures.append(f'{ref_path}: was additive with {entry["added_lines"]} added lines, '
                                f'now {added} — a port-only block disappeared')
        elif len(removed) != entry['removed_lines']:
            failures.append(f'{ref_path}: {len(removed)} reference lines removed, pin recorded '
                            f'{entry["removed_lines"]} — the amendment must be re-audited')
        for name, status in (('only_in_reference', sorted(set(rf) - set(pf))),
                             ('only_in_port', sorted(set(pf) - set(rf)))):
            if status != entry[name]:
                failures.append(f'{ref_path}: {name} changed\n      pin: {entry[name]}\n      now: {status}')
        lc = LAYOUT_CRITICAL.get(ref_path[len(REF_PREFIX):])
        if lc:
            for name in lc:
                if name not in rf:
                    failures.append(f'{ref_path}: layout-critical function {name}() is gone from the reference')
                elif name not in pf:
                    failures.append(f'{ref_path}: layout-critical function {name}() is gone from the port')
                elif normalise(rf[name]) != normalise(pf[name]):
                    failures.append(f'{ref_path}: LAYOUT-CRITICAL {name}() differs from the reference')
    notes.append(f'divergent: {len(pin["divergent"])} files re-audited at function level')

    # 3. files the reference has and the port does not: keep the list honest.
    live_missing = {p: s for p, s in live.items() if not os.path.exists(os.path.join(VENDOR, p[len(REF_PREFIX):]))}
    if sorted(live_missing) != sorted(pin['missing_from_port']):
        failures.append('missing-from-port set changed\n'
                        f'      pin: {sorted(pin["missing_from_port"])}\n'
                        f'      now: {sorted(live_missing)}')
    if live_missing:
        notes.append('missing from the port (headers only): ' + ', '.join(sorted(live_missing)))
    return failures, notes


def main():
    args = sys.argv[1:]
    if not args or args[0] not in ('--write', '--check'):
        print(__doc__)
        return 2
    mode = args[0]
    cache = None
    if '--tree-json' in args:
        cache = args[args.index('--tree-json') + 1]
    if '--pin' in args:
        global PIN_PATH
        PIN_PATH = args[args.index('--pin') + 1]

    if os.path.exists(PIN_PATH):
        with open(PIN_PATH, encoding='utf-8') as fh:
            head = json.load(fh)
        repo, branch, commit = head['repo'], head['branch'], head['commit']
    else:
        # First pin: discover the branch head.
        repo = os.environ.get('KO_ORACLE_REPO', 'crosspoint-reader-ko/crosspoint-reader-ko')
        branch = os.environ.get('KO_ORACLE_BRANCH', 'release/korean')
        r = sh('gh', 'api', f'repos/{repo}/git/ref/heads/{branch}', '--jq', '.object.sha')
        if r.returncode != 0:
            print(f'cannot resolve {repo} {branch}: {r.stderr.strip()}', file=sys.stderr)
            return 2
        commit = r.stdout.strip()

    # Live branch check: the pin records a commit, but a moved branch is the first thing a
    # reader wants to know.
    live = sh('gh', 'api', f'repos/{repo}/git/ref/heads/{branch}', '--jq', '.object.sha')
    live_sha = live.stdout.strip() if live.returncode == 0 else None

    if mode == '--write':
        blobs = fetch_tree(repo, commit, cache)
        pin = write_pin(repo, branch, commit, blobs, PIN_PATH)
        print(f"wrote {PIN_PATH}")
        c = pin['counts']
        print(f"  {repo} {branch} @ {commit}")
        print(f"  engine files {c['reference_engine_files']}: identical {c['identical']}, "
              f"divergent {c['divergent']}, missing from port {c['missing_from_port']}")
        return 0

    cache_dir = os.environ.get('KO_ORACLE_SRC', '')
    blobs = fetch_tree(repo, commit, cache)
    failures, notes = check_pin(repo, commit, blobs, cache_dir)
    print(f'oracle pin check — {repo} {branch} @ {commit}')
    for n in notes:
        print(f'  .  {n}')
    if live_sha and live_sha != commit:
        print(f'  !  {branch} has moved: {live_sha} (pin stays on {commit} until re-audited)')
    if failures:
        print(f'\nFAIL — {len(failures)} drift(s):')
        for f in failures:
            print(f'  x  {f}')
        return 1
    print('\nPASS — vendored engine matches the pinned reference at the level it claims')
    return 0


if __name__ == '__main__':
    sys.exit(main())
