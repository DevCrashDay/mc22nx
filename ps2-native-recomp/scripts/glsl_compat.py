"""Make naga's GLSL compute/fragment shaders compile to code that Mesa's nouveau (Switch) and Ryujinx both run, and fast.

Each step rewrites a construct whose Maxwell code Ryujinx's shader translator does not implement, or that is slow:

1. Storage buffers -> r32ui buffer images. nouveau compiles every storage-buffer access to LD/ST (global memory), which
   Ryujinx does not implement ("Shader instruction Ld is not implemented"); image loads, stores and atomics (SULD, SUST,
   SUATOM) are what games use and run everywhere. WGSL's atomicLoad/atomicStore, which naga spells atomicOr(x, 0u) and
   atomicExchange(x, v) (a read-modify-write for every read of GS memory), become plain imageLoad/imageStore on a
   coherent image; the sub-word atomicXor stays atomic (imageAtomicXor).
2. main() -> main_body_() called from a new main(): nouveau turns an early `return` in main into PRET/RET, which Ryujinx
   rejects ("Invalid return on main function"); in a called function the GLSL compiler lowers it to plain branches.
3. uint `/` and `%` by a variable -> udiv_/umod_: nouveau calls a library routine for them (JCAL), which Ryujinx does not
   implement. The helpers are exact for operands below 2^31 (GS addresses and sizes are below 2^24).
4. memoryBarrierBuffer() -> memoryBarrierImage(), to go with step 1.
5. switch -> if/else chains: under Ryujinx, threads that leave nouveau's code for a switch (with a `return` in a case, as
   the alpha test in plot() has) never reach the statement after it, so the rasterizer wrote no pixel. WGSL has no
   fallthrough, so each case body (with its closing `break;` removed) becomes one branch.

usage: python glsl_compat.py <glsl file>...   (run on naga's output; idempotent)
"""
import re
import sys

ARRAY = r"_group_0_binding_\d+_[a-z]+"
MARK = "// glsl_compat.py"

HELPERS = """
// glsl_compat.py: uint division without nouveau's division routine (JCAL); exact for a, b < 2^31
uint udiv_(uint a, uint b) {
    float fb = float(b);
    uint q = uint(float(a) / fb);
    q = uint(int(q) + int(floor(float(int(a - q * b)) / fb)));
    int r = int(a - q * b);
    if (r < 0) q -= 1u;
    else if (uint(r) >= b) q += 1u;
    return q;
}
uint umod_(uint a, uint b) { return a - udiv_(a, b) * b; }
"""


def closing(text: str, open_at: int) -> int:
    """Index of the bracket that closes the one at text[open_at]."""
    depth = 0
    for i in range(open_at, len(text)):
        c = text[i]
        if c in "([":
            depth += 1
        elif c in ")]":
            depth -= 1
            if depth == 0:
                return i
    raise ValueError("unbalanced brackets")


def closing_brace(text: str, open_at: int) -> int:
    depth = 0
    for i in range(open_at, len(text)):
        if text[i] == "{":
            depth += 1
        elif text[i] == "}":
            depth -= 1
            if depth == 0:
                return i
    raise ValueError("unbalanced braces")


def opening(text: str, close_at: int) -> int:
    """Index of the bracket that opens the one at text[close_at]."""
    depth = 0
    for i in range(close_at, -1, -1):
        c = text[i]
        if c in ")]":
            depth += 1
        elif c in "([":
            depth -= 1
            if depth == 0:
                return i
    raise ValueError("unbalanced brackets")


def images(text: str) -> str:
    decl = re.compile(r"layout\(std430, binding = (\d+)\) ((?:coherent |readonly )*)buffer \w+ \{ uint (" + ARRAY + r")\[\]; \};")
    names = []

    def to_image(m: re.Match) -> str:
        names.append(m.group(3))
        qual = "readonly" if "readonly" in m.group(2) else "coherent"
        return f"layout(r32ui, binding = {m.group(1)}) uniform {qual} uimageBuffer {m.group(3)};"

    text = decl.sub(to_image, text)
    for name in names:
        out, i = [], 0
        pattern = re.compile(r"(atomic(?:Or|Xor|Exchange|Add|And|Min|Max)\()?\b" + name + r"\[")
        while (m := pattern.search(text, i)):
            out.append(text[i:m.start()])
            bracket = m.end() - 1
            end = closing(text, bracket)
            index = text[bracket + 1:end]
            if m.group(1):  # atomicOp(name[index], value)
                call_end = closing(text, m.start() + len(m.group(1)) - 1)
                value = text[end + 1:call_end].lstrip(", ").strip()
                op = m.group(1)[len("atomic"):-1]
                if op == "Or" and value == "0u":
                    out.append(f"imageLoad({name}, int({index})).x")
                elif op == "Exchange" and re.match(r"\s*;", text[call_end + 1:]) and text[:m.start()].rstrip(" ").endswith("\n"):
                    out.append(f"imageStore({name}, int({index}), uvec4({value}, 0u, 0u, 0u))")
                else:
                    out.append(f"imageAtomic{op}({name}, int({index}), {value})")
                i = call_end + 1
                continue
            store = re.match(r" = ([^;]*);", text[end + 1:])
            if store and text[:m.start()].rstrip(" ").endswith("\n"):  # name[index] = value; as a statement
                out.append(f"imageStore({name}, int({index}), uvec4({store.group(1)}, 0u, 0u, 0u));")
                i = end + 1 + store.end()
                continue
            out.append(f"imageLoad({name}, int({index})).x")
            i = end + 1
        out.append(text[i:])
        text = "".join(out)
    return text.replace("memoryBarrierBuffer()", "memoryBarrierImage()")


