#!/usr/bin/env bash
# Fetch each library at a pinned ref, build it with its own release settings,
# and link the benchmark. Override a *_REPO / *_REF variable to test a branch,
# with WORK and OUT set so that it gets its own build tree and binary:
#   ZNG_REF=develop WORK=work-zng-dev OUT=bench-zng-dev ./build.sh
# To try another zlib only, SHARE=<a finished work dir> reuses that build of
# zlib-ng, ISA-L and libdeflate instead of building them again.
set -euo pipefail

ROOT=$(cd "$(dirname "$0")" && pwd)
WORK=${WORK:-$ROOT/work}
SHARE=${SHARE:-}
OTHERS=${SHARE:-$WORK}
JOBS=${JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)}
CC=${CC:-cc}
OUT=${OUT:-$ROOT/bench}

ZLIB_REPO=${ZLIB_REPO:-https://github.com/madler/zlib}
ZLIB_REF=${ZLIB_REF:-v1.4}
ZLIB_LABEL=${ZLIB_LABEL:-$ZLIB_REF}
ZNG_REPO=${ZNG_REPO:-https://github.com/zlib-ng/zlib-ng}
ZNG_REF=${ZNG_REF:-2.3.3}
ISAL_REPO=${ISAL_REPO:-https://github.com/intel/isa-l}
ISAL_REF=${ISAL_REF:-v2.32.1}
LIBDEFLATE_REPO=${LIBDEFLATE_REPO:-https://github.com/ebiggers/libdeflate}
LIBDEFLATE_REF=${LIBDEFLATE_REF:-v1.26}

fetch() { # name repo ref
	local dir=$WORK/$1
	if [ ! -d "$dir/.git" ]; then
		mkdir -p "$dir"
		git -C "$dir" init -q
		git -C "$dir" remote add origin "$2"
	fi
	git -C "$dir" fetch -q --depth 1 origin "$3"
	git -C "$dir" -c advice.detachedHead=false checkout -q -f FETCH_HEAD
	echo "$1: $2 @ $3 ($(git -C "$dir" rev-parse --short HEAD))"
}

mkdir -p "$WORK"
fetch zlib "$ZLIB_REPO" "$ZLIB_REF"

echo "== zlib (z_ prefix) =="
(cd "$WORK/zlib" && git clean -qfdx && ./configure --static --zprefix >/dev/null &&
	make -j"$JOBS" libz.a >/dev/null)

if [ -z "$SHARE" ]; then
	fetch zlib-ng "$ZNG_REPO" "$ZNG_REF"
	fetch isa-l "$ISAL_REPO" "$ISAL_REF"
	fetch libdeflate "$LIBDEFLATE_REPO" "$LIBDEFLATE_REF"

	echo "== zlib-ng (native API) =="
	ZNG_ARGS=(-DCMAKE_BUILD_TYPE=Release -DZLIB_COMPAT=OFF -DBUILD_SHARED_LIBS=OFF
		-DZLIB_ENABLE_TESTS=OFF -DZLIBNG_ENABLE_TESTS=OFF -DWITH_GTEST=OFF)
	cmake -S "$WORK/zlib-ng" -B "$WORK/zlib-ng/build" "${ZNG_ARGS[@]}" >"$WORK/zlib-ng-cmake.log"
	cmake --build "$WORK/zlib-ng/build" -j"$JOBS" >/dev/null
	grep -E "Architecture-specific source files" "$WORK/zlib-ng-cmake.log" | sed 's/^/  /' || true

	echo "== ISA-L =="
	# Makefile.unx does not recognise "arm64" (macOS) and would build the
	# portable C code, so name the architecture explicitly, as python-isal does.
	ISAL_ARGS=()
	case "$(uname -m)" in
	arm64 | aarch64) ISAL_ARGS=(host_cpu=aarch64 arch=aarch64) ;;
	esac
	(cd "$WORK/isa-l" && git clean -qfdx &&
		make -f Makefile.unx -j"$JOBS" "${ISAL_ARGS[@]}" lib >"$WORK/isa-l-make.log" 2>&1) ||
		{ tail -30 "$WORK/isa-l-make.log"; exit 1; }
	echo "  inflate symbols: $(nm "$WORK/isa-l/bin/isa-l.a" 2>/dev/null | grep -i -E " T _?decode_huffman_code_block_stateless" | awk '{print $3}' | sort -u | tr '\n' ' ')"

	echo "== libdeflate =="
	cmake -S "$WORK/libdeflate" -B "$WORK/libdeflate/build" -DCMAKE_BUILD_TYPE=Release \
		-DLIBDEFLATE_BUILD_SHARED_LIB=OFF -DLIBDEFLATE_BUILD_GZIP=OFF >/dev/null
	cmake --build "$WORK/libdeflate/build" -j"$JOBS" >/dev/null
else
	echo "== zlib-ng, ISA-L and libdeflate reused from $SHARE =="
fi

echo "== benchmark =="
OBJ=$WORK/obj
rm -rf "$OBJ"
mkdir -p "$OBJ"
CFLAGS="-O2 -std=c11 -Wall -Wextra -I$ROOT/src"
isal_ver="${ISAL_REF#v} ($(git -C "$OTHERS/isa-l" rev-parse --short HEAD))"
$CC $CFLAGS -c "$ROOT/src/main.c" -o "$OBJ/main.o"
$CC $CFLAGS -c "$ROOT/src/prof.c" -o "$OBJ/prof.o"
$CC $CFLAGS -c "$ROOT/src/be_syszlib.c" -o "$OBJ/be_syszlib.o"
$CC $CFLAGS -I"$WORK/zlib" -DZ_PREFIX -DMADLER_NAME="\"zlib @$ZLIB_LABEL\"" -c "$ROOT/src/be_madler.c" -o "$OBJ/be_madler.o"
$CC $CFLAGS -Wno-deprecated-declarations -I"$OTHERS/zlib-ng/build" -I"$OTHERS/zlib-ng" \
	-DZNG_NAME="\"zlib-ng @$ZNG_REF\"" -c "$ROOT/src/be_zng.c" -o "$OBJ/be_zng.o"
$CC $CFLAGS -I"$OTHERS/isa-l/include" -DISAL_VERSION_STR="\"$isal_ver\"" -c "$ROOT/src/be_isal.c" -o "$OBJ/be_isal.o"
$CC $CFLAGS -I"$OTHERS/libdeflate" -c "$ROOT/src/be_libdeflate.c" -o "$OBJ/be_libdeflate.o"
$CC $CFLAGS -c "$ROOT/src/be_apple_compression.c" -o "$OBJ/be_apple_compression.o"
LIBS="-lz"
[ "$(uname -s)" = Darwin ] && LIBS="$LIBS -lcompression"
$CC -o "$OUT" "$OBJ"/*.o "$WORK/zlib/libz.a" "$OTHERS/zlib-ng/build/libz-ng.a" \
	"$OTHERS/isa-l/bin/isa-l.a" "$OTHERS/libdeflate/build/libdeflate.a" $LIBS
echo "built $OUT"
