# AGENTS.md — Wheel

Onboarding for whoever (or whatever) picks this up next. `CLAUDE.md` is the
short command reference; this is the *why*. Read "What is actually verified"
before you tell anybody this works.

---

## What the plugin is

An FFGL 2.1 effect (`WH01`, `SW Wheel`) that models a **single-chip DLP
projector**: a colour wheel of filter segments in front of a DMD that holds
each frame, and the retina that adds the colour sub-fields back together —
correctly when the eye is still, and as the rainbow effect when it moves.

---

## The one idea

**The fringe is in the eye, not the projector.**

A single-chip DLP shows one colour at a time. The DMD holds the current frame
for the whole frame period and shows it through each segment's filter as the
wheel passes. A still eye receives every field in the same place and sees
the picture. An eye moving at `v` receives field `k`, shown at time `t_k`,
displaced by `v · t_k` — and that is all the rainbow effect is.

So the plugin is a **retina**:

    output( r ) = Σ_k  W_k · F_k · I( r + E( t_k ) )      (box-blurred over the sub-field)

where `F_k` is the segment's filter, `W_k` its share of the frame times the
lamp normalisation, `I` the held frame in linear light, and `E( t )` the eye's
displacement within the frame, relative to the frame's midpoint. Everything
in `Segments.h`.

What falls out, and is proved:

- **A still eye on any wheel returns the input, bitwise** (`--still`). Not
  approximately: the primaries are normalised per channel so that a primary's
  weight is `w / w`, which IEEE makes exactly 1, and the still-eye path is a
  single `texelFetch` per sub-field with no filtering in it.
- **Pursuit separates the fields by `v( t_G − t_R )`** (`--separation`), with
  the earlier field leading in the direction of eye travel, and a 2x wheel
  halving it.
- **Light is conserved** (`--energy`): displacement moves it, and the
  trapezoid taps sum to exactly one.
- **A white segment brightens grey by its share and a saturated primary not
  at all** (`--white`), because it shows `min( r, g, b )`.
- **A saccade decays with Saccade Time** (`--saccade`).
- **Bit planes reconstruct their grey exactly and come apart under motion**
  (`--bits`).

### What does not fall out, and is the honest limit

**The eye is a model, not a measurement.** A pursuit here is a constant
velocity; a saccade is an exponentially decaying burst in a hashed direction;
Track is a coarse global block match. Real eyes accelerate, overshoot, and
look at one thing while the background does another. What this reproduces
is the *geometry* of the artefact — where each field lands for a given eye
path — and that part is exact. Which eye path a viewer's eye actually takes
is the operator's to choose.

**The saccade's box is linear across a sub-field.** The displacement within
a sub-field follows an exponential, and the shader integrates a uniform box
between its two ends. For a pursuit the two are identical. For a saccade
with a Saccade Time much shorter than a frame the blur within one sub-field
is slightly wrong in shape; the *position* of each sub-field is right, and
`--saccade` measures positions.

**Nothing interpolates between frames**, because a DMD does not. The
negative control gives it one and `--still` fails.

---

## The shape of the code

| File | What it is |
| --- | --- |
| `source/Segments.{h,cpp}` | The wheel as arithmetic: segments, the normalisation, the frame's schedule of sub-fields, the eye's displacement and each sub-field's placement. No GL. |
| `source/Controls.{h,cpp}` | What a 0..1 slider means, with inverses where a check has to hit a value exactly. |
| `source/Onset.{h,cpp}` | Spectral flux against an adaptive floor, primed on its first frame. |
| `source/Shaders.{h,cpp}` | Three fragment shaders: linearising copy, motion grid, integrate. |
| `source/Wheel.{h,cpp}` | The plugin: parameters, the clock, the saccade, the Track estimate, the passes. |
| `source/PassBuffer.{h,cpp}`, `Clock.{h,cpp}`, `Diag.{h,cpp}` | From cadence, unchanged but for the namespace. |
| `tools/whtest/` | The offline harness: renders, checks, negative controls, benchmarks. |
| `tools/sweep.py` | No control is silently dead. |
| `whtest --pipe` | The fleet's filming format: raw RGBA through the real plugin on a synthetic millisecond clock, driven by a cue sheet. `runPipe` in `tools/whtest/main.cpp`. |
| `tools/verify.sh` | All of it, from a fresh universal build. |

