# wheel

A single-chip DLP projector — the colour wheel, the retina that integrates
its sub-fields, and the rainbow effect a moving eye sees — as an FFGL
**effect** for Resolume Arena/Avenue. C++17 + GLSL 4.10, CMake MODULE →
universal `.bundle` (macOS) + Windows `.dll`. MIT.

Read `AGENTS.md` before changing the schedule, the normalisation or the
integrate shader. The operator's manual is `docs/USER-GUIDE.md`: every range
and behaviour in it is read off the source, so a control change updates it.

## Commands (CMake)
- Configure: `cmake -B build -DCMAKE_BUILD_TYPE=Release`
- Fast dev build: add `-DCMAKE_OSX_ARCHITECTURES=arm64`
- Universal (what ships, and what `verify.sh` builds): `cmake -B build-universal -DCMAKE_BUILD_TYPE=Release`
- Build: `cmake --build build`
- Install to Resolume: `cmake --install build` (into `~/Documents/Resolume Arena/Extra Effects`)
- Render a frame offline: `./build/whtest --out /tmp/frame.png --size 1280x720 --set "Eye Mode=1"`
- List parameters, with kind, default and range: `./build/whtest --list`
- Set a control: `--set "Eye Mode=1" --set "Pursuit Speed=0.25"` (repeatable, by display name;
  options and Bit Depth take their element value, not a fraction)
- Feed the audio input a click train: `--tone`; press Fire on frame N: `--fire N`
- Film a clip through it: `ffmpeg -i in.mov -f rawvideo -pix_fmt rgba - | ./build/whtest --pipe --size 1920x1080 --fps 30 [--script cues.txt] | ffmpeg -f rawvideo -pix_fmt rgba -s 1920x1080 -r 30 -i - out.mov`
  — raw RGBA frames on stdin, raw RGBA frames on stdout, the fleet's format. The clock is
  synthetic: milliseconds, as Resolume sends them, at `--fps` (default 60), so the wheel, a
  pursuit and a saccade move per frame of the take. A `--script` line is
  `frame  Parameter Name  value` in host units (0..1 sliders, option element values, Bit Depth
  1..8, Fire 1 then 0); sliders and Bit Depth interpolate between keys and hold their first key
  before it (as pbtest); options, Bit Planes and Fire step, and are left alone before their
  first key. A value reaches the plugin only when it changes. An unknown name, `Audio` or an
  About line refuses the run. Audio is silence unless `--tone`. **Film from the first frame**:
  the Track eye and a saccade carry state, so there is no seeking.

## Verify
- Everything, from a fresh universal build: `tools/verify.sh` (~1 min)
- A still eye returns the input, bitwise: `./build/whtest --still`
- Pursuit lands the fields `v(t_G - t_R)` apart: `./build/whtest --separation`
- Displacement neither makes nor loses light: `./build/whtest --energy`
- A W segment brightens grey by its share, not a primary: `./build/whtest --white`
- Three-chip: a panel offset moves that channel exactly: `./build/whtest --converge`
- One primed onset is one saccade, decaying over Saccade Time: `./build/whtest --saccade`
- Bit planes reconstruct their grey: `./build/whtest --bits`
- The Track eye reads one cell a frame as one cell a frame: `./build/whtest --track`
- A resize mid-run: `./build/whtest --resize`
- No parameter name over 16 characters: `./build/whtest --names`
- The pipe round-trips (still eye bitwise, cues from their frame, Fire from a cue): `tools/verify.sh`'s pipe step
- Every check can fail: `./build/whtest --negative`
- All of those in one run: `./build/whtest --all`
- No dead controls: `python3 tools/sweep.py`
- Render cost: `./build/whtest --bench` (see README for the numbers; macOS only)
- What a host sees: `../oxbow/build/oxbow probe build-universal/Wheel.bundle`

