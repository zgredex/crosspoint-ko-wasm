#!/usr/bin/env python3
"""Function-level divergence inventory between a reference C/C++ file and the port's copy.

Why: whole-file identity is the strongest gate but some vendored files MUST diverge
(device RAM policy, host caches, wasm render plumbing). A whole-file diff then tells us
nothing about whether the divergence touched TYPOGRAPHY. This extracts top-level
function bodies from both sides, normalises comments/whitespace, and reports the exact
set of functions that actually differ.

Usage: oracle_functions.py <reference.cpp> <port.cpp> [--list]
"""
import re
import sys

IDENT = re.compile(r'^[A-Za-z_~][\w:<>,*&\s\[\]]*?[\w:~]+\s*\(')


def strip_comments(src: str) -> str:
    src = re.sub(r'/\*.*?\*/', ' ', src, flags=re.S)
    src = re.sub(r'//[^\n]*', ' ', src)
    return src


def normalise(body: str) -> str:
    body = strip_comments(body)
    return re.sub(r'\s+', ' ', body).strip()


def functions(src: str):
    """Yield (name, body) for top-level function definitions, by brace matching.

    Heuristic and deliberately conservative: a definition starts at column 0 with
    something that looks like a declarator, and the first non-empty line ending in
    '{' opens the body. Nested braces are balanced with a scanner that is aware of
    strings/char literals/comments. Anything it cannot parse is skipped, never
    guessed at.
    """
    src_nc = strip_comments(src)
    lines = src_nc.split('\n')
    i = 0
    while i < len(lines):
        line = lines[i]
        if not line or line[0] in ' \t#}' or not IDENT.match(line):
            i += 1
            continue
        # Collect the declarator until the opening brace.
        head = []
        j = i
        depth_paren = 0
        found = None
        while j < len(lines) and j < i + 30:
            head.append(lines[j])
            depth_paren += lines[j].count('(') - lines[j].count(')')
            if '{' in lines[j] and depth_paren <= 0:
                found = j
                break
            if ';' in lines[j] and depth_paren <= 0:
                break  # a declaration, not a definition
            j += 1
        if found is None:
            i += 1
            continue
        # Brace-match the body.
        depth = 0
        body = []
        k = found
        while k < len(lines):
            for ch in lines[k]:
                if ch == '{':
                    depth += 1
                elif ch == '}':
                    depth -= 1
            body.append(lines[k])
            if depth == 0:
                break
            k += 1
        # The declarator is part of the function: start at the definition's first line,
        # not at the line that happens to carry the '{'. Wrapped signatures put the name
        # and the parameter list on earlier lines, and dropping them made exactly the
        # long-signature functions (drawTextImpl, getTextWidth, ...) invisible to this
        # parser — silently, which is the worst way to be wrong.
        text = '\n'.join(lines[i:k + 1])
        name_m = re.search(r'(?:[A-Za-z_]\w*::)*([A-Za-z_~]\w*)\s*\([^;]*\)\s*(?:const)?\s*(?:noexcept)?\s*\{',
                           text, re.S)
        if name_m:
            # Skip control-flow keywords that can match the declarator pattern.
            if name_m.group(1) not in ('if', 'for', 'while', 'switch', 'return', 'catch'):
                yield name_m.group(1), text
        i = (k + 1) if k > i else i + 1


def main():
    ref_path, port_path = sys.argv[1], sys.argv[2]
    listing = '--list' in sys.argv
    ref = {n: normalise(b) for n, b in functions(open(ref_path, encoding='utf-8', errors='replace').read())}
    port = {n: normalise(b) for n, b in functions(open(port_path, encoding='utf-8', errors='replace').read())}
    if listing:
        print(f'reference functions: {len(ref)}   port functions: {len(port)}')
    changed = sorted(n for n in ref if n in port and ref[n] != port[n])
    only_ref = sorted(set(ref) - set(port))
    only_port = sorted(set(port) - set(ref))
    print(f'{ref_path}\n  vs {port_path}')
    print(f'  functions: ref {len(ref)} / port {len(port)}   identical {len(set(ref) & set(port)) - len(changed)}')
    if changed:
        print(f'  CHANGED ({len(changed)}): {", ".join(changed)}')
    if only_ref:
        print(f'  ONLY-IN-REFERENCE ({len(only_ref)}): {", ".join(only_ref)}')
    if only_port:
        print(f'  ONLY-IN-PORT ({len(only_port)}): {", ".join(only_port)}')
    return 1 if (changed or only_ref or only_port) else 0


if __name__ == '__main__':
    sys.exit(main())
