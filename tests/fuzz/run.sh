#!/usr/bin/env bash
#
# libFuzzer harnesses for fpm_proctitle (clang, NTS PHP headers).
#
#   tests/fuzz/run.sh [build]          # build the targets into tests/fuzz/build/
#   tests/fuzz/run.sh regress          # run every target once over its corpus
#   tests/fuzz/run.sh fuzz [SECONDS]   # fuzz each target for SECONDS (default 60)
#
# Override the toolchain with CC=... and PHP_CONFIG=..., pick targets with TARGETS="title".
#
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
BUILD="$HERE/build"
CC="${CC:-clang}"
PHP_CONFIG="${PHP_CONFIG:-php-config}"
TARGETS="${TARGETS:-title proc_stat}"

build() {
	mkdir -p "$BUILD"
	local t
	for t in $TARGETS; do
		# shellcheck disable=SC2046
		"$CC" -g -O1 -fsanitize=fuzzer,address,undefined -fno-sanitize-recover=all \
			$("$PHP_CONFIG" --includes) -o "$BUILD/fuzz_$t" "$HERE/fuzz_$t.c" "$HERE/stubs.c"
	done
}

dict_opt() { # $1 = target
	if [ -f "$HERE/$1.dict" ]; then echo "-dict=$HERE/$1.dict"; fi
}

regress() {
	local t
	for t in $TARGETS; do
		"$BUILD/fuzz_$t" -runs=0 "$HERE/corpus/$t" 2>&1 | tail -n 1 | sed "s/^/# $t: /"
	done
}

fuzz() { # $1 = seconds
	local t work rc=0
	work="$(mktemp -d)"
	trap 'rm -rf "$work"' RETURN
	for t in $TARGETS; do
		# fuzz a copy: the committed corpus only holds hand-picked seeds
		mkdir "$work/$t"
		cp "$HERE/corpus/$t"/* "$work/$t/"
		echo "=== $t (${1}s)"
		# shellcheck disable=SC2046
		"$BUILD/fuzz_$t" -max_total_time="$1" -print_final_stats=1 \
			-artifact_prefix="$BUILD/" $(dict_opt "$t") "$work/$t" || rc=1
	done
	return $rc
}

case "${1:-build}" in
	build)   build ;;
	regress) build && regress ;;
	fuzz)    build && fuzz "${2:-60}" ;;
	*) echo "usage: $0 [build|regress|fuzz [SECONDS]]" >&2; exit 1 ;;
esac
