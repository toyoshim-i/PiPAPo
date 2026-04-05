#!/usr/bin/env python3
"""Check that #include lines within each group are in sorted order.
Convention: <system> headers sort before "project" headers.
Within each category, alphabetical order."""
import sys, os, re

INCLUDE_RE = re.compile(r'^#include\s+([<"])(.+?)[>"]')

def sort_key(kind, path):
    # <system> = 0, "project" = 1, then by path
    return (0 if kind == '<' else 1, path)

def check_file(path):
    violations = []
    with open(path) as f:
        lines = f.readlines()
    
    group = []
    
    def check_group():
        if len(group) < 2:
            return
        keys = [sort_key(g[1], g[2]) for g in group]
        for i in range(len(keys) - 1):
            if keys[i] > keys[i+1]:
                violations.append((group[i][0], group[i+1][0], 
                                  group[i][2], group[i+1][2], path))
                break
    
    for i, line in enumerate(lines, 1):
        m = INCLUDE_RE.match(line.strip())
        if m:
            group.append((i, m.group(1), m.group(2), line.rstrip()))
        else:
            check_group()
            group = []
    check_group()
    return violations

def main():
    script_dir = os.path.dirname(os.path.abspath(__file__))
    root = os.path.join(script_dir, '..', 'src')
    all_violations = []
    for dirpath, _, filenames in os.walk(root):
        if '/user/' in dirpath:
            continue
        for fn in sorted(filenames):
            if not (fn.endswith('.c') or fn.endswith('.h')):
                continue
            path = os.path.join(dirpath, fn)
            if '/kernel/' not in path:
                continue
            vs = check_file(path)
            all_violations.extend(vs)
    
    if all_violations:
        for v in all_violations:
            print(f"{v[4]}:{v[0]}: '{v[2]}' should come after '{v[3]}'")
        print(f"\n{len(all_violations)} include order violations found")
        sys.exit(1)
    else:
        print("Include order OK")

if __name__ == '__main__':
    main()
