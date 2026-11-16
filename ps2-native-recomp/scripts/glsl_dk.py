#!/usr/bin/env python3
"""GLSL for the deko3d backend: the GL shaders (runtime/src/gs/gpu/glsl, already through glsl_compat.py and the Switch
Mesa fixes) with every r32ui buffer image turned back into a storage buffer. deko3d binds storage buffers by address
(the NVN way, which Ryujinx and the hardware both run well); buffer images would need image descriptors.

usage: glsl_dk.py <in.glsl> <out.glsl>
"""
import re
import sys


def args_of(src, start):
    """src[start] is just after '('; returns (list of top-level args, index after the closing ')')."""
    depth, args, cur, i = 0, [], [], start
    while True:
        ch = src[i]
        if ch == '(':
            depth += 1
        elif ch == ')':
            if depth == 0:
                args.append(''.join(cur).strip())
                return args, i + 1
            depth -= 1
        elif ch == ',' and depth == 0:
            args.append(''.join(cur).strip())
            cur = []
            i += 1
            continue
        cur.append(ch)
        i += 1


def convert(src):
    src = re.sub(r'layout\(r32ui, binding = (\d+)\) uniform (coherent|readonly|writeonly)? ?uimageBuffer (\w+);',
                 lambda m: f'layout(std430, binding = {m.group(1)}) {m.group(2) or ""} buffer {m.group(3)}_b {{ uint {m.group(3)}[]; }};',
                 src)
    src = src.replace('memoryBarrierImage()', 'memoryBarrierBuffer()')
    out, i = [], 0
    pat = re.compile(r'image(Load|Store|AtomicXor|AtomicAdd|AtomicOr|AtomicAnd|AtomicMin|AtomicMax|AtomicExchange)\(')
    while True:
        m = pat.search(src, i)
        if not m:
            out.append(src[i:])
            break
        out.append(src[i:m.start()])
        args, end = args_of(src, m.end())
        args = [convert(a) for a in args]
        op = m.group(1)
        if op == 'Load':
            if not src.startswith('.x', end):
                sys.exit(f'imageLoad without .x at {m.start()}')
            out.append(f'{args[0]}[{args[1]}]')
            end += 2
        elif op == 'Store':
            v = re.fullmatch(r'uvec4\((.*), 0u, 0u, 0u\)', args[2], re.S)
            if not v:
                sys.exit(f'imageStore value not uvec4(v, 0u, 0u, 0u): {args[2]}')
            out.append(f'{args[0]}[{args[1]}] = {v.group(1)}')
        else:
            out.append(f'atomic{op[6:]}({args[0]}[{args[1]}], {args[2]})')
        i = end
    return ''.join(out)


if __name__ == '__main__':
    with open(sys.argv[1], encoding='utf-8') as f:
        text = convert(f.read())
    if 'image' in re.sub(r'//.*', '', text).replace('_image', ''):
        for n, line in enumerate(text.splitlines(), 1):
            if re.search(r'\bimage\w*\(|uimageBuffer', line):
                sys.exit(f'{sys.argv[1]}:{n}: image access left: {line.strip()}')
    with open(sys.argv[2], 'w', encoding='utf-8', newline='\n') as f:
        f.write(text)