def wrap_main(text: str) -> str:
    return re.sub(r"(?m)^void main\(\) \{", "void main_body_() {", text) + "\nvoid main() { main_body_(); }\n"


def divisions(text: str) -> str:
    # naga reuses names (_e18) across functions with other types: a name counts as uint in the function that declares it so
    functions = [(m.start(), closing_brace(text, m.end() - 1)) for m in re.finditer(r"(?m)^\w+ \w+\([^)]*\) \{", text)]
    globals_ = set(re.findall(r"(?m)^uint (\w+)\b", text))  # non-const top-level uints (rare)

    def uint_in_scope(name: str, at: int) -> bool:
        for start, end in functions:
            if start <= at <= end:
                body = text[start:end]
                if re.search(r"\b(float|int|vec\d|uvec\d|ivec\d|bool|const uint) " + name + r"\b", body):
                    return False
                return re.search(r"\buint " + name + r"\b", body) is not None or name in globals_
        return False

    pattern = re.compile(r" (/|%) (\w+)\)")
    out, i = [], 0
    for m in pattern.finditer(text):
        if not uint_in_scope(m.group(2), m.start()):
            continue
        close = m.end() - 1
        open_ = opening(text, close)
        if open_ < i:
            continue
        left = text[open_ + 1:m.start()]
        out.append(text[i:open_])
        out.append(f"{'udiv_' if m.group(1) == '/' else 'umod_'}({left}, {m.group(2)})")
        i = close + 1
    out.append(text[i:])
    text = "".join(out)
    if "udiv_(" not in text:
        return text
    lines = text.split("\n")
    at = max(n for n, line in enumerate(lines) if line.startswith(("#version", "#extension", "precision")))
    return "\n".join(lines[:at + 1] + [HELPERS] + lines[at + 1:])


def switches(text: str) -> str:
    serial = 0
    while True:
        done = True
        for m in re.finditer(r"switch\(", text):
            expr_end = closing(text, m.end() - 1)
            brace = text.index("{", expr_end)
            end = closing_brace(text, brace)
            block = text[brace + 1:end]
            if "switch(" in block:
                continue  # inner ones first
            expr = text[m.end():expr_end]
            branches, default, i, labels = [], None, 0, []
            ok = True
            while True:
                lm = re.compile(r"\s*(case ([^:]+)|default):").match(block, i)
                if not lm:
                    break
                labels.append(lm.group(2))
                i = lm.end()
                if not re.match(r"\s*\{", block[i:]):
                    continue  # grouped labels: case 0u: case 1u: { ... }
                b0 = block.index("{", i)
                b1 = closing_brace(block, b0)
                body = block[b0 + 1:b1].rstrip()
                if body.endswith("break;"):
                    body = body[:-len("break;")].rstrip()
                if "break;" in body and not re.search(r"\b(while|for)\s*\(", body):
                    ok = False  # a break out of the middle of a case: leave this switch as it is
                if None in labels:
                    default = body
                else:
                    branches.append((labels, body))
                labels, i = [], b1 + 1
            if not ok or block[i:].strip():
                continue
            kind = "uint" if any(l.strip().endswith("u") for ls, _ in branches for l in ls) else "int"
            var = f"sw{serial}_"
            serial += 1
            out = [f"{{ {kind} {var} = {expr};"]
            for n, (ls, body) in enumerate(branches):
                cond = " || ".join(f"{var} == {l.strip()}" for l in ls)
                out.append(f"{'if' if n == 0 else ' else if'} ({cond}) {{{body}\n}}")
            if default is not None:
                out.append(f" else {{{default}\n}}" if branches else f"{{{default}\n}}")
            out.append(" }")
            text = text[:m.start()] + "".join(out) + text[end + 1:]
            done = False
            break
        if done:
            return text


def clip_space(text: str) -> str:
    """6. naga negates y for WebGPU's clip space; OpenGL's has y up already, so the picture came out upside down."""
    return text.replace("gl_Position.yz = vec2(-gl_Position.y, gl_Position.z * 2.0 - gl_Position.w);",
                        "gl_Position.z = gl_Position.z * 2.0 - gl_Position.w;  // glsl_compat.py: no y flip in OpenGL")


def convert(text: str) -> str:
    if MARK in text:
        return text
    text = clip_space(text)
    text = images(text)
    text = switches(text)
    if re.search(r"(?m)^void main\(\) \{", text) and "return;" in text.split("void main() {", 1)[1]:
        text = wrap_main(text)
    text = divisions(text)
    return f"{MARK}: converted for nouveau and Ryujinx (see the script)\n" + text if not text.startswith("#version") else \
        text.replace("\n", f"\n{MARK}: converted for nouveau and Ryujinx (see the script)\n", 1)


def main(paths: list[str]) -> int:
    for path in paths:
        with open(path, encoding="utf-8") as f:
            text = f.read()
        new = convert(text)
        if new != text:
            with open(path, "w", encoding="utf-8", newline="\n") as f:
                f.write(new)
        print(f"[glsl_compat] {path}: {'converted' if new != text else 'unchanged'}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
