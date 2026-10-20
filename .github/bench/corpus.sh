#!/bin/bash
# corpus.sh -- unpack the Silesia corpus into ~/corpus/silesia and tar it
# into ~/corpus/silesia.tar, downloading silesia.zip if the cache missed.
set -e
mkdir -p ~/corpus
cd ~/corpus
[ -f silesia.zip ] ||
	curl -fsSL --retry 5 -o silesia.zip https://sun.aei.polsl.pl/~sdeor/corpus/silesia.zip
rm -rf silesia
mkdir silesia
unzip -q silesia.zip -d silesia
tar -cf silesia.tar -C silesia .
ls -l silesia.tar
