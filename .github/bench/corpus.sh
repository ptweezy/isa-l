#!/bin/bash
# corpus.sh -- unpack the Silesia corpus into ~/corpus/silesia and tar it
# into ~/corpus/silesia.tar, downloading silesia.zip if the cache missed.
# The tar is the same on every runner: sorted names, fixed times and owners.
set -e
mkdir -p ~/corpus
cd ~/corpus
[ -f silesia.zip ] ||
	curl -fsSL --retry 5 -o silesia.zip https://sun.aei.polsl.pl/~sdeor/corpus/silesia.zip
rm -rf silesia
mkdir silesia
unzip -q silesia.zip -d silesia
tar --sort=name --mtime=@0 --owner=0 --group=0 --numeric-owner -cf silesia.tar -C silesia .
ls -l silesia.tar
sha256sum silesia.tar
