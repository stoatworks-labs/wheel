# Wheel

> **AI-assisted project.** This codebase was created with [Claude](https://claude.com/claude-code)
> (Anthropic), directed and reviewed by a human author. The model is
> verified numerically by an offline harness that drives the real plugin
> class in a headless GL context: a still eye is proved to return the input
> **bitwise** at every wheel speed on a moving clip, a pursuit is proved to
> land the red and green fields exactly `v(t_G − t_R)` apart (whole-pixel
> cases as an exact translation, fractional ones to a derived 8-bit bound),
> and every one of those checks is shown to **fail** when the model is
> perturbed — see [Status](#status). It has **never been loaded into
> Resolume**, on any platform. Check it in your own rig before trusting it in
> a show.

A single-chip DLP projector — the colour wheel, the retina that adds its
sub-fields up, and the rainbow effect a moving eye sees through it — as an
FFGL effect for [Resolume](https://resolume.com) Arena and Avenue.

![A rightward pursuit over the harness's moving card: red leads and blue trails on every bright edge](docs/hero.png)

*A pursuit of 12 px a frame to the right, on a 1x RGB wheel. Red leads and
blue trails on every bright edge, because red was shown first and the eye
had moved on by the time blue arrived. The colour bars fringe too: this is
not an edge effect, it is where each field landed on the retina. Rendered by
`whtest`, not captured from Resolume.*

## The one idea

**A single-chip DLP does not show a colour picture.** It shows a red one,
then a green one, then a blue one (and on many wheels a white or CMY
segment), in turn, through a spinning filter wheel. The eye adds them up.

It only adds them up correctly **if the eye is still relative to the
screen.** When the eye moves — pursuing a moving object, or in a saccade
across a bright frame — each colour field lands on the retina in a
different place. That is the rainbow effect, and it is the whole plugin.

So the model is a retina, not a filter. Each sub-field is shown for its
segment's duration, and the retina integrates them, displaced by the eye's
travel during that sub-field. Everything you recognise falls out of that.
None of it is drawn:

| What you see | Why it happens |
| --- | --- |
| **No fringe on a still eye**, however fast the clip moves | The DMD holds the frame for the whole frame period, so every colour lands in the same place. The harness proves this bitwise. |
| **A pursued object fringes, and so does the background** | The eye moves; everything on the held frame lands `v · t` from where it was. |
| **A rainbow flash on a loud hit** | A saccade is a burst of eye velocity; an audio onset fires one. |
| **A faster wheel packs the fringe tighter** | At 2x the red and green sub-fields are half as far apart in time, so half as far apart on the retina. Proved as a ratio. |
| **Whites brighter, colours where they were** | A white segment shows the pixel's shared component. A saturated primary has none. |
| **Greys breaking into bands under motion** | The DMD makes grey from binary-weighted bit planes; with eye motion the planes of one grey land in different places. Off by default. |
| **A static fringe that never moves** | Three-chip mode: no wheel, three panels, and the misconvergence between them. |

![Three-chip mode with red and blue panels offset and rotated](docs/threechip.png)

*Three-chip mode: no temporal fringe at all, and the other kind of
projector fringe — red and blue panels a few pixels off and slightly
rotated against green.*

## Controls

The full manual, control by control, is the [user guide](docs/USER-GUIDE.md).

**Wheel** — *Wheel Type* (`RGB`, `RGBW`, `RGBCMY` in the BrilliantColor
order R Y G C B M, `Custom`), *Wheel Speed* (1x, 2x, 3x, 4x, 6x rotations per
frame; the sub-field times follow exactly), *Red/Green/Blue/White Width*
(Custom only), *White Gain* (how bright the secondary segments are, against
the primaries).

**Eye** — *Eye Mode* (`Still`: the projector working as intended; `Pursuit`:
a set velocity and angle; `Track`: the clip's own dominant motion, by coarse
block matching; `Saccade`: still, with a burst on every audio onset), *Pursuit
Speed* (0–32 px/frame), *Pursuit Angle*, *Saccade Size* (0–200 px of travel),
*Saccade Time* (20 ms–1 s, the decay), *Fire* (a saccade by hand, in any
mode), *Audio*.

**DMD** — *Bit Planes*, *Bit Depth* (1–8).

**Three-Chip** — *Chip Mode* (`Single Chip`, `Three Chip`), *Red dx*, *Red dy*,
*Blue dx*, *Blue dy* (±8 px), *Panel Rotate* (±1°).

**Output** — *Mix*, *Output Gamma* (the picture is linearised on the way in
and re-encoded on the way out; 1.0 is linear and skips the pow).

Two worth knowing:

- **With Eye Mode on Still, most controls do nothing**, and that is correct
  rather than a bug — the primaries are normalised so that a still eye
  reconstructs the input exactly whatever the wheel. Eye Mode is the first
  thing to reach for.
- **A white segment can push a white past 1.0.** That is what a real RGBW
  wheel does to the ratio of white brightness to colour brightness; the
  re-encode clips it. Turn White Gain down if it bothers you.

## Status

**v0.1.0, and honestly early.** It has **never been loaded into Resolume**,
on any platform, and never installed into Extra Effects. Everything below is
the offline harness, which drives the real plugin class headlessly through
the real FFGL sequence. Measured on an M4 Max, macOS 26.4, on 2026-09-23.
Run it yourself with `tools/verify.sh`, which builds the universal bundle
from scratch and takes about a minute. Every picture check runs at
**320×180** (what CI's GPU-less runner uses) **and 1280×720**, and every
tolerance is derived from a lattice — one 8-bit code, one texel — not fitted
to a number this machine printed.

| Check | Result |
| --- | --- |
| A still eye returns the input | **0 bytes differ** of 4.4 M per raster, at all five wheel speeds, on a moving clip, through the gamma 2.2 round trip and without it, and on a custom wheel with widths 0.5/0.2/0.3 |
| Pursuit separation | at v = 6 px, 1x: R–G **2.0000** (want 2.0000), and red is green translated by 2 px **bitwise** over 910,080 pixels; 2x: **1.0000**; at v = 4.5 (fractional): 1.5000 and 0.7509 against 1.5 and 0.75, bound 0.018; ratio 2x/1x **0.5000** and 0.5006 |
| Energy | per-channel sums unchanged under pursuit to **1.1e-4** relative (RGB 2x, 6 px), 3.2e-4 (13.7 px at 37°), and on an RGBCMY 3x wheel and in three-chip mode, all inside the worst-case 8-bit bound of 5–9e-3 |
| White segment | RGBW at White Gain 1: grey 64 → **128** (want 128.00); at 0.5 → 96; Custom R.3 G.3 B.3 W.1 → 85 (want 85.33); red 255 → 255 with green 0 in every case |
| Three-chip convergence | red (2,−3) and blue (−1,1): **0 of 889,856 pixels wrong**, bitwise; fractional 1.5 and −2.25: centroids −1.5000 and 2.2510, bound 0.009 |
| Saccade | 0 onsets before the first hit (primed), **exactly one** saccade on the hit's frame; the fringe decays by e⁻¹ over Saccade Time: ratios 0.3676, 0.3677, 0.3680 against 0.3679 |
| Bit planes | 8 planes are the input **bitwise**; 4 planes are the 4-bit input bitwise; under motion 4 and 8 differ in 2.4 M bytes |
| Track eye | a one-cell-per-frame scroll reads **10.02** px/frame at 320×180 (cell 10) and 40.09 at 720p (cell 40); a still picture reads 0.01 and 0.04 |
| A resize mid-run | the still eye still returns the input bitwise on the first new frame; the Track eye starts from zero; no crash |
| `--pipe` | a still eye through the pipe is the input **bitwise**; a cue acts from its frame and not before; a Fire cue saccades; a partial frame at EOF is dropped; an unknown name is refused |
| Negative controls | **12 of 12 reject**: a DMD that interpolates, segment times 15% and 5% out, taps summing past one, a W share 15% out, an offset a pixel out, an unprimed detector, a Saccade Time 30% out, four planes judged as eight, two cells judged as one |
| Mutation | `p + off` → `p − off` in the shipped integrate shader: `--separation` fails 14 of 18; `--still` and `--energy` correctly do not |
| No dead controls | all **23** swept parameters measurably change the picture |
| Shaders | 4 shaders compile through `glslc`, not merely Apple's driver |
| macOS binary | universal (`x86_64 arm64`), exports `plugMain`, plist correct, ad-hoc signs |
| What a host sees | `oxbow probe`: name `SW Wheel`, id `WH01`, type `effect` |

**Render cost**, `whtest --bench`, 60 frames after a 20-frame warm-up with
`glFinish` on both sides, macOS only:

| | defaults (RGB 2x, still eye) | pursuit, RGBCMY 6x (the heaviest wheel) | Track eye (a readback per frame) |
| --- | --- | --- | --- |
| 1280×720 | 0.45 ms | 2.27 ms | 1.08 ms |
| 1920×1080 | **0.90 ms** | 5.45 ms | 1.72 ms |
| 2560×1440 | 1.66 ms | 9.61 ms | 2.65 ms |
| 3840×2160 | **3.29 ms** | **19.1 ms** | 5.19 ms |

The heaviest wheel is 36 sub-fields a pixel, each a box of up to nine
hand-rolled bilinear taps; at 4K under pursuit that is more than a 60 fps
frame. RGB at 2x is six.

**Not done:** never run in Resolume; no OpenFX port and no browser demo (not
required at 0.1.0); no factory presets; the audio
path has only ever seen the harness's synthetic spectra, and nobody has
measured what Resolume's 64 FFT bins are; the Track eye is a global
estimate that follows whatever most of the picture is doing, not the object
you are looking at; CI is written and has never run, because there is no
remote. `StoatworksAbout.h` and `ATTRIBUTIONS.md` are provisional hand
copies. See [AGENTS.md](AGENTS.md) for the full list of what is assumed
rather than measured, and for the traps.

## Build

```bash
git clone --recursive https://github.com/stoatworks-labs/wheel
cd wheel
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
cmake --install build     # straight into Resolume's Extra Effects folder, on macOS
```

macOS builds universal (arm64 + x86_64) by default; add
`-DCMAKE_OSX_ARCHITECTURES=arm64` for a faster development build. Needs CMake
3.15+, a C++17 compiler and the FFGL SDK submodule (pinned to `b1afaf9`).

A bundle you build yourself is unsigned, which is fine locally — quarantine
only applies to files that arrive from a browser.

## Building and testing

The harness renders the real plugin class offline and asserts one claim per
flag, at two rasters:

```bash
./build/whtest --out /tmp/frame.png --set "Eye Mode=1"   # the moving card, pursued
./build/whtest --list                   # every parameter, kind, default and range
./build/whtest --still                  # a still eye returns the input, bitwise
./build/whtest --separation             # pursuit lands the fields v(t_G - t_R) apart
./build/whtest --energy                 # displacement neither makes nor loses light
./build/whtest --white                  # a W segment brightens grey, not a primary
./build/whtest --converge               # three-chip: a panel offset is exact
./build/whtest --saccade                # one primed onset, one saccade, e^-1 decay
./build/whtest --bits                   # bit planes reconstruct their grey
./build/whtest --track                  # the Track eye finds a one-cell scroll
./build/whtest --resize                 # a resize mid-run
./build/whtest --negative               # every check can fail
./build/whtest --bench                  # 720p through 4K, three loads
python3 tools/sweep.py                  # no control is silently dead
tools/verify.sh                         # all of it, from a fresh universal build
```

`--set "Name=value"` sets any control by its display name, repeatably;
`--tone` pushes a synthetic click train into the audio input and `--fire N`
presses Fire on frame N.

To film a clip through the real plugin, `--pipe` takes the fleet's raw
format — RGBA frames on stdin, RGBA frames on stdout — on a synthetic clock
of `--fps` frames a second, driven by an optional cue sheet:

```bash
ffmpeg -i in.mov -f rawvideo -pix_fmt rgba - \
  | ./build/whtest --pipe --size 1920x1080 --fps 30 --script cues.txt \
  | ffmpeg -f rawvideo -pix_fmt rgba -s 1920x1080 -r 30 -i - out.mov
```

```
# frame  Parameter Name  value     (host units: 0..1 sliders, option values)
0    Pursuit Speed   0.375         # 12 px a frame
30   Eye Mode        1             # Pursuit from frame 30
90   Eye Mode        0             # Still again
120  Fire            1             # one saccade by hand
121  Fire            0
```

Sliders and Bit Depth interpolate between their keys; options, Bit Planes
and Fire step. An unknown name refuses the run. The Audio input is silence
(or `--tone`), so Saccade mode only saccades on a Fire cue.

<!-- attributions:start -->
This project is built on other people's work — see [ATTRIBUTIONS.md](ATTRIBUTIONS.md).
<!-- attributions:end -->

## Licence

MIT — see [LICENSE](LICENSE).

The single-chip DLP — a colour wheel in front of a digital micromirror
device, bit-plane grey, and the colour breakup a moving eye sees — is
described in the display engineering literature and Texas Instruments'
published DLP material. No code was taken from any projector or vendor.
