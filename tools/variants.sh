#!/usr/bin/env bash
# Build or run extra binaries that swap one library for another ref.
#   tools/variants.sh build NAME...   ->  ./bench-NAME
#   tools/variants.sh run NAME...     ->  that library and the system zlib (as a control)
set -u
mode=$1
shift
DATA="corpus/SRR2589044_1.fastq corpus/silesia.cat"
ME=https://github.com/ptweezy/isa-l
for v in "$@"; do
	case $v in
	zng-dev) vars="ZNG_REF=develop" only="zlib-ng,system zlib" ;;
	isal-master) vars="ISAL_REF=master" only="ISA-L,system zlib" ;;
	isal-465) vars="ISAL_REPO=$ME ISAL_REF=igzip-aarch64-inflate-fast-path" only="ISA-L,system zlib" ;;
	isal-c0) vars="ISAL_REPO=$ME ISAL_REF=c0-infl" only="ISA-L,system zlib" ;;
	zlib-develop) vars="ZLIB_REF=develop" only="zlib @,system zlib" ;;
	zlib-793) vars="ZLIB_REF=refs/pull/793/head ZLIB_LABEL=PR-793" only="zlib @,system zlib" ;;
	zlib-v1) vars="ZLIB_REPO=https://github.com/ptweezy/zlib ZLIB_REF=inffast-next ZLIB_LABEL=v1-preload" only="zlib @,system zlib" ;;
	zlib-v2) vars="ZLIB_REPO=https://github.com/ptweezy/zlib ZLIB_REF=inffast-zng-format ZLIB_LABEL=v2-one-shift" only="zlib @,system zlib" ;;
	*) echo "unknown variant $v" >&2; exit 2 ;;
	esac
	if [ "$mode" = build ]; then
		env $vars WORK="$PWD/work-$v" OUT="$PWD/bench-$v" bash build.sh || echo "variant $v did not build"
	else
		echo "#### variant: $v"
		if [ -x "./bench-$v" ]; then
			# shellcheck disable=SC2086
			"./bench-$v" --max-mb 256 --rounds "${ROUNDS:-9}" --only "$only" $DATA
		else
			echo "variant $v did not build"
		fi
	fi
done
