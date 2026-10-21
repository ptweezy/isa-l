#!/bin/bash
# c0.sh <results dir> -- after prepare.sh master stack infl l1 l3 huff crc all:
# crc kernel A/B of the crc branch (x64), then Silesia deflate/inflate of
# master, stack and all; of stack and the infl, l1 and l3 branches; and of
# stack and the huff branch on small inputs.
set -e
B=$(cd $(dirname $0) && pwd)
out=$1
mkdir -p $out
cd ~/isal
bash $B/cpu.sh | tee $out/cpu.txt

if [ "$(uname -m)" = x86_64 ]; then
	gcc -O2 $B/ckab.c crc/bin/isa-l.a -o ckab
	for i in 1 2 3; do
		./ckab | tee -a $out/ckab.txt
	done
fi

gcc -O2 -I master/include $B/ab.c -o ab -ldl -lz
L=$HOME/isal
for i in 1 2; do
	./ab -r 9 $L/master/bin/libisal.so $L/stack/bin/libisal.so $L/all/bin/libisal.so \
		-- ~/corpus/silesia.tar ~/corpus/silesia/* | tee $out/abA.$i.txt
	./ab -r 9 $L/stack/bin/libisal.so $L/infl/bin/libisal.so $L/l1/bin/libisal.so \
		$L/l3/bin/libisal.so -- ~/corpus/silesia.tar ~/corpus/silesia/* | tee $out/abB.$i.txt
	./ab -r 9 -s $L/stack/bin/libisal.so $L/huff/bin/libisal.so \
		-- ~/corpus/silesia.tar ~/corpus/silesia/* | tee $out/abC.$i.txt
done