Three passes:

1. **copy** — picture size, RGBA16F, `texelFetch` per texel, linearised by
   Output Gamma. Exact by construction; no MaxUV, no sampler.
2. **grid** — 32×18, Track mode only: cell luminance off the copy's mip
   chain, read back (576 floats) for the block match on the CPU.
3. **integrate** — the host's framebuffer. For each sub-field, a hand-rolled
   bilinear from four fetches (with the secondary's `min` applied per texel
   first), one sample for a still eye or a trapezoid box along the eye's path;
   bit planes; three-chip offsets; re-encode; mix.

The CPU decides the schedule and the placements in `double`; the GPU touches
pixels. Everything the harness needs to reason about is a short list of
numbers it can read out of the plugin (`ScheduleForTest`,
`PlacementsForTest`) plus a picture it can measure.

---

## Traps

Roughly in the order they cost time.

### ☠️ A secondary's `min` has to be taken per mirror, before the blur

Min is concave, so `min( blur( g ), blur( b ) ) ≥ blur( min( g, b ) )`: a min
taken after the retinal blur *makes light out of the blur*. The first version
took it after, because that is where the code was shorter. It is now inside
`bilinear()`, applied to each fetched texel, so the retina blurs what the DMD
showed. The energy check on a six-segment wheel is the check that exists for
this, though what it actually caught first was the next trap.

### ☠️ Anything past 1.0 clips, and a clipped channel is not linear in anything

The energy patch was at 255. With secondaries adding on top, every channel
went past 1.0, the 8-bit output clipped, and `--energy` reported a 5%
"gain" on the RGBCMY wheel that was really the blur spreading a clipped edge
into unclipped pixels. The patch is now dim enough for any wheel. Worth
knowing as a *design* fact too: an RGBW wheel at White Gain 1 doubles a
white, which is what a real one does to the white/colour brightness ratio,
and the re-encode clips it. That is deliberate and documented in the README.

### ☠️ `InitGL` must not clear a pending Fire

Hosts (and the harness) push parameter values *before* `InitGL`. The first
version cleared `firePending` there, so a Fire that arrived early was
dropped. `tools/sweep.py` reported Fire dead and was right.

### ☠️ Resolume's clock overflows a float

Measured across the fleet at ~499 million ms. Nothing absolute crosses into
GLSL here: the wheel's phase is per frame (an integer number of rotations
per frame means every frame starts at angle zero), and the saccade's
remaining travel is decayed frame by frame in `double` on the CPU. The clock
is only used for `dt`.

### ☠️ Prime the onset detector

The first spectrum a detector sees is a rise from silence in every bin, and
the one-pole floor fed a frame of `dt = 0` snaps to that spurious flux and is
deaf for a second. `Onset` takes its first frame as the reference and does
not compare it. `--saccade` asserts zero onsets before the first real hit,
and the negative control turns priming off and shows it fire on frame one.

### `texelFetch` everywhere, and a snapped pixel

The bilinear is built by hand from four `texelFetch`es with its own weights,
so at a whole-pixel position the weights are exactly 1 and 0 and no sampler
is consulted. The fragment's pixel is `floor( uv * size ) + 0.5` rather than
`uv * size`, because an interpolated uv is not guaranteed to land on the
pixel centre to the last bit, and a one-pixel displacement from a centre one
ulp off is not a whole pixel. Both are what let `--separation` and
`--converge` claim *bitwise* translation on any rasteriser.