Every picture check runs at 320x180 (what CI's GPU-less runner uses) and at
1280x720, and every tolerance is derived from a lattice — one 8-bit code,
one texel — never fitted to a number this Mac printed. See AGENTS.md.

## Notes
- **The model is a retina, not a filter.** `Segments.h` turns the wheel
  into sub-fields (filter, share of the frame, start and end time); the eye
  turns each into a displacement and a box blur; the shader adds them in
  linear light. Nothing interpolates between frames, because a DMD does not.
- **The primaries are normalised per channel** so that a still eye on any
  wheel returns the input exactly (`x / x` is exactly 1 in IEEE). Secondaries
  (W, C, M, Y) add on top at White Gain and show the pixel's shared component
  `min()` — taken **per mirror, before the retina blurs**. Min is concave;
  taken after the blur it makes light.
- **Every picture read is a `texelFetch`**: the bilinear is hand-rolled from
  four fetches, so a whole-pixel displacement is exactly the texel on any
  rasteriser. The pixel is snapped (`floor( uv * size ) + 0.5`) for the same
  reason.
- **Nothing absolute crosses into GLSL.** The wheel's phase is per frame;
  the saccade is integrated frame by frame in double on the CPU. Resolume's
  clock is ~5e8 ms and a float of that resolves to 0.03 s.
- **The onset detector is primed on its first frame**, or every clip launch
  is a saccade and the one-pole floor snaps deaf on `dt = 0`.
- **`InitGL` must not clear `firePending`.** Hosts push values before
  InitGL; the sweep found Fire dead when it did.
- All ranged host parameters are 0..1 and mapped in `Controls.cpp`, with
  inverses where a check needs to hit a value exactly. Bit Depth is a real
  `FF_TYPE_INTEGER` 1..8.
- Override `SetTextParameter` to return `FF_SUCCESS` for the About block, or
  no host can instantiate the plugin at all.
- `ScopedFBOBinding` does not restore the viewport; every `ffglex::Scoped*`
  clears to 0 on exit; `FFGLFBO::Release()` leaks the colour texture
  (`PassBuffer::Destroy()` deletes it). Every `Ensure()` happens before
  anything binds a texture.
- `FFGLShader::Set` has no array overload: the three sub-field arrays go
  through `glUniform4fv`.
- Parameter names must be unique — `--set` and the sweep find them by name.
- macOS build must be universal. Verify with `lipo`, never the build log.
- FFGL id is `WH01`, name `SW Wheel`.

## Not done yet
- **Never loaded into Resolume on macOS.** Everything numeric is the offline
  harness against the real plugin class. The Windows CI build of v0.1.0 passed
  the Arena gate 8 of 9 on win-lab (Arena 7.27.1, llvmpipe) on 2026-09-23; the
  ninth read Saccade Size and Saccade Time dead because the gate never fires a
  saccade.
- No OpenFX port, no factory presets. The browser demo is below.
- The audio path has only seen the harness's synthetic spectra.

## Diagnostics

`source/Diag.{h,cpp}` — log file only, no crash handler (this runs inside
Resolume). It records which shader failed to compile, the GL vendor/renderer/
version, a buffer that could not be allocated, and the host's clock unit at
frame 60.

    ~/Library/Logs/wheel/wheel.YYYY-MM-DD.log             (macOS)
    %LOCALAPPDATA%\wheel\logs\wheel.YYYY-MM-DD.log          (Windows)

## Browser demo

`demo/` is a static page at **wheel-demo.stoatworks-labs.com**, deployed by
`cf-run npx wrangler deploy` from the repo root (`wrangler.toml`, a
static-assets-only Worker — no build step, no Pages, no `_redirects`).

- `demo/vendor/` is vendored from
  `infrastructure/stoatworks-backend/resolume-demo/kit/` and is **not** a place
  to edit. Re-sync with
  `~/Projects/infrastructure/stoatworks-backend/resolume-demo/sync.sh wheel`
  and confirm it says `synced wheel` rather than skipping.
- The four shader constants in `demo/plugin.js` are `source/Shaders.cpp`
  verbatim, and `demo/tools/check_shaders.py` (run by `verify.sh`) fails on a
  single character of drift.
- **Everything else in `demo/plugin.js` is a hand port** of `Controls.cpp`,
  `Segments.cpp`, `estimateMotion`, the saccade's hash and decay, and the
  clamping half of `Clock.cpp`, and **nothing checks it but a reader.** Change
  the schedule or the eye in C++ and it has to be changed there too.
- Absent from the page and said so on it: the `Audio` buffer and `Onset.cpp`
  (a browser has no Resolume FFT parameter), the clock-unit voting, and the
  About block. Fire is a toggle the renderer releases; Bit Depth is a slider.
