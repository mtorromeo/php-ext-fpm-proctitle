#!/usr/bin/env bash
#
# Integration test for fpm_proctitle.
#
#   tests/integration/run.sh                 # host php-fpm + ./modules/fpm_proctitle.so
#   tests/integration/run.sh docker IMAGE    # e.g. php:7.0-fpm (builds the extension in the container)
#   tests/integration/run.sh matrix          # all images in $IMAGES
#
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
PHP="${PHP:-php}"
IMAGES="${IMAGES:-php:7.0-fpm php:7.1-fpm php:7.4-fpm php:8.0-fpm php:8.3-fpm php:8.5-fpm}"

render_conf() { # $1 = dir for pid/log, $2 = listen www, $3 = listen api, $4 = output, [$5 = template]
	sed -e "s|@DIR@|$1|g" -e "s|@LISTEN@|$2|g" -e "s|@LISTEN2@|$3|g" "$HERE/${5:-pool.conf.in}" > "$4"
}

# second master whose environ is not contiguous (see envgap.c)
GAP_ENV="PT_GAP=original PT_SENTINEL=intact"

free_port() {
	"$PHP" -r '$s = stream_socket_server("tcp://127.0.0.1:0"); $n = stream_socket_get_name($s, false); echo substr($n, strrpos($n, ":") + 1);'
}

wait_port() { # $1 = port
	for _ in $(seq 1 100); do
		if "$PHP" -r 'exit(@fsockopen("127.0.0.1", (int)$argv[1]) ? 0 : 1);' "$1"; then
			return 0
		fi
		sleep 0.1
	done
	echo "php-fpm did not start" >&2
	return 1
}

run_local() {
	local fpm="${PHP_FPM:-$(command -v php-fpm || echo /usr/sbin/php-fpm)}"
	local so="$ROOT/modules/fpm_proctitle.so"
	[ -f "$so" ] || { echo "build the extension first (phpize && ./configure && make)" >&2; exit 1; }

	local port port2 port3 rc=0
	FPM_TMP="$(mktemp -d)"
	port="$(free_port)"
	port2="$(free_port)"
	port3="$(free_port)"
	render_conf "$FPM_TMP" "127.0.0.1:$port" "127.0.0.1:$port2" "$FPM_TMP/php-fpm.conf"
	render_conf "$FPM_TMP" "127.0.0.1:$port3" "" "$FPM_TMP/gap.conf" gap.conf.in
	"${CC:-cc}" -shared -fPIC -o "$FPM_TMP/envgap.so" "$HERE/envgap.c"

	"$fpm" -n -y "$FPM_TMP/php-fpm.conf" -d "extension=$so" -F > /dev/null 2>&1 &
	FPM_PID=$!
	# shellcheck disable=SC2086
	env -i PATH="$PATH" LD_PRELOAD="$FPM_TMP/envgap.so" $GAP_ENV \
		"$fpm" -n -y "$FPM_TMP/gap.conf" -d "extension=$so" -F > /dev/null 2>&1 &
	FPM_PID2=$!
	trap 'kill "$FPM_PID" "$FPM_PID2" 2>/dev/null || true; wait "$FPM_PID" "$FPM_PID2" 2>/dev/null || true; rm -rf "$FPM_TMP"' EXIT
	wait_port "$port"
	wait_port "$port2"
	wait_port "$port3"

	"$PHP" -n "$HERE/check.php" "127.0.0.1:$port" "$HERE/title.php" "127.0.0.1:$port2" "127.0.0.1:$port3" || rc=$?

	# CLI must be unaffected
	local t
	t="$("$PHP" -n -d "extension=$so" -r 'echo explode("\0", file_get_contents("/proc/self/cmdline"))[0];')"
	if [ "$t" = "$PHP" ]; then echo "ok   cli untouched"; else echo "FAIL cli untouched: $t"; rc=1; fi

	[ $rc -eq 0 ] || { echo "--- php-fpm.log"; cat "$FPM_TMP/php-fpm.log" "$FPM_TMP/php-fpm-gap.log" || true; }
	return $rc
}