### `x / x` is exactly 1; `( 1 / 3 ) * 3` is not promised to be

The primary normalisation is written as `s.width / primarySum[ c ]` — the
same double over itself — and not as `width * ( 1.0 / sum )`. IEEE 754
guarantees the first is exactly 1.0. The addendum's warning about exact
cancellation is why.

### `FFGLShader::Set` has no array overload

`SubW`, `SubO` and `SubT` go through `glUniform4fv` against a hand-fetched
location. `Set( name, a, b )` on an array would resolve to `( float, float )`
and raise `GL_INVALID_OPERATION` where nothing can see it.

### Fire fires on every send, so the pipe sends on change

`SetFloatParameter( PT_FIRE, v )` sets `firePending` for any `v >= 0.5`, not
on a rising edge: a host sends 1 on press and 0 on release, and only when it
changes. Anything that re-applies a held value every frame — pbtest's cue
loop does — turns one press into a saccade a frame. `runPipe` sends a cue
only when its value changes, and verify.sh's pipe step would catch the
other half of the trap (a Fire held at its first key from frame 0).

### A control the host alone can feed reads as dead

`Audio` (a buffer), and `Fire` (an event). The sweep skips the buffer and
presses Fire through `--set`, which is why Fire must survive `InitGL`.

### With a still eye, the plugin is the identity — and the sweep knows

Twenty of the twenty-three controls need `Eye Mode=1` or a wheel with a
white segment as context to do anything. That is the plugin's central claim
showing up in the sweep, not the sweep being lenient.

### The fleet's standing traps, all of which apply

`ScopedFBOBinding` restores the framebuffer and not the viewport (captured
at the top of `ProcessOpenGL`, restored before the integrate pass). Every
`ffglex::Scoped*` clears to 0 on exit, so every `Ensure()` happens before
anything binds a texture. `FFGLFBO::Release()` leaks the colour texture;
`PassBuffer::Destroy()` deletes it first. `FFGLScopedFBOBinding.h` is not in
the umbrella header. A `TEXT` parameter without `SetTextParameter` kills the
plugin in a real host. A ranged `STANDARD` parameter cannot have a ranged
default (Bit Depth is a real `FF_TYPE_INTEGER`, which is exempt). The plugin
registers itself from a file-scope constructor, so the core is an OBJECT
library. `nm | grep -q` fails when grep succeeds under pipefail. `vcpkg.json`
is invisible from the CMakeLists.

---

## Would this hold on another rasteriser, at another raster?

Every numeric check, one line each. "Raster" means the two the harness runs,
320×180 and 1280×720, unless stated.

