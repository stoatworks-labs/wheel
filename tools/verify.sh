#!/usr/bin/env bash
#
# Everything that can be checked without a host, in one go.
#
#   tools/verify.sh
#
# It builds the UNIVERSAL Release bundle from scratch and then asks it every
# question this repo knows how to ask. Each check answers one none of the
# others can:
#
#   shaders       does every shader compile, through a real GLSL compiler,
#                 before a host has to find out. A shader that will not
#                 compile presents to an operator as "the effect does
#                 nothing", with the real message buried in a log file.
#   --still       a still eye on an RGB wheel returns the input, bitwise, at
#                 every wheel speed, on a moving clip, through the gamma round
#                 trip and on a custom wheel. The headline claim: the fringe is
#                 in the eye, not the projector.
#   --separation  under pursuit the red and green fields' centroids land
#                 v( t_G - t_R ) apart, measured from the picture against the
#                 wheel's stated widths; whole-pixel cases are an exact
#                 translation, fractional ones hold to the derived 8-bit bound,
#                 and 2x is half of 1x.
#   --energy      the per-channel sum is independent of the eye's velocity:
#                 displacement moves light and never makes or loses it.
#   --white       an RGBW wheel raises a grey by White Gain times the W
#                 segment's share of the red one, from the stated widths, and
#                 leaves a saturated primary at exactly what it was.
#   --converge    three-chip: a whole-pixel panel offset translates that
#                 channel bitwise; a fractional one moves its centroid by the
#                 offset.
#   --saccade     the onset detector is primed (nothing fires on frame one),
#                 one hit is one saccade on the frame it lands, and the fringe
#                 decays by e^-1 over Saccade Time.
#   --bits        eight bit planes are the input and four are the 4-bit input,
#                 bitwise; under motion the planes come apart.
#   --track       the Track eye reads a one-cell-per-frame scroll as one cell
#                 a frame, and a still picture as still.
#   --resize      a resize mid-run: the still eye still returns the input on
#                 the first new frame, and the Track eye starts from zero.
#   --names       no parameter name a host would truncate.
#   --negative    every one of those can fail (ten perturbations, twelve
#                 assertions): a DMD that interpolates, the
#                 wrong segment times, taps that sum past one, a wrong W
#                 share, an offset a pixel out, an unprimed detector, a wrong
#                 Saccade Time, four planes judged as eight, two cells judged
#                 as one.
#   --pipe        the fleet's filming format: a still eye through the pipe
#                 is the input bitwise, a cue acts from its frame and not
#                 before, Fire fires from a cue, a partial frame is dropped,
#                 an unknown name is refused.
#   sweep.py      that no control is silently dead.
#   registration  that the bundle contains a plugin at all -- a file-scope
#                 CFFGLPluginInfo nothing names, which a linker may drop while
#                 still producing a bundle that loads and exports plugMain.
#   lipo          is the macOS build really universal.
#   plist         does CFBundleExecutable name the binary that is on disk.
#   codesign      the exact command the release job runs, against a copy.
#   oxbow         the name, the id and the type a HOST sees.
#   --bench       the render cost, for the record. Not pass/fail.
#
# Every picture check runs at 320x180 -- what CI's GPU-less runner uses --
# and at 1280x720.
set -uo pipefail

cd "$(dirname "$0")/.."

BUILD="${BUILD:-build-universal}"

failures=()

step() { printf '\n\033[1m== %s\033[0m\n' "$1"; }
fail() { printf '   \033[31mFAIL\033[0m %s\n' "$1"; failures+=("$1"); }
pass() { printf '   \033[32mok\033[0m   %s\n' "$1"; }

