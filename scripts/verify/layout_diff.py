#!/usr/bin/env python3
"""Explain the difference between two layout manifests.

A gate that fails with "the files differ" is only half a gate: the point of dumping the
layout was to find out WHICH decision moved. This reports the first differences grouped by
kind, in the order they would be hit while reading the page, and a count per kind so a
one-word shift does not look like a whole-book break.

usage: layout_diff.py <a.json> <b.json> [--limit N]
"""
import json
import sys


def page_key(p):
    return (p['spine'], p['page'])


def diff_manifest(a, b):
    out = []
    for k in ('margins', 'viewport', 'font', 'screen', 'oracle'):
        if a.get(k) != b.get(k):
            out.append((0, 'header', f'{k}: {a.get(k)} vs {b.get(k)}'))
    for k, v in (a.get('spec') or {}).items():
        if (b.get('spec') or {}).get(k) != v:
            out.append((0, f'spec.{k}', f'{v} vs {(b.get("spec") or {}).get(k)}'))

    pa = {page_key(p): p for p in a['pages']}
    pb = {page_key(p): p for p in b['pages']}
    if len(a['pages']) != len(b['pages']):
        out.append((0, 'page count', f"{len(a['pages'])} vs {len(b['pages'])}"))
    only_a = sorted(set(pa) - set(pb))
    only_b = sorted(set(pb) - set(pa))
    if only_a:
        out.append((0, 'pages only in A', f'{only_a[:6]}'))
    if only_b:
        out.append((0, 'pages only in B', f'{only_b[:6]}'))

    for key in sorted(set(pa) & set(pb)):
        x, y = pa[key], pb[key]
        tag = f'spine {key[0]} page {key[1]}'
        if x.get('visibleTextOffset') != y.get('visibleTextOffset'):
            out.append((key[1], f'{tag}: visibleTextOffset',
                        f"{x.get('visibleTextOffset')} vs {y.get('visibleTextOffset')}"))
        if len(x['lines']) != len(y['lines']):
            out.append((key[1], f'{tag}: line count', f"{len(x['lines'])} vs {len(y['lines'])}"))
        for li, (lx, ly) in enumerate(zip(x['lines'], y['lines'])):
            where = f'{tag} line {li}'
            if (lx['x'], lx['y']) != (ly['x'], ly['y']):
                out.append((key[1], f'{where}: y/x',
                            f"y{lx['y']} x{lx['x']} vs y{ly['y']} x{ly['x']}"))
            if lx.get('ruby') != ly.get('ruby'):
                out.append((key[1], f'{where}: ruby', f"{lx.get('ruby')} vs {ly.get('ruby')}"))
            wxs, wys = lx['w'], ly['w']
            if len(wxs) != len(wys):
                out.append((key[1], f'{where}: word count', f'{len(wxs)} vs {len(wys)}'))
            for wi, (wx, wy) in enumerate(zip(wxs, wys)):
                if wx != wy:
                    out.append((key[1], f'{where} word {wi}',
                                f"x{wx['x']} s{wx['s']} {wx['t']!r} vs x{wy['x']} s{wy['s']} {wy['t']!r}"))
                    break
        if x.get('images') != y.get('images'):
            out.append((key[1], f'{tag}: images', f"{x.get('images')} vs {y.get('images')}"))
        if x.get('rules') != y.get('rules'):
            out.append((key[1], f'{tag}: rules', f"{x.get('rules')} vs {y.get('rules')}"))
    return out


def main():
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    limit = int(sys.argv[sys.argv.index('--limit') + 1]) if '--limit' in sys.argv else 20
    if len(args) != 2:
        print(__doc__)
        return 2
    a = json.load(open(args[0], encoding='utf-8'))
    b = json.load(open(args[1], encoding='utf-8'))
    diffs = diff_manifest(a, b)
    if not diffs:
        print('layout manifests are identical')
        return 0
    kinds = {}
    for _p, kind, _d in diffs:
        base = kind.split(':')[0]
        kinds[base] = kinds.get(base, 0) + 1
    print(f'{len(diffs)} difference(s)')
    for base, n in sorted(kinds.items(), key=lambda kv: -kv[1])[:8]:
        print(f'  {n:5d}  {base}')
    print('  first differences:')
    for p, kind, detail in diffs[:limit]:
        print(f'    p{p}  {kind}: {detail}')
    if len(diffs) > limit:
        print(f'    ... {len(diffs) - limit} more')
    return 1


if __name__ == '__main__':
    sys.exit(main())
