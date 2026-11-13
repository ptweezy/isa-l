#!/usr/bin/env bash
# Download the benchmark inputs into ./corpus:
#   silesia.cat          the Silesia corpus, its files concatenated in name order
#   SRR2589044_1.fastq   the first 256 MiB of a real Illumina FASTQ file (E. coli, ENA)
# A corpus that cannot be downloaded is skipped.
set -u
mkdir -p corpus
cd corpus

if [ ! -s silesia.cat ]; then
	for url in https://sun.aei.polsl.pl/~sdeor/corpus/silesia.zip \
		http://mattmahoney.net/dc/silesia.zip; do
		curl -fsSL --retry 2 --max-time 300 -o silesia.zip "$url" && break
		rm -f silesia.zip
	done
	if [ -s silesia.zip ]; then
		rm -rf silesia && mkdir silesia && unzip -q -o silesia.zip -d silesia
		(cd silesia && LC_ALL=C ls | LC_ALL=C sort | while read -r f; do cat "$f"; done) >silesia.cat
		rm -rf silesia silesia.zip
	fi
fi

if [ ! -s SRR2589044_1.fastq ]; then
	if curl -fsSL --retry 3 --max-time 600 -o SRR2589044_1.dl.gz \
		https://ftp.sra.ebi.ac.uk/vol1/fastq/SRR258/004/SRR2589044/SRR2589044_1.fastq.gz; then
		gunzip -c SRR2589044_1.dl.gz 2>/dev/null | head -c 268435456 >SRR2589044_1.fastq
	fi
	rm -f SRR2589044_1.dl.gz
fi

ls -l