run_docker() { # $1 = image
	local image="$1" name port port2 port3 rc=0
	name="fpm-proctitle-test-$$-${image//[^a-zA-Z0-9]/_}"
	port="$(free_port)"
	port2="$(free_port)"
	port3="$(free_port)"

	echo "=== $image"
	docker run -d --rm --name "$name" -p "127.0.0.1:$port:9000" -p "127.0.0.1:$port2:9001" -p "127.0.0.1:$port3:9002" \
		-v "$ROOT:/src:ro" "$image" sleep infinity > /dev/null
	trap "docker rm -f '$name' >/dev/null 2>&1 || true" RETURN

	# build in a scratch copy so the host tree stays clean
	docker exec -e TERM=dumb "$name" sh -ec '
		mkdir /build && cd /src
		cp -r config.m4 fpm_proctitle.c php_fpm_proctitle.h /build/
		mkdir /build/tests && cp tests/*.phpt /build/tests/
		cd /build && phpize >/dev/null && ./configure -q >/dev/null && make -s >/dev/null 2>build.err || { cat build.err; exit 1; }
		grep -i warning build.err || true
		make test NO_INTERACTION=1 2>&1 | grep -E "^Tests (passed|failed)" | sed "s/^/# phpt: /"
		! grep -q . tests/*.diff 2>/dev/null
		cc -shared -fPIC -o /build/envgap.so /src/tests/integration/envgap.c
	' || { echo "build failed on $image"; return 1; }

	echo "# libbsd linked: $(docker exec "$name" sh -c 'ldd "$(command -v php-fpm)" 2>/dev/null | grep -c libbsd || true')"

	render_conf "/tmp" "0.0.0.0:9000" "0.0.0.0:9001" "/tmp/pt-$$.conf"
	docker cp "/tmp/pt-$$.conf" "$name:/tmp/php-fpm.conf" > /dev/null
	render_conf "/tmp" "0.0.0.0:9002" "" "/tmp/pt-$$.conf" gap.conf.in
	docker cp "/tmp/pt-$$.conf" "$name:/tmp/gap.conf" > /dev/null
	rm -f "/tmp/pt-$$.conf"

	docker exec -d "$name" php-fpm -n -R -y /tmp/php-fpm.conf -d extension=/build/modules/fpm_proctitle.so -F > /dev/null
	# shellcheck disable=SC2086
	docker exec -d "$name" env -i PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin \
		LD_PRELOAD=/build/envgap.so $GAP_ENV \
		php-fpm -n -R -y /tmp/gap.conf -d extension=/build/modules/fpm_proctitle.so -F > /dev/null
	wait_port "$port"
	wait_port "$port2"
	wait_port "$port3"

	"$PHP" -n "$HERE/check.php" "127.0.0.1:$port" /src/tests/integration/title.php "127.0.0.1:$port2" "127.0.0.1:$port3" docker exec "$name" || rc=$?

	local t
	t="$(docker exec "$name" php -n -d extension=/build/modules/fpm_proctitle.so -r 'echo extension_loaded("fpm_proctitle") ? "loaded" : "missing";')"
	if [ "$t" = "loaded" ]; then echo "ok   cli loads extension"; else echo "FAIL cli: $t"; rc=1; fi

	[ $rc -eq 0 ] || { echo "--- php-fpm.log"; docker exec "$name" cat /tmp/php-fpm.log /tmp/php-fpm-gap.log || true; }
	return $rc
}

case "${1:-local}" in
	local)  run_local ;;
	docker) run_docker "${2:?image required}" ;;
	matrix)
		failed=""
		for img in $IMAGES; do
			run_docker "$img" || failed="$failed $img"
			echo
		done
		if [ -n "$failed" ]; then echo "FAILED:$failed"; exit 1; fi
		echo "all images passed"
		;;
	*) echo "usage: $0 [local|docker IMAGE|matrix]" >&2; exit 1 ;;
esac
