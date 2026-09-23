# Attributions

Wheel is built on other people's work. This file lists what that work is, who did
it, and what it is doing here.

It is generated — the master lists live in the `stoatworks-backend` repo and are
pushed out by `scripts/sync-attributions.py`. Edit it there, not here.

## Code we derived from other people's work

Someone else solved this first, and this project would not exist in its current form without their work.

### Clock, Diag, PassBuffer and the harness shape — Stoatworks cadence

<https://github.com/stoatworks-labs/cadence>  
Licence: MIT  
Copyright: Stoatworks Labs

source/Clock.{h,cpp}, source/Diag.{h,cpp} and source/PassBuffer.{h,cpp} are carried over from cadence (which had them from macroblock, afterglow and tinsel in turn), as are the shape of the offline harness, tools/sweep.py, tools/verify.sh and the two workflows.

### Build layout and release workflow — Stoatworks tinsel

<https://github.com/stoatworks-labs/tinsel>  
Licence: MIT  
Copyright: Stoatworks Labs

The CMake MODULE-plus-submodule layout, the OBJECT-library core and the release workflow.

## Third-party code this project uses

Libraries, SDKs and frameworks the project is built on or bundles.

### Resolume FFGL SDK

<https://github.com/resolume/ffgl>  
Licence: BSD-3-Clause  
Copyright: FreeFrame

Vendored as a git submodule at external/ffgl (third_party/ffgl in oxbow).

The plugin ABI itself. An FFGL effect or source is defined by this SDK's headers — there is no other way to be loadable by Resolume Arena and Avenue.

### GLEW — the OpenGL Extension Wrangler Library

<https://github.com/nigels-com/glew>  
Licence: BSD-3-Clause (with Mesa 3-D and Khronos components)  
Copyright: Milan Ikits, Marcelo E. Magallon and Lev Povalahev

Arrives inside the FFGL submodule at external/ffgl/deps/glew-2.1.0. Not fetched separately.

Resolves OpenGL entry points on Windows, where the system headers stop at OpenGL 1.1.

### libpng

<http://www.libpng.org/pub/png/libpng.html>  
Licence: PNG Reference Library License (libpng)  
Copyright: the PNG Reference Library authors

Arrives inside the FFGL submodule, under the SDK's CustomThumbnail sample.

Part of the upstream SDK tree rather than something these plugins call directly — listed because it is present in the checkout.

## Inspirations

What this set out to be. No code, assets or binaries from any of these were used or examined — the debt is to the idea.

### Single-chip DLP projection

A colour wheel of filter segments in front of a digital micromirror device that holds each frame and forms grey from binary-weighted bit planes, and the colour breakup a moving eye sees through it, as described in the display engineering literature and Texas Instruments' published DLP material. Nothing is taken from any projector's firmware or any vendor's code; the retina model, the normalisation and the checks are this repository's own.

## Getting this wrong

If your work is here and the description is inaccurate, the licence is wrong, or you would rather not be listed — open an issue and it will be fixed.
