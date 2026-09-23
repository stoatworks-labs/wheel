"""The demo's GLSL must be the plugin's GLSL, character for character.

    python3 demo/tools/check_shaders.py

Called from `tools/verify.sh`. Exit code 1 means the two have drifted.

------------------------------------------------------------------- the point

`demo/plugin.js` carries a second copy of every shader in `source/Shaders.cpp`,
because a browser cannot include a C++ file. Two copies of a shader is exactly
the arrangement that drifts, and the drift is invisible from both sides: the
plugin keeps working, the page keeps working, and they quietly stop being the
same effect. The page's whole claim is that what it runs is the plugin's own
code, so the moment that stops being checkable the page is a lie.

This compares the text, not the behaviour. Reformatting counts as drift, and
that is deliberate -- "it is only whitespace" is how a real change gets waved
through.

The one transformation is a decode, not a normalisation: a backtick cannot
appear raw inside a JavaScript template literal, so plugin.js escapes one as
\\`. This undoes that escape and REJECTS any other backslash on the JS side --
there is none in the C++, so a second escape could only be somebody hiding a
difference.

--------------------------------------------------------------- what it cannot

Nothing here checks the *ported* arithmetic. The conversions out of
`Controls.cpp`, the wheel, schedule, eye and placement out of `Segments.cpp`,
the Track eye's block matcher, the saccade's hash and decay out of
`Wheel::ProcessOpenGL`, and the clamping half of `Clock.cpp` in plugin.js are a
hand translation, and only a reader can tell whether they still agree. When you
change one in C++, change it there too.
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]

# The C++ constant, and the JS constant it must equal.
PAIRS = [
    ("source/Shaders.cpp", "kVertexShader", "VERTEX"),
    ("source/Shaders.cpp", "kCopyShader", "COPY"),
    ("source/Shaders.cpp", "kGridShader", "GRID"),
    ("source/Shaders.cpp", "kIntegrateShader", "INTEGRATE"),
]


def cpp_literal(path, name):
    """The body of `const char* const name = R"( ... )";`.

    A shader may be several ADJACENT raw strings -- MSVC caps one literal at
    about 16 KB -- so everything up to the terminating semicolon is joined, the
    same recipe tools/verify.sh's extraction uses.
    """
    source = (ROOT / path).read_text()
    match = re.search(
        r'const char\* const\s+' + re.escape(name)
        + r'\s*=\s*((?:\s*(?://[^\n]*\n)*\s*R"\(.*?\)")+)\s*;',
        source,
        re.S,
    )
    if match is None:
        return None
    return "".join(re.findall(r'R"\((.*?)\)"', match.group(1), re.S))


def js_literal(source, name):
    """The body of ``const NAME = `...`;``, with the backtick escape undone.

    Returns (text, complaint)."""
    match = re.search(r'^const\s+' + re.escape(name) + r'\s*=\s*`(.*?)`;$', source, re.S | re.M)
    if match is None:
        return None, None
    body = match.group(1)
    stray = re.search(r"\\(?!`)", body)
    if stray is not None:
        line = body[: stray.start()].count("\n") + 1
        return None, f"backslash that is not an escaped backtick, at line {line}"
    return body.replace("\\`", "`"), None


def main():
    js = (ROOT / "demo/plugin.js").read_text()
    failures = 0

    for path, cpp_name, js_name in PAIRS:
        expected = cpp_literal(path, cpp_name)
        actual, complaint = js_literal(js, js_name)

        if expected is None:
            print(f"MISSING  {cpp_name} not found in {path}")
            failures += 1
            continue
        if complaint is not None:
            print(f"UNUSABLE {js_name} in demo/plugin.js has a {complaint}")
            failures += 1
            continue
        if actual is None:
            print(f"MISSING  {js_name} not found in demo/plugin.js")
            failures += 1
            continue
        if "${" in expected:
            print(f"UNUSABLE {cpp_name} contains ${{ -- it cannot be a JS template literal")
            failures += 1
            continue

        if expected == actual:
            print(f"ok       {js_name} matches {cpp_name} ({len(expected)} chars)")
            continue

        failures += 1
        print(f"DRIFTED  {js_name} does not match {cpp_name}")
        expected_lines = expected.splitlines()
        actual_lines = actual.splitlines()
        for i in range(max(len(expected_lines), len(actual_lines))):
            a = expected_lines[i] if i < len(expected_lines) else "<end>"
            b = actual_lines[i] if i < len(actual_lines) else "<end>"
            if a != b:
                print(f"         first difference at line {i + 1}")
                print(f"           {path}: {a!r}")
                print(f"           demo/plugin.js: {b!r}")
                break

    print()
    if failures:
        print(f"{failures} shader(s) have drifted. Copy the C++ across; do not edit the JS.")
        return 1

    print(f"all {len(PAIRS)} shaders are identical to the plugin's")
    return 0


if __name__ == "__main__":
    sys.exit(main())