| Check | Assertion | Tolerance, and where it comes from | Rasteriser-independent? | Raster-independent? |
|---|---|---|---|---|
| `--still` | output == input, **bitwise in 8 bits**, 6 frames × 5 speeds × 3 wheels | **Zero.** The still path is one `texelFetch` per sub-field times a weight of exactly 1.0 (`x / x`) or 0.0; the sum of a value and exact zeros is the value. With gamma 2.2 the round trip is `pow( pow( k/255, 2.2 ), 1/2.2 )` through an RGBA16F store: the GLSL 4.10 §8.2 bound on `pow` is a few ULP relative and the 16F store is 2⁻¹¹ relative, so the worst absolute error is ~0.06 of a code and 8-bit rounding lands on `k`. This is an argument from the spec's bounds, not from this GPU | Yes, by that argument. The one thing it assumes is that a driver's `pow` is within the spec's stated ULP bound | Run at both rasters |
| `--separation`, whole pixel | red == green translated by `v/3R` px, **bitwise** | **Zero.** The red taps are the green taps at whole-pixel offsets, so the red and green results are the same arithmetic on the same texels | Yes: no sampler, hand-rolled weights of exactly 1 and 0 | Both rasters, 910,080 pixels at 720p |
| `--separation`, centroid | `c_R − c_G == v( t_G − t_R )` | **Derived per run**: each lit pixel may be off by half a code, so the centroid can move by at most `Σ|x − c| · 0.5 / mass`, computed from the measured profile and printed (0.012–0.018 px). Observed error: 0.0000 whole, 0.0009 fractional | Yes: a coverage-weighted centroid, never a thresholded one; the taps are a partition of unity | Both rasters; the expectation is in pixels and does not scale |
| `--separation`, ratio | `sep_2x / sep_1x == 0.5` | The two bounds combined, over the 1x separation (0.012–0.016) | Yes | Both |
| `--energy` | per-channel frame sum unchanged by eye motion | **Worst case 8-bit**: half a code on every lit pixel of both pictures, summed (5–9e-3 relative); observed 1e-4 to 2e-3 | Yes: bilinear weights and trapezoid weights each sum to one in exact arithmetic, and the patch never reaches the edge clamp | Both; the bound is recomputed from the support at each |
| `--white` | grey 64 → `64 · ( 1 + gain · w_W / w_R )` | **One code**: the expectation is not always an integer (85.33) and the measurement is one. Observed exact where the expectation is an integer | Yes: a single fetch per sub-field, weights from `double` | Both; a single pixel is read |
| `--converge`, whole pixel | each channel == its input channel shifted by (dx, dy), **bitwise**, interior only | **Zero** | Yes | Both |
| `--converge`, fractional | centroid moved by exactly the offset | Derived 8-bit centroid bound as above (0.009) | Yes | Both |
| `--saccade`, count | 0 onsets before the hit; exactly 1 saccade, on its frame | **Exact integers** | Yes — no GL in the count | Both |
| `--saccade`, decay | `sep( n + τ ) / sep( n ) == e⁻¹` at three `n` | The two 2-D centroid bounds over the earlier separation (0.011–0.017); observed error ≤ 0.0003 | Yes: the ratio is a property of `saccadeRemaining *= exp( −1/τ )` in `double`; the picture only has to locate two centroids | Both; the direction is hashed and the measurement is a 2-D distance, so it does not care which way the saccade went |
| `--bits` | 8 planes == input; 4 planes == `round( k/255 · 15 ) / 15` re-encoded; **bitwise** | **Zero.** `Σ 2^b · bit_b / ( 2^B − 1 )` is `q / ( 2^B − 1 )` in exact arithmetic, and `q · 17` is an integer for B = 4 | Yes, at linear gamma (the quantisation is in linear light, and with gamma 2.2 the 8-bit linear value is not the 8-bit code) | Both |
| `--track` | one cell a frame reads as one cell a frame; still reads still | **1% of a cell, stated not derived.** The lattice answer is exact (a whole-cell shift gives a SAD minimum of exactly zero at that shift), and the parabolic refinement adds a texture-dependent bias (0.023 cells here) because the SAD at ±1 cell is not symmetric. The still case is the smoothing's 0.5¹⁰ of a cell | Yes: SAD over 576 floats read back; the grid comes off a mip chain whose exact values differ per driver but whose minimum does not move | Both; cell sizes 10 and 40 |
| `--resize` | still eye bitwise on the first new frame; Track reads 0.0 | **Zero** | Yes | The check IS a raster change, 320×180 → 400×200 |
| `--negative` | nine perturbations, asserted to FAIL | n/a | Yes | 320×180 |
| `--bench` | — | Not pass/fail | — | — |

### The negative controls

`whtest --negative` perturbs the model by what a real defect would produce
and asserts the relevant check rejects it:

1. a DMD given **inter-frame interpolation** (`SetInterpolationForTest`,
   a uniform-gated branch in the shipped shader): `--still` — 8,652 bytes
   differ
2. **segment times 15% out** (`SetTimeScaleForTest`): `--separation`'s
   centroid (2.298 against 2.000, bound 0.0135) *and* its bitwise translation
3. **segment times 5% out**: both again (2.102 against 2.000) — the
   interesting boundary, since the bound is under 1%
