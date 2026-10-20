#!/bin/bash
# x64.sh <results dir> -- after prepare.sh master pr444 pr445 crc:
# kernel A/B of #445's adler32 and the Zen 5 crc32_iscsi branch, correctness
# checks of both kernels, then Silesia deflate/inflate of master, #444 and #445.
set -e
B=$(cd $(dirname $0) && pwd)
out=$1
mkdir -p $out
cd ~/isal
bash $B/cpu.sh | tee $out/cpu.txt

nm pr445/bin/isa-l.a | grep -q " T adler32_avx512$"
nm crc/bin/isa-l.a | grep -q " T crc32_iscsi_by16_10_fused$"
gcc -O2 -DNO_CRC32_ISCSI_FUSED $B/kab.c pr445/bin/isa-l.a -o kab_pr445
gcc -O2 -DNO_ADLER32_AVX512 $B/kab.c crc/bin/isa-l.a -o kab_crc
for i in 1 2 3; do
	./kab_pr445 | tee -a $out/kab_pr445.txt
	./kab_crc | tee -a $out/kab_crc.txt
done

# The checker calls the AVX-512 kernels directly, so it needs a CPU that has them
if grep -q "avx512 group 2 1" $out/kab_pr445.txt; then
	for t in pr445 crc; do
		gcc -O2 $B/t445.c $t/bin/isa-l.a -o t445_$t
		addr() { nm t445_$t | awk -v s="$1" '$3==s {print "0x"$1}'; }
		oa=$(($(addr isal_adler32_dispatched) - $(addr isal_adler32)))
		oc=$(($(addr crc32_iscsi_dispatched) - $(addr crc32_iscsi)))
		./t445_$t $oa $oc 1 | tee $out/t445_$t.txt
	done
fi

gcc -O2 -I master/include $B/ab.c -o ab -ldl -lz
for i in 1 2 3; do
	./ab -r 11 $HOME/isal/master/bin/libisal.so $HOME/isal/pr444/bin/libisal.so \
		$HOME/isal/pr445/bin/libisal.so -- ~/corpus/silesia.tar ~/corpus/silesia/* |
		tee $out/ab.$i.txt
done
