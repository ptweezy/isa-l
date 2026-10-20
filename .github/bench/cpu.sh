#!/bin/bash
# cpu.sh -- print the runner's CPU, and the flags the x86 kernels dispatch on
if [ -r /proc/cpuinfo ]; then
	lscpu | grep -E "^(Architecture|CPU\(s\)|Model name|Vendor ID|CPU family|Model|Stepping|Thread|Core|Socket|L[123])" || true
	flags=$(grep -m1 -E "^(flags|Features)" /proc/cpuinfo | cut -d: -f2)
	for f in avx2 avx512f avx512vl avx512bw avx512_vnni avx512_vbmi2 gfni vaes vpclmulqdq avx512_bitalg avx512_vpopcntdq asimd pmull crc32 sve sve2; do
		case " $flags " in
		*" $f "*) printf "%s " $f ;;
		esac
	done
	echo
fi