4. **box taps 5% heavy at the ends** (`SetTapBiasForTest`): `--energy` —
   511,764 against 487,520, bound 3,652
5. **a W share 15% out**: `--white` — 137.6 predicted, 128 measured
6. **a panel offset judged one pixel out**: `--converge` — 5,783 pixels wrong
7. **an unprimed detector** (`SetOnsetPrimingForTest( false )`): fires once
   before the first hit
8. **a Saccade Time 30% long**: the decay ratio 0.463 against 0.368 measured,
   tolerance 0.011
9. **four bit planes judged as eight**: 147,612 bytes differ
10. **a scroll of two cells judged as one**: 19.98 against 10

All reject, at 320×180. If one ever stops rejecting, the check it belongs
to has gone soft.

### The mutation test

A harness that drives a copy of the code proves nothing about what ships.
One character of the shipped integrate shader was changed — `p + off` to
`p − off` on the pursuit tap path, which reverses which way the fields land —
and `--separation` failed **14 of 18** assertions (every centroid and the
bitwise translation, with R−G reading −2.0000 against +2.0000). `--still` and
`--energy` correctly did not fail: a still eye's offset is zero either way,
and a reversed displacement still conserves light. Reverted, and the tree
carries no trace of it; recorded here so the next person does not wonder.

### What the audit changed

- `--energy` was first written against an RGB still for every wheel. The
  RGBCMY case then failed by 5%, which turned out to be two things at once:
  the secondaries legitimately add light (so the reference must be the same
  wheel, eye stilled), and the patch clipped (see the traps). Both fixed;
  the min-before-blur reordering was made on the way and is the right
  physics, though the clipping was the larger number.
- `--track`'s still case first let the scroll jump from shift 130 to 0 in one
  frame, which is a 130 px cut, not a still picture; it now holds the last
  position. A check whose first run fails is not automatically a bug found.

---

## What is actually verified, and what is assumed

### Verified by measurement, on an M4 Max running macOS 26.4 (2026-09-23)

Everything below is `tools/verify.sh` on this machine, against a fresh
universal Release build, driving the **real plugin class** through the real
FFGL sequence in a headless CGL context. **86 assertions** across eleven
suites, all passing, every picture check at 320×180 and 1280×720.

- **A still eye returns the input bitwise**: 0 of 4,423,680 bytes differ
  per raster per case, at 1x, 2x, 3x, 4x and 6x, on the moving card, with
  gamma 2.2 and linear, and on a custom wheel with widths 0.5/0.2/0.3/0.
- **Pursuit separation** at v = 6 px: R−G 2.0000 at 1x and 1.0000 at 2x, and
  red is green translated by 2 px bitwise over 910,080 pixels; at v = 4.5:
  1.5000 and 0.7509 against 1.5 and 0.75, bound 0.018; ratio 0.5000 / 0.5006.
- **Energy** conserved to 1.1e-4 relative (RGB 2x, 6 px), 3.2e-4 (13.7 px at
  37°), and on RGBCMY 3x and in three-chip mode, all inside worst-case
  bounds of 5–9e-3.
- **White**: 64 → 128, 96 and 85 against 128.00, 96.00 and 85.33; primaries
  untouched.
- **Convergence**: 0 of 889,856 pixels wrong at whole offsets; fractional
  centroids to 0.001 px.
- **Saccade**: primed, one saccade on the hit, decay ratios within 0.0003 of
  e⁻¹.
- **Bit planes**: 8 and 4 exact; under motion 2.4 M bytes come apart.
- **Track**: 10.02 and 40.09 px/frame for 10 and 40; still 0.01 and 0.04.
- **Resize**: bitwise on the first new frame; Track restarts at zero.
- **12 negative controls reject**; the mutation test caught the mutation.
- **All 23 controls** reach the picture.
- **4 shaders compile** through `glslc`.
- **The bundle** is universal, exports `_plugMain`, plist correct, ad-hoc
  signs; `oxbow probe` reports `SW Wheel` / `WH01` / `effect`.
