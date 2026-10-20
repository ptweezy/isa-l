#!/bin/bash
# prepare.sh name:commit ... -- extract each commit into ~/isal/<name> and
# build its static and shared libraries.
set -e
for spec in "$@"; do
	name=${spec%%:*}
	commit=${spec#*:}
	rm -rf ~/isal/$name
	mkdir -p ~/isal/$name
	git archive $commit | tar -x -C ~/isal/$name
	echo "$name: $(git log -1 --format='%h %s' $commit)"
	make -C ~/isal/$name -f Makefile.unx -j $(nproc) lib slib > ~/isal/$name.build.log 2>&1 ||
		{ tail -30 ~/isal/$name.build.log; exit 1; }
done
