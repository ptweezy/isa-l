#!/bin/bash
# adler2.sh <results dir> -- after prepare.sh master pr445: the adler32 kernels
# of #445, its checker, and Silesia deflate/inflate of master and #445.
set -e
B=$(cd $(dirname $0) && pwd)
out=$1
mkdir -p $out
cd ~/isal
bash $B/cpu.sh | tee $out/cpu.txt

gcc -O2 -DNO_CRC32_ISCSI_FUSED $B/kab.c pr445/bin/isa-l.a -o kab_pr445
for i in 1 2 3; do
	./kab_pr445 | tee -a $out/kab_pr445.txt
done

gcc -O2 $B/t445.c pr445/bin/isa-l.a -o t445_pr445
addr() { nm t445_pr445 | awk -v s="$1" '$3==s {print "0x"$1}'; }
oa=$(($(addr isal_adler32_dispatched) - $(addr isal_adler32)))
oc=$(($(addr crc32_iscsi_dispatched) - $(addr crc32_iscsi)))
./t445_pr445 $oa $oc 1 | tee $out/t445_pr445.txt

gcc -O2 -I master/include $B/ab.c -o ab -ldl -lz
for i in 1 2 3; do
	./ab -r 11 $HOME/isal/master/bin/libisal.so $HOME/isal/pr445/bin/libisal.so \
		-- ~/corpus/silesia.tar ~/corpus/silesia/* | tee $out/ab.$i.txt
done