#---------------------------------------------------------------------------
# Every shader, through a real GLSL compiler, before a host has to find out.
#
# --target-env=opengl4.5 with -fauto-map-locations: glslc targets SPIR-V,
# which demands an explicit layout( location ) on every uniform and varying.
# Those are Vulkan rules and not GLSL ones.
#
# glslc is optional -- `brew install shaderc` -- so a machine without it skips
# rather than fails.
#---------------------------------------------------------------------------
shaders_compile() {
	local dir bad=0 n=0 shader

	if ! command -v glslc >/dev/null 2>&1; then
		printf '   skipped: glslc not installed (brew install shaderc)\n'
		return 0
	fi

	dir="$( mktemp -d )"

	python3 - "$dir" <<'SHADERS_PY'
import re, sys, pathlib
out = pathlib.Path( sys.argv[ 1 ] )

FILES = [
	"source/Shaders.cpp",
]

# A shader may be several ADJACENT raw strings -- MSVC caps one literal at
# about 16 KB -- so everything up to the terminating semicolon is joined.
named = {}
for f in FILES:
	text = pathlib.Path( f ).read_text()
	for m in re.finditer( r'(\w+)\s*=\s*((?:\s*(?://[^\n]*\n)*\s*R"\(.*?\)")+)\s*;', text, re.S ):
		named[ m.group( 1 ) ] = "".join( re.findall( r'R"\((.*?)\)"', m.group( 2 ), re.S ) )

def emit( name, body ):
	ext = ".vert" if re.search( r"\bgl_Position\s*=", body ) else ".frag"
	( out / ( name + ext ) ).write_text( body )

for name, body in named.items():
	if body.lstrip().startswith( "#version" ) and "void main" in body:
		emit( name, body )
SHADERS_PY

	for shader in "$dir"/*.vert "$dir"/*.frag; do
		[ -e "$shader" ] || continue
		n=$(( n + 1 ))
		if ! glslc --target-env=opengl4.5 -fauto-map-locations \
			   "$shader" -o /dev/null 2>"$dir/err"; then
			printf '   %s does not compile\n' "$( basename "$shader" )"
			sed "s|$dir/||; s|^|      |" "$dir/err"
			bad=$(( bad + 1 ))
		fi
	done

	if [ "$n" -eq 0 ]; then
		printf '   no shaders were extracted -- the extraction has gone stale\n'
		rm -rf "$dir"
		return 1
	fi

	if [ "$bad" -eq 0 ]; then
		printf '   %d shaders, all compile\n' "$n"
	fi
	rm -rf "$dir"
	return "$bad"
}

step "shaders: every one through a real GLSL compiler"
if shaders_compile; then
	pass "every shader compiles"
else
	fail "a shader does not compile"
fi

#---------------------------------------------------------------------------
# The build being verified. Universal, Release, from a clean configure.
#---------------------------------------------------------------------------
step "build: a fresh universal Release build"
rm -rf "$BUILD"
if cmake -B "$BUILD" -DCMAKE_BUILD_TYPE=Release >/dev/null 2>&1 \
	&& cmake --build "$BUILD" --parallel >/dev/null 2>&1; then
	pass "configures and builds"
else
	fail "the universal build failed -- run: cmake -B $BUILD -DCMAKE_BUILD_TYPE=Release && cmake --build $BUILD"
	printf '\nFAILURES:\n'
	printf '  %s\n' "${failures[@]}"
	exit 1
fi

TEST="$BUILD/whtest"

#---------------------------------------------------------------------------
# The claims, and the negative controls.
#---------------------------------------------------------------------------
for check in still separation energy white converge saccade bits track resize names negative; do
	step "$check"
	if "$TEST" --"$check" > /tmp/wheel-$check.txt 2>&1; then
		grep -E '^   (ok|FAIL)' /tmp/wheel-$check.txt | sed 's/^   //' | sed 's/^/   /'
		tail -2 /tmp/wheel-$check.txt | head -1
		pass "whtest --$check"
	else
		cat /tmp/wheel-$check.txt
		fail "whtest --$check"
	fi
done

step "sweep: no control silently dead"
if python3 tools/sweep.py --binary "$TEST" > /tmp/wheel-sweep.txt 2>&1; then
	tail -1 /tmp/wheel-sweep.txt
	pass "every control reaches the picture"
else
	printf '   *** dead controls, see /tmp/wheel-sweep.txt\n'
	tail -6 /tmp/wheel-sweep.txt
	fail "tools/sweep.py reports a dead control"
fi

#---------------------------------------------------------------------------
# --pipe, the way a filming script uses it: raw RGBA in, raw RGBA out. A
# still eye through the pipe is the input bitwise (the --still claim, now
# through the pipe's own flip and clock); a cue changes the picture from
# its frame and not before; Fire fires from a cue; a partial frame at EOF
# is dropped; an unknown name is refused before a byte is written.
#---------------------------------------------------------------------------
step "pipe: raw frames in, raw frames out, driven by cues"
if python3 - "$TEST" <<'PIPE_PY'
import random, subprocess, sys, tempfile, pathlib
test = sys.argv[ 1 ]
W, H, N = 64, 36, 6
size = W * H * 4
rng = random.Random( 7 )
frames = [ bytes( b for _ in range( W * H ) for b in ( rng.randrange( 256 ), rng.randrange( 256 ), rng.randrange( 256 ), 255 ) ) for _ in range( N ) ]
stream = b"".join( frames ) + frames[ 0 ][ :100 ]
bad = 0

def run( cues ):
    with tempfile.TemporaryDirectory() as d:
        command = [ test, "--pipe", "--size", f"{W}x{H}", "--fps", "30" ]
        if cues is not None:
            p = pathlib.Path( d ) / "cues.txt"
            p.write_text( cues )
            command += [ "--script", str( p ) ]
        r = subprocess.run( command, input=stream, capture_output=True )
        return r.returncode, r.stdout

def same( out ):
    return [ out[ f * size:( f + 1 ) * size ] == frames[ f ] for f in range( len( out ) // size ) ]

def check( ok, what ):
    global bad
    print( f"   {'ok  ' if ok else 'FAIL'} {what}" )
    bad += 0 if ok else 1

code, out = run( None )
check( code == 0 and len( out ) == N * size, f"{N} whole frames in, {N} out, partial frame dropped (got {len( out ) / size:g})" )
check( all( same( out ) ) and len( out ) == N * size, "still eye through the pipe: output == input, bitwise" )

code, out = run( "0 Pursuit Speed 0.25\n3 Eye Mode 1\n" )
check( code == 0 and same( out ) == [ True ] * 3 + [ False ] * 3, f"'3 Eye Mode 1': frames 0-2 untouched, 3-5 fringed ({same( out )})" )

code, out = run( "4 Fire 1\n5 Fire 0\n" )
check( code == 0 and same( out )[ :5 ] == [ True ] * 4 + [ False ], f"'4 Fire 1': a saccade from frame 4 and not before ({same( out )})" )

code, out = run( "0 Eye Mdoe 1\n" )
check( code == 2 and len( out ) == 0, "an unknown name is refused, exit 2, nothing written" )
sys.exit( 1 if bad else 0 )
PIPE_PY
then
	pass "whtest --pipe"
else
	fail "whtest --pipe"
fi

step "bench: the render cost, for the record"
"$TEST" --bench --frames 60 2>&1 | sed 's/^/   /'

#---------------------------------------------------------------------------
# The bundle, as a host meets it.
#---------------------------------------------------------------------------
BUNDLE="$BUILD/Wheel.bundle"
BIN="$BUNDLE/Contents/MacOS/Wheel"

if [ "$(uname)" = "Darwin" ] && [ -d "$BUNDLE" ]; then
	step "registration"
	# Captured, then matched -- never `nm ... | grep -q`. Under pipefail a
	# grep -q that FINDS its match exits at once, nm takes SIGPIPE, and the
	# pipeline reports failure on a correct binary.
	symbols=$( nm -gU "$BIN" 2>/dev/null )
	case "$symbols" in
		*_plugMain*) pass "exports plugMain" ;;
		*) fail "no plugMain -- the host will load the bundle and find no plugins" ;;
	esac

	step "lipo: universal"
	archs=$( lipo -archs "$BIN" 2>/dev/null )
	printf '   architectures: %s\n' "$archs"
	case "$archs" in *arm64*) pass "arm64 present" ;; *) fail "no arm64 (got: $archs)" ;; esac
	case "$archs" in *x86_64*) pass "x86_64 present" ;; *) fail "no x86_64 (got: $archs) -- a universal build was asked for" ;; esac

	step "plist"
	exe=$( /usr/libexec/PlistBuddy -c "Print :CFBundleExecutable" "$BUNDLE/Contents/Info.plist" 2>/dev/null )
	if [ -n "$exe" ] && [ -f "$BUNDLE/Contents/MacOS/$exe" ]; then
		pass "CFBundleExecutable ($exe) is on disk"
	else
		fail "CFBundleExecutable is '$exe' but no such binary exists -- codesign will fail after the tag"
	fi
	ident=$( /usr/libexec/PlistBuddy -c "Print :CFBundleIdentifier" "$BUNDLE/Contents/Info.plist" 2>/dev/null )
	if [ "$ident" = "com.stoatworks.ffgl.wheel" ]; then
		pass "CFBundleIdentifier is $ident"
	else
		fail "CFBundleIdentifier is '$ident', expected com.stoatworks.ffgl.wheel"
	fi

	step "codesign"
	tmp=$( mktemp -d )
	cp -R "$BUNDLE" "$tmp/" 2>/dev/null
	if codesign --force --sign - --timestamp=none "$tmp/Wheel.bundle" >/dev/null 2>&1; then
		pass "ad-hoc signs (the command the release job runs)"
	else
		fail "ad-hoc signing failed"
		codesign --force --sign - --timestamp=none "$tmp/Wheel.bundle" 2>&1 | sed 's/^/       /'
	fi
	rm -rf "$tmp"

	step "oxbow: the name, id and type a host sees"
	OXBOW="${OXBOW:-../oxbow/build/oxbow}"
	[ -x "$OXBOW" ] || OXBOW="$HOME/Projects/resolume/oxbow/build/oxbow"
	if [ -x "$OXBOW" ]; then
		out=$( "$OXBOW" probe "$BUNDLE" 2>&1 )
		printf '%s\n' "$out" | sed -n '1,8p' | sed 's/^/   /'
		case "$out" in *"SW Wheel"*) pass "name is SW Wheel" ;; *) fail "the host does not see the name SW Wheel" ;; esac
		case "$out" in *WH01*) pass "id is WH01" ;; *) fail "the host does not see the id WH01" ;; esac
		case "$out" in *"type:        effect"*) pass "type is effect" ;; *) fail "the host does not see an effect" ;; esac
	else
		printf '   skipped: oxbow not built at %s\n' "$OXBOW"
	fi
fi

printf '\n'
if (( ${#failures[@]} == 0 )); then
	printf '\033[32mall checks passed\033[0m\n'
	exit 0
fi

printf '\033[31mFAILURES:\033[0m\n'
printf '  %s\n' "${failures[@]}"
exit 1
