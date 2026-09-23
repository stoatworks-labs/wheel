# Attributions

Wheel is built on other people's work. This file lists what that work is, who
did it, and what it is doing here.

> **Provisional.** Across the fleet this file is generated from master lists in
> `stoatworks-backend` by `scripts/sync-attributions.py`. Wheel is not yet in
> that script's lists, so this copy is hand-written in the shape the sync
> generates. Registering the project and re-running the sync is the fix.

## Third-party code this project uses

### Resolume FFGL SDK

<https://github.com/resolume/ffgl>
Licence: BSD-3-Clause
Copyright: FreeFrame

Vendored as a git submodule at `external/ffgl`, pinned to `b1afaf9`.

The plugin ABI itself. An FFGL effect is defined by this SDK's headers — there
is no other way to be loadable by Resolume Arena and Avenue.

### GLEW — the OpenGL Extension Wrangler Library

<https://github.com/nigels-com/glew>
Licence: BSD-3-Clause (with Mesa 3-D and Khronos components)
Copyright: Milan Ikits, Marcelo E. Magallon and Lev Povalahev

Windows only, from vcpkg, statically linked. The SDK's headers pull it in for
the OpenGL function pointers; macOS uses the system OpenGL framework instead.

### zlib

<https://zlib.net>
Licence: zlib
Copyright: Jean-loup Gailly and Mark Adler

Linked by the offline harness only, from the macOS system, to deflate the PNGs
it writes. Nothing in the shipped plugin uses it.

## Work from elsewhere in the fleet

### cadence

<https://github.com/stoatworks-labs/cadence>
Licence: MIT
Copyright: Stoatworks Labs

`source/Clock.{h,cpp}`, `source/Diag.{h,cpp}` and `source/PassBuffer.{h,cpp}`
are carried over from cadence (which had them from macroblock, afterglow and
tinsel in turn), as is the shape of the offline harness, `tools/sweep.py`,
`tools/verify.sh` and the two workflows. Copied rather than shared, because
the fleet has no common library and a header shared between two repos by
hand is a header that diverges silently.

### tinsel

<https://github.com/stoatworks-labs/tinsel>
Licence: MIT
Copyright: Stoatworks Labs

The CMake MODULE-plus-submodule layout, the OBJECT-library core and the
release workflow.

## The method

The single-chip DLP modelled here — a colour wheel of filter segments in
front of a digital micromirror device that holds each frame and forms grey
from binary-weighted bit planes, and the colour breakup a moving eye sees
through it — is described in the display engineering literature and in
Texas Instruments' published DLP material. Nothing here is taken from any
projector's firmware or from any vendor's code; the retina model, the
normalisation and the checks are this repository's own.

## Getting this wrong

If your work is here and the description is inaccurate, the licence is wrong, or
you would rather not be listed — open an issue and it will be fixed.