- **Render cost**, 60 frames after a 20-frame warm-up, `glFinish` both sides:

  | | defaults | pursuit, RGBCMY 6x | Track eye |
  | --- | --- | --- | --- |
  | 1280×720 | 0.445 ms | 2.268 ms | 1.083 ms |
  | 1920×1080 | 0.900 ms | 5.454 ms | 1.722 ms |
  | 2560×1440 | 1.663 ms | 9.611 ms | 2.645 ms |
  | 3840×2160 | 3.290 ms | 19.099 ms | 5.190 ms |

  The heaviest wheel under pursuit is 36 sub-fields × up to 9 taps × 4
  fetches a pixel, and at 4K that is more than a frame. Other plugin builds
  were running on this Mac during the bench; treat the numbers as an upper
  bound.

### Assumed, or not done

- ☠️ **Never loaded into Resolume on macOS.** On Windows, v0.1.0's CI build
  passed the fleet Arena gate 8 of 9 in Arena 7.27.1 on llvmpipe (2026-09-23):
  it loads, registers as `SW Wheel` / `WH01` / effect, all 30 host controls
  match the declaration, it renders and the log is clean. The ninth, controls,
  read Saccade Size and Saccade Time dead: they act only during a saccade, and
  the gate sends neither an audio onset (win-lab has no sound device) nor a
  Fire press; `--saccade` and the sweep prove both. (Red dx read live in one
  gate run and dead in the other — a gate flake.) On macOS `oxbow probe` is
  the only host that has opened the bundle. How the controls present on
  macOS, whether Resolume draws `Fire` as a button with one rising edge per press, and what the host's
  blend state does on the way in are untested.
- ☠️ **The audio path has only seen the harness's synthetic spectra.** Nobody
  has measured Resolume's 64 bins; the detector depends on nothing but "a
  hit makes some of them rise", and that is an assumption.
- **The harness has never run on a GPU-less rasteriser.** The plugin has
  rendered on llvmpipe in Arena, but only the gate's checks were made there.
  The argument in the audit table is that no check *can* depend on the
  rasteriser; it is an argument. CI runs on GitHub and is green, but its macOS
  runner has no GL, so it runs only the checks that need none.
- **Nothing has run on Intel.** The build is universal and `lipo` says so;
  only the arm64 slice has executed.
- **The Track eye is a global estimate.** It follows whatever most of the
  picture is doing, to a grid cell, with a texture-dependent bias from the
  parabolic refinement. It is not the object the viewer is looking at.
- **No OpenFX port** (not required at 0.1.0), no factory presets. The browser
  demo exists (2026-09-24); its CPU half is a hand port — see below.
- **`--pipe` has filmed one test clip**, not the release video: 75 frames of
  ffmpeg's `testsrc2` at 640×360, by eye (Still clean, Pursuit fringed, a
  Fire cue saccading, RGBCMY brightening the greys, Three Chip's static
  fringe, 3 bit planes banding). `verify.sh` asserts the round trip at
  64×36; no picture measurement goes through the pipe beyond "bitwise when
  still, different when not".

---

## Decisions taken without asking

The brief said to decide and write it down.

- **Primaries normalised per channel; secondaries on top.** The spec asked
  for a still eye to return the input *and* for a white segment to brighten
  white while leaving a primary alone. Normalising over the whole wheel would
  dim the primaries (which a real RGBW projector does — that is the colour
  brightness complaint); normalising over the primaries alone is the only
  way to satisfy both claims, and the cost is that a bright white can leave
  0..1 and clip. Said so in the README.
- **Secondaries show the shared component**, `min` over the channels the
  filter passes, which is how BrilliantColor-style processing splits a pixel.
