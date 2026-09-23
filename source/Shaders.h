#pragma once

/**
	The passes, as GLSL source.

	Three fragment shaders and one vertex shader:

	1. **copy**       picture size, RGBA16F. The host's input, linearised by
	                  Output Gamma, fetched texel for texel with `texelFetch`
	                  so the copy is exact by construction and no sampler is
	                  involved. Everything after it reads this buffer.
	2. **grid**       kGridWidth x kGridHeight, RGBA32F. The picture's
	                  luminance averaged over a cell, off the copy's mip
	                  chain, for the Track eye's block matching. Only in Track
	                  mode; the CPU reads it back.
	3. **integrate**  output size, straight to the host's framebuffer. The
	                  retina. For each sub-field: the linear picture through
	                  the segment's filter, displaced by the eye and box-blurred
	                  along its path, added in linear light. Then the three-chip
	                  misconvergence, the bit planes, the re-encode and the mix.

	**Every picture read is a texelFetch.** `bilinear()` builds a bilinear
	sample out of four fetches and its own weights, so a whole-pixel
	displacement has weights of exactly 1 and 0 and returns the texel. That
	is what lets the harness claim bitwise results for whole-pixel offsets
	on any rasteriser: no filtering hardware is consulted.

	**The pixel is snapped.** `p = floor( uv * size ) + 0.5`: the
	interpolated uv is not guaranteed to land on the pixel centre to the last
	bit, and a displacement of exactly one pixel from a centre that is one
	ulp off is not a whole-pixel displacement any more. Snapping makes an
	output the picture's size exact and an output another size a
	nearest-neighbour rescale, which for a picture made of discrete mirrors
	is the honest choice.
*/
namespace wheel
{

/// The Track eye's measurement grid. Coarse on purpose: this is a global
/// motion estimate, not optical flow, and 576 floats read back per frame
/// is cheap.
constexpr int kGridWidth  = 32;
constexpr int kGridHeight = 18;

/// The most taps a sub-field's box blur takes. Past this a box longer than
/// kMaxTaps - 1 pixels is sampled more coarsely than a pixel, which is
/// visible as banding only at eye speeds nobody would set.
constexpr int kMaxTaps = 9;

extern const char* const kVertexShader;
extern const char* const kCopyShader;
extern const char* const kGridShader;
extern const char* const kIntegrateShader;

} // namespace wheel
