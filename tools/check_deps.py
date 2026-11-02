#!/usr/bin/env python3
"""Enforce the module rules of CLAUDE.md on app/:

- a module is a flat directory (no nested directories);
- the only cross-module include is "<m>/<m>.h" (or any "foundation/<x>.h");
- the allowed edges are exactly those listed in app/DEPS;
- app.c may include anything.
Exit code 1 and a list of violations otherwise.
"""
import os, re, sys

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'app')
INC = re.compile(r'^\s*#\s*include\s*"([^"]+)"', re.M)

def load_deps():
    allow = {}
    with open(os.path.join(ROOT, 'DEPS')) as f:
        for line in f:
            line = line.split('#', 1)[0].strip()
            if not line: continue
            name, _, rest = line.partition(':')
            allow[name.strip()] = set(rest.split())
    return allow

def main():
    allow = load_deps()
    problems = []
    modules = [d for d in sorted(os.listdir(ROOT)) if os.path.isdir(os.path.join(ROOT, d))]
    for m in modules:
        if m not in allow:
            problems.append(f'{m}/: directory is not declared in app/DEPS')
            continue
        mdir = os.path.join(ROOT, m)
        for entry in os.listdir(mdir):
            path = os.path.join(mdir, entry)
            if os.path.isdir(path):
                problems.append(f'{m}/{entry}/: nested directories are not allowed inside a module')
                continue
            if not entry.endswith(('.c', '.h')): continue
            if m != 'foundation' and not entry.startswith(m + '_') and entry not in (m + '.c', m + '.h'):
                problems.append(f'{m}/{entry}: files must be <m>.c/.h or <m>_<part>.c/.h')
            src = open(path, encoding='utf-8', errors='replace').read()
            module_files = set(os.listdir(mdir))
            for inc in INC.findall(src):
                if '/' not in inc:
                    # A bare include is either a private header of this module or an external
                    # (IDF / third-party) header. Another module's private header can only be
                    # reached with a path, which is caught below.
                    continue
                target, _, header = inc.partition('/')
                if target == m:
                    if header not in module_files:
                        problems.append(f'{m}/{entry}: includes "{inc}" which does not exist')
                    continue
                if target == 'foundation':
                    if 'foundation' not in allow.get(m, set()) and m != 'foundation':
                        problems.append(f'{m}/{entry}: includes foundation but DEPS does not allow it')
                    continue
                if target not in allow:
                    continue  # IDF / third-party header with a slash, e.g. "driver/gpio.h"
                if header != target + '.h':
                    problems.append(f'{m}/{entry}: includes private header "{inc}" of another module')
                if target not in allow[m]:
                    problems.append(f'{m}/{entry}: depends on {target}, which DEPS forbids for {m}')
    # cycles in DEPS itself
    def reach(a, seen):
        for b in allow.get(a, ()):
            if b in seen: continue
            seen.add(b); reach(b, seen)
        return seen
    for a in allow:
        if a in reach(a, set()):
            problems.append(f'DEPS: {a} participates in a dependency cycle')
    if problems:
        print('\n'.join(problems)); return 1
    print(f'check_deps: ok ({len(modules)} modules)'); return 0

if __name__ == '__main__':
    sys.exit(main())
