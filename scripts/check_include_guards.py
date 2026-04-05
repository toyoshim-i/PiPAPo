#!/usr/bin/env python3
"""Check that #ifndef/#define include guards match the full path.
Expected: PPAP_<PATH_IN_CAPS_WITH_UNDERSCORES>
e.g. src/kernel/core/proc/proc.h -> PPAP_KERNEL_CORE_PROC_PROC_H
     src/arch/arm_m/kernel/common/irq.h -> PPAP_ARCH_ARM_M_KERNEL_COMMON_IRQ_H
"""
import sys, os, re

def expected_guard(filepath):
    """Compute expected guard from file path relative to src/."""
    # Strip leading path up to and including src/
    idx = filepath.find('src/')
    if idx >= 0:
        rel = filepath[idx + 4:]  # after "src/"
    else:
        rel = filepath
    # Convert to uppercase, replace / and . and - with _
    guard = rel.upper().replace('/', '_').replace('.', '_').replace('-', '_')
    return 'PPAP_' + guard

def check_file(path):
    with open(path) as f:
        lines = f.readlines()
    
    # Find #ifndef ... #define ... pair
    ifndef_guard = None
    define_guard = None
    ifndef_line = None
    
    for i, line in enumerate(lines, 1):
        stripped = line.strip()
        if stripped.startswith('#ifndef ') and ifndef_guard is None:
            ifndef_guard = stripped.split()[1]
            ifndef_line = i
        elif stripped.startswith('#define ') and ifndef_guard and define_guard is None:
            tokens = stripped.split()
            if len(tokens) >= 2 and tokens[1] == ifndef_guard:
                define_guard = tokens[1]
                break
            else:
                # Not a matching define — reset
                ifndef_guard = None
                ifndef_line = None
    
    if ifndef_guard is None:
        return None  # No include guard found (might be .inc or guarded differently)
    
    expected = expected_guard(path)
    if ifndef_guard != expected:
        return (path, ifndef_line, ifndef_guard, expected)
    return None

def main():
    script_dir = os.path.dirname(os.path.abspath(__file__))
    root = os.path.join(script_dir, '..', 'src')
    violations = []
    for dirpath, _, filenames in os.walk(root):
        if '/user/' in dirpath:
            continue
        for fn in sorted(filenames):
            if not fn.endswith('.h'):
                continue
            path = os.path.join(dirpath, fn)
            if '/kernel/' not in path and '/common/' not in path:
                continue
            v = check_file(path)
            if v:
                violations.append(v)
    
    if violations:
        for v in violations:
            print(f"{v[0]}:{v[1]}: guard '{v[2]}' should be '{v[3]}'")
        print(f"\n{len(violations)} include guard violations")
        sys.exit(1)
    else:
        print("Include guards OK")

if __name__ == '__main__':
    main()
