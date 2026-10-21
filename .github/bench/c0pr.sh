#!/bin/bash
# c0pr.sh <results dir> <group> -- builds the commits a group needs and times
# each PR against its base with two libraries per ab run (base first, PR second).
#   defl: master vs short-match-hash, icf-encode-vbmi, lvl3-parse-asm,
#         lvl3-map-no-gather and lvl3-extend-16; map-no-gather vs extend-16
#   infl: master vs inflate-dist-lookup, inflate-copy32, pending-lits-lookback
#         and one-shift-tables; 458e249 vs one-shift-tables; one-shift-tables
#         vs inflate-near-end
#   inflarm: the inflate PRs that change aarch64 code: pending-lits-lookback,
#         one-shift-tables (vs master and vs 458e249)
#   huff: 8fa1e72 vs #444; #444 vs each Huffman PR, on 1K, 4K and 16K inputs
set -e
B=$(cd $(dirname $0) && pwd)
out=$1
group=$2
mkdir -p $out
MASTER=469c7d0735f8cf9330622b111a5dc48eb8a09b7e
M8=8fa1e72fee7338fcfbf829b00f2f407631bd2dba
SMH=a07afc70820af43e0bf6fbb5361d69135b4ef754
VBMI=bf99d0865ee58b2c3dfbf2db7fca0acfc0d36009
PARSE=ebda6008774f6fc62f2e490ecd8bbab39c143d70
NOGATHER=67dcdcb10d0b1e256b2a6c155ba855ffe4e553d0
EXT16=bb78bdc1bd0946beac8268e83a1b1f70e6a2040e
DIST=4b9be0c5c6f5325e46f202aadbd5c51582ece6e4
COPY32=b579067345e60c647858740cdb8f22e1bea356a4
PENDING=1452f395b9ab806b559a9fc47ceb230e6401a518
OSBASE=458e24936eb0e595cfcb03f7091d457fdcc6464e
ONESHIFT=1d85edb000cc0bde13a7df1575e2c9d18bafaf97
NEAREND=66bb6416a3940daed99dce2236a2cf8cf466d8f1
PR444=e292b2028964a6e89d05b6f80f7b9cae20506814
CODELENS=c3ffd164b410989b92e36591962155047f1891e2
TREE=3c6c9607b19c9935a0f7a3f78d608acb56373ff3
RLE=b196f7db2b0e551453ea8144a0cb6d78c382ce08

case $group in
defl) builds="master:$MASTER smh:$SMH vbmi:$VBMI parse:$PARSE nogather:$NOGATHER ext16:$EXT16"
	pairs="master,smh,defl_L master,vbmi,defl_L master,parse,defl_L3 master,nogather,defl_L3
		nogather,ext16,defl_L3 master,ext16,defl_L3" ;;
infl) builds="master:$MASTER dist:$DIST copy32:$COPY32 pending:$PENDING osbase:$OSBASE oneshift:$ONESHIFT nearend:$NEAREND"
	pairs="master,dist,infl master,copy32,infl master,pending,infl osbase,oneshift,infl
		master,oneshift,infl oneshift,nearend,infl" ;;
inflarm) builds="master:$MASTER pending:$PENDING osbase:$OSBASE oneshift:$ONESHIFT"
	pairs="master,pending,infl osbase,oneshift,infl master,oneshift,infl" ;;
huff) builds="m8:$M8 pr444:$PR444 codelens:$CODELENS tree:$TREE rle:$RLE"
	pairs="m8,pr444,small_defl pr444,codelens,small_defl pr444,tree,small_defl pr444,rle,small_defl" ;;
*) echo "unknown group $group"; exit 1 ;;
esac

bash $B/prepare.sh $builds
cd ~/isal
bash $B/cpu.sh | tee $out/cpu.txt
gcc -O2 -I $(ls -d */include | head -1) $B/abx.c -o ab -ldl -lz
L=$HOME/isal
for i in 1 2; do
	for p in $pairs; do
		IFS=, read a b what <<< "$p"
		case $what in
		defl_L) args=(-c "defl L") ;;
		defl_L3) args=(-c "defl L3") ;;
		infl) args=(-c infl) ;;
		small_defl) args=(-s -z 1,4,16 -c defl) ;;
		esac
		./ab -r 11 "${args[@]}" $L/$a/bin/libisal.so $L/$b/bin/libisal.so \
			-- ~/corpus/silesia.tar ~/corpus/silesia/* | tee $out/ab.$a.$b.$i.txt
	done
done