- **Custom is R G B W with four width sliders**, a primary never narrower
  than 2% of a turn (its normalisation would divide by zero), and a W at zero
  dropped.
- **RGBCMY is in the order R Y G C B M**, the real alternating order, so a
  primary is never next to a primary. It changes `t_G − t_R` to a third of
  a turn from a sixth.
- **Eye Mode is four modes, and Fire works in all of them.** Saccade mode is
  a still eye plus a saccade on every onset; Fire fires one anywhere, so
  "pursuit and saccade add" holds for the manual trigger. Onsets fire only
  in Saccade mode, so an operator does not get surprise saccades under
  Pursuit.
- **A saccade's direction is a hash of its count.** Random enough to read as
  a saccade, the same on every run.
- **The sub-field's box is centred on the midpoint of its two ends**, not on
  the displacement at its centre time. For a pursuit they are the same
  point; for a saccade the shader's uniform box has to be centred where the
  box is.
- **Bit planes are least-significant first**, one point on the path each,
  quantised in linear light. A real DMD splits the MSB into several slices;
  this does not.
- **Output Gamma 1.0 skips the pow entirely** rather than trusting
  `pow( x, 1.0 ) == x`.
- **`--pipe` is the fleet's format, with two deliberate departures from
  pbtest's cue semantics.** (Added for the release video, 2026-09-23.)
  Units, the parser and the continuous controls are pbtest's exactly: the
  value is what the host sends (0..1 sliders, an option's element value,
  Bit Depth 1..8), sliders and Bit Depth interpolate between keys and hold
  their first key before it. But **options, Bit Planes and Fire step** and
  are **left alone before their first key**, and **a value reaches the
  plugin only when it changes**. pbtest re-sends every track every frame and
  holds the first key from frame 0, which for Wheel would make `65 Wheel
  Type 2` an RGBCMY wheel for the whole take, make `30 Fire 1` a press on
  frame 0, and — because Fire saccades on every value >= 0.5 it is *sent* —
  fire a saccade on every frame a 1 is held. Re-sending only on change is
  also what a host does. The first round trip found the first of these: a
  still eye was not bitwise through the pipe until discrete controls
  stopped holding their first key.
- **`--pipe`'s clock is milliseconds, declared.** `SetTime( frame * 1000 /
  fps )` in double, computed from the index (never accumulated), with the
  clock's unit declared as 0.001 through `SetClockScaleForTest` — a pipe
  renders as fast as the GPU allows, so the calibration has nothing to
  measure. `--fps` outside 24..240 warns, because `Clock` clamps a frame to
  1/240..1/24 s as it would in Resolume.
- **`--pipe` feeds the Audio buffer silence** (or `--tone`'s click train),
  and refuses a cue on `Audio` or an About line. Saccade mode in a take
  therefore saccades only on Fire cues. Feeding a real soundtrack's spectrum
  would need Resolume's 64 bins characterised first, and nobody has.
- **`--script` without `--pipe` is refused** rather than ignored.
- **The user guide (`docs/USER-GUIDE.md`) is plumbicon's shape**, every
  range and behaviour read off the source, not the README. Three things it
  says that the code forced: Red/Blue dx above the middle moves that colour
  LEFT and dy UP (the panel is displaced, so what lands at x came from
  x + dx — `--converge`'s convention); the RGBW 64 -> 128 figure is at
  Output Gamma 1.0, where `--white` measures it (at 2.2 it is ~88, stated as
  arithmetic); and at gamma 2.2 even eight bit planes crush the deepest
  shadows, because the planes quantise linear light.
- **The guide says nothing about notarisation.** `release.yml` ad-hoc signs
  the macOS bundle and nothing in this repo notarises it, so the guide gives
  the quarantine remedy under "If it looks wrong" rather than promising a
  signed download. The Windows installer is built `--plain` and is not
  code-signed, which the guide does say.
- **`guide` in `StoatworksAbout.h` is set** to
  `https://stoatworks-labs.com/software/wheel/guide/`, plumbicon's shape,
  so the About block has four buttons. Like `page`, it 404s until the site
  registers the project and publishes the guide; the sync overwrites this
  hand copy then. The About block is last in the enum, so the extra id
  shifts nothing.
