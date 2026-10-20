#!/bin/bash
# arm64.sh <results dir> -- after prepare.sh master pr440 pr444:
# Silesia deflate/inflate of master, #440 and #444.
set -e
B=$(cd $(dirname $0) && pwd)
out=$1
mkdir -p $out
cd ~/isal
bash $B/cpu.sh | tee $out/cpu.txt
gcc -O2 -I master/include $B/ab.c -o ab -ldl -lz
for i in 1 2 3; do
	./ab -r 11 $HOME/isal/master/bin/libisal.so $HOME/isal/pr440/bin/libisal.so \
		$HOME/isal/pr444/bin/libisal.so -- ~/corpus/silesia.tar ~/corpus/silesia/* |
		tee $out/ab.$i.txt
done
