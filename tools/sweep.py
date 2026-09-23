#!/usr/bin/env python3
"""
No control is silently dead.

A GLSL uniform whose name does not match the C++ is ignored without a word:
glGetUniformLocation returns -1 and glUniform on -1 is a documented no-op. So a
slider can be wired to nothing while the plugin compiles, links, loads and
renders perfectly. This renders each parameter at three positions across its
declared range and checks the picture actually changed.

    python3 tools/sweep.py [--binary build/whtest]

--------------------------------------------------------------- the traps

**Almost everything here is invisible with a still eye, and that is the
plugin.** A still eye on an RGB wheel returns the input exactly, whatever the
wheel speed, the segment widths, the gamma or the mix -- `whtest --still` is
the proof. So most entries carry `Eye Mode=1` as context, which is not the
sweep being lenient: it is the one condition under which those controls have
anything to do.

**A saccade has to be in flight.** Saccade Size and Saccade Time reach the
picture only between a hit and the fringe dying away, so their contexts fire
one (`@tone` puts a click on frame 6) and stop eight frames in, while it is
still visible. Fire is pressed by `--set` on frame 0, so its run is short too.

**Positions are scaled to the declared range.** An option's range is its
element count and Bit Depth's is 1..8; a sweep at 0, 0.5 and 1 would read
both as "the first element, twice, and the second".

**An entry beginning with `@` is a harness setting, not a plugin parameter.**
`@frames=` is the run length and `@tone` pushes a synthetic click train into
the Audio buffer.
"""

import argparse
import hashlib
import pathlib
import re
import subprocess
import sys
import tempfile

# What else has to be true for a parameter to have any effect at all.
CONTEXT = {
    # The primaries are normalised per channel, so with a still eye no wheel
    # geometry can show; the eye has to move.
    "Wheel Speed": ["Eye Mode=1"],
    "Red Width": ["Wheel Type=3", "Eye Mode=1"],
    "Green Width": ["Wheel Type=3", "Eye Mode=1"],
    "Blue Width": ["Wheel Type=3", "Eye Mode=1"],
    # A white segment's share shows with a still eye: it brightens greys.
    "White Width": ["Wheel Type=3"],
    "White Gain": ["Wheel Type=1"],
    "Pursuit Speed": ["Eye Mode=1"],
    "Pursuit Angle": ["Eye Mode=1"],
    "Saccade Size": ["Eye Mode=3", "@tone", "@frames=8"],
    "Saccade Time": ["Eye Mode=3", "@tone", "@frames=8"],
    "Fire": ["@frames=8"],
    "Bit Planes": ["Eye Mode=1"],
    "Bit Depth": ["Bit Planes=1", "Eye Mode=1"],
    # Single and three chip differ only once something moves or is offset.
    "Chip Mode": ["Eye Mode=1"],
    "Red dx": ["Chip Mode=1"],
    "Red dy": ["Chip Mode=1"],
    "Blue dx": ["Chip Mode=1"],
    "Blue dy": ["Chip Mode=1"],
    "Panel Rotate": ["Chip Mode=1"],
    "Mix": ["Eye Mode=1"],
    "Output Gamma": ["Eye Mode=1"],
}

# The FFT buffer is the host's to fill and the About block is a text line and
# four browser buttons -- sweeping those opens a tab per press.
SKIP_KINDS = {"buffer", "about", "text"}


def render(binary, out, settings, frames, tone):
    command = [binary, "--out", str(out), "--size", "320x180", "--frames", str(frames)]
    if tone:
        command.append("--tone")
    for setting in settings:
        command += ["--set", setting]

    result = subprocess.run(command, capture_output=True, text=True)
    if result.returncode != 0:
        raise RuntimeError(f"render failed: {result.stderr.strip()}")
    return hashlib.sha256(out.read_bytes()).hexdigest()


def parameters(binary):
    """name, kind and (low, high), from the harness's own declaration."""
    listing = subprocess.run([binary, "--list"], capture_output=True, text=True)
    if listing.returncode != 0:
        raise RuntimeError(f"could not list parameters: {listing.stderr.strip()}")

    found = []
    for line in listing.stdout.splitlines()[1:]:
        # "  0  Wheel Type            option     0.0000  [ 0 .. 3 ]"
        m = re.match(r"\s*\d+\s+(.+?)\s{2,}(\w+)\s+\S+\s+(?:\[\s*(\S+)\s*\.\.\s*(\S+)\s*\]|-)", line)
        if not m:
            continue
        name, kind, low, high = m.group(1).strip(), m.group(2), m.group(3), m.group(4)
        rng = (float(low), float(high)) if low is not None else None
        found.append((name, kind, rng))
    return found


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--binary", default="build/whtest")
    arguments = parser.parse_args()

    binary = pathlib.Path(arguments.binary)
    if not binary.exists():
        print(f"no {binary} -- build with -DWHEEL_BUILD_TOOLS=ON first")
        return 2

    try:
        declared = parameters(str(binary))
    except RuntimeError as error:
        print(error)
        return 2

    if not declared:
        print("no parameters found -- the --list parsing has gone stale")
        return 2

    dead = []
    swept = 0
    skipped = []

    with tempfile.TemporaryDirectory() as directory:
        out = pathlib.Path(directory) / "sweep.png"

        for name, kind, rng in declared:
            if kind in SKIP_KINDS or rng is None:
                skipped.append((name, kind))
                continue

            context = list(CONTEXT.get(name, []))

            # Long enough for the Track eye to have measured a velocity.
            frames = 24
            tone = False
            for entry in list(context):
                if entry.startswith("@frames="):
                    frames = int(entry.split("=", 1)[1])
                    context.remove(entry)
                elif entry == "@tone":
                    tone = True
                    context.remove(entry)

            low, high = rng
            positions = [low, 0.5 * (low + high), high]

            digests = set()
            failed = False
            for position in positions:
                try:
                    digests.add(render(binary, out, context + [f"{name}={position:g}"], frames, tone))
                except RuntimeError as error:
                    print(f"  {name}: {error}")
                    dead.append(name)
                    failed = True
                    break

            if failed:
                continue

            swept += 1
            alive = len(digests) > 1
            if not alive:
                dead.append(name)
            note = f"   ({', '.join(context)})" if context else ""
            print(f"  {'ok' if alive else 'DEAD':4}  {name}{note}")

    print()
    for name, kind in skipped:
        print(f"  skip  {name}: {kind}")

    print()
    if dead:
        print(f"{len(dead)} parameter(s) changed nothing: {', '.join(dead)}")
        print("either the uniform name does not match the shader, or the sweep")
        print("needs a CONTEXT entry saying what else has to be true.")
        return 1

    print(f"all {swept} swept parameters measurably change the picture")
    return 0


if __name__ == "__main__":
    sys.exit(main())