- **The bench reports three loads**, because "the cost" of this plugin
  ranges over a factor of six depending on the wheel and the eye, and a
  single number would be either a lie or a worst case.

---

## Open questions

- **Should the W segment's content be `min( r, g, b )` or luminance?** Real
  processing pipelines differ; `min` is the one that leaves a saturated
  primary exactly alone, which is what the spec asked to be measured.
- **Should the primaries be normalised over the whole wheel instead**, so a
  white never clips and colours dim as on a real RGBW projector? It is the
  more faithful model of brightness and the less faithful one of the spec's
  two claims; a `Normalise` option could offer both.
- **Should Track be local?** A block match around the frame's brightest
  moving object would model "the eye follows the thing", where the global
  estimate models "the eye follows the pan". Both are real behaviours.
- **Is 16-bit float enough for the linear copy on a driver whose `pow` is
  worse than the spec's bound?** The argument for `--still` has 8× headroom
  on an 8-bit source; a 10-bit host would eat two of those bits.
- **Should the heaviest wheel be cheaper?** RGBCMY at 6x under pursuit is 19
  ms at 4K. Merging the taps of sub-fields that share a filter would cut it,
  at the cost of the schedule no longer being a plain list.

---

## Siblings

- **`cadence`** — the closest relative: colour fields in time, the ring,
  the clock, the harness shape, `sweep.py`, `verify.sh` and the workflows.
- **`readout`** — the float-clock trap and the per-row sample time.
- **`abomerration`** — per-channel displacement done cleanly.
- **`tinsel`** — the fleet's trap list, `PassBuffer`, the CI shape.
- **`photofinish`** — the shape of the rasteriser audit and the negative
  controls.
- **`oxbow`** — `oxbow probe` is what loads this bundle as a host.

## The browser demo, 2026-09-24

- **The whole retina is on the page.** All three passes — copy, grid, integrate —
  run from the plugin's own GLSL, and the CPU half is ported whole: `Segments.cpp`
  (the four wheels, the per-channel primary normalisation, the sub-field schedule,
  `DisplacementAt`, `PlaceSubField`), `estimateMotion` with its synchronous
  576-float readback of the RGBA32F grid, the PCG hash that picks a saccade's
  direction, the per-frame decay, and the 1/240 … 1/24 s clamp from `Clock.cpp`.
  The `timeScale`, `Interp` and `TapBias` hooks are held at the values the plugin
  always runs with. Nothing checks the port but a reader.
- **The audio side is absent rather than present and dead.** No `Audio` buffer, no
  `Onset.cpp`. An onset in the plugin does nothing but set `firePending`, which is
  exactly what Fire does, so Saccade mode is still the same code path — driven by a
  hand instead of a kick drum.
- **Fire is a toggle the renderer releases** (readout's precedent) and **Bit Depth
  is a slider** over 1..8; the kit has neither an event nor an integer control.
- **The page opens on the plugin's defaults, which show nothing** — Eye Mode Still
  is the projector working as intended. That was kept rather than "fixed" with a
  livelier default: the controls must be the plugin's. The tagline says, as the
  plugin's own description does, to start with Eye Mode on Pursuit, and the first
  preset does it.
- **Track is honest and dull here**: the kit's clips barely move, so the block
  matcher finds little. The disclosure says Pursuit is the way to see the fringe.
- **Presets are the page's own**, disclosed as such; the plugin ships none.
- Verified 2026-09-24 in headless Chrome (Metal): renders, no console errors and no
  WebGL warnings in Pursuit, Track, Saccade (Fire pressed) and Three Chip; changing
  Wheel Speed under Pursuit with the transport paused changes 31% of the pixels.
