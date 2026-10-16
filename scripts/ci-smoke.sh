#!/bin/sh
# ci-smoke.sh BUILD -- what a build made, run where there is no screen and
# under whatever sanitizers it was built with (.github/workflows/ci.yml):
# the views drawn without tracing the room, in every language; every sound;
# what goes on which shelf; the version; Shell Settings' --check. A report
# from a sanitizer, or a program killed by a signal, fails it; what the
# programs themselves say does not.
set -u
build=${1:?usage: ci-smoke.sh BUILD-DIRECTORY}
build=$(cd "$build" && pwd)
here=$(cd "$(dirname "$0")" && pwd)
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

# a home of its own, and no display to find
export HOME="$tmp/home" XDG_CACHE_HOME="$tmp/cache" XDG_CONFIG_HOME="$tmp/config" \
	XDG_DATA_HOME="$tmp/data" XDG_STATE_HOME="$tmp/state" XDG_RUNTIME_DIR="$tmp/run"
mkdir -p "$HOME" "$XDG_RUNTIME_DIR" "$tmp/sounds"
chmod 700 "$XDG_RUNTIME_DIR"
unset WAYLAND_DISPLAY DISPLAY
# A sanitizer's report ends the program with 99, which no program here
# uses. cairo keeps every face it was asked for by name until it is
# unloaded: that is cairo's cache, not a leak of the desk's (lsan.supp).
export ASAN_OPTIONS="exitcode=99:detect_leaks=1:abort_on_error=0"
export UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1:exitcode=99"
export LSAN_OPTIONS="suppressions=$here/lsan.supp:print_suppressions=0"

failed=0
# run OK-CODES NAME COMMAND... -- OK-CODES as a case pattern, "0" or "0|1"
run() {
	ok=$1 name=$2
	shift 2
	"$@" > "$tmp/out" 2>&1
	rc=$?
	eval "case \$rc in $ok) good=1 ;; *) good=0 ;; esac"
	if [ "$good" -eq 1 ]; then
		echo "ok    $name"
	else
		echo "FAIL  $name (exit $rc)"
		sed 's/^/      /' "$tmp/out" | tail -n 60
		failed=$((failed + 1))
	fi
}

run 0 "desk --version" "$build/desk" --version
for lang in en ja zh ko es fr de; do
	for view in calendar memo agenda; do
		run 0 "desk-preview --$view ($lang)" \
			"$build/desk-preview" --lang "$lang" "--$view" -o "$tmp/$view-$lang.png"
	done
done
run 0 "desk-preview --sounds" "$build/desk-preview" --sounds "$tmp/sounds"
run 0 "desk-preview --apps" "$build/desk-preview" --apps
# 1: a row whose program is not installed, as none are here
if [ -x "$build/desk-settings" ]; then
	run "0|1" "desk-settings --check" "$build/desk-settings" --check
fi

if [ "$failed" -gt 0 ]; then
	echo "$failed failed"
	exit 1
fi
echo "all passed"
