#!/bin/bash
# c0attr.sh <results dir> -- after prepare.sh stack l3a l3b l3c l3po l1a l1b l1c l1eo:
# deflate of stack and each commit of the l3 and l1 branches, and of the
# parse-only and encoder-only builds.
set -e
B=$(cd $(dirname $0) && pwd)
out=$1
mkdir -p $out
cd ~/isal
bash $B/cpu.sh | tee $out/cpu.txt
gcc -O2 -I stack/include $B/ab.c -o ab -ldl -lz
L=$HOME/isal
for i in 1 2; do
	./ab -r 9 -c defl $L/stack/bin/libisal.so $L/l3a/bin/libisal.so $L/l3b/bin/libisal.so \
		$L/l3c/bin/libisal.so -- ~/corpus/silesia.tar ~/corpus/silesia/* | tee $out/abD.$i.txt
	./ab -r 9 -c defl $L/stack/bin/libisal.so $L/l1a/bin/libisal.so $L/l1b/bin/libisal.so \
		$L/l1c/bin/libisal.so -- ~/corpus/silesia.tar ~/corpus/silesia/* | tee $out/abE.$i.txt
	./ab -r 9 -c defl $L/stack/bin/libisal.so $L/l3po/bin/libisal.so $L/l1eo/bin/libisal.so \
		-- ~/corpus/silesia.tar ~/corpus/silesia/* | tee $out/abF.$i.txt
done
