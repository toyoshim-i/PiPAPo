#!/usr/bin/env python3
"""Check own-header-first rule: foo.c's first #include should be foo.h (if included)."""
import sys, os, re

INCLUDE_RE = re.compile(r'^#include\s+[<"](.+?)[>"]')

def check_file(path):
    basename = os.path.basename(path)
    if not basename.endswith('.c'):
        return None
    stem = basename[:-2]  # "foo" from "foo.c"
    
    with open(path) as f:
        lines = f.readlines()
    
    first_include_line = None
    first_include_path = None
    own_header_line = None
    own_header_path = None
    
    for i, line in enumerate(lines, 1):
        m = INCLUDE_RE.match(line.strip())
        if m:
            inc_path = m.group(1)
            inc_basename = os.path.basename(inc_path)
            if first_include_line is None:
                first_include_line = i
                first_include_path = inc_path
            if inc_basename == stem + '.h':
                own_header_line = i
                own_header_path = inc_path
                break  # found it
    
    if own_header_line is None:
        return None  # no own header included — OK
    if own_header_line == first_include_line:
        return None  # own header is first — OK
    return (path, own_header_line, own_header_path, first_include_line, first_include_path)

def main():
    script_dir = os.path.dirname(os.path.abspath(__file__))
    root = os.path.join(script_dir, '..', 'src')
    violations = []
    for dirpath, _, filenames in os.walk(root):
        if '/user/' in dirpath:
            continue
        for fn in sorted(filenames):
            if not fn.endswith('.c'):
                continue
            path = os.path.join(dirpath, fn)
            if '/kernel/' not in path:
                continue
            v = check_file(path)
            if v:
                violations.append(v)
    
    if violations:
        for v in violations:
            print(f"{v[0]}:{v[1]}: own header '{v[2]}' should be first (line {v[3]}: '{v[4]}' is first)")
        print(f"\n{len(violations)} own-header-first violations")
        sys.exit(1)
    else:
        print("Own-header-first OK")

if __name__ == '__main__':
    main()
