#!/bin/bash
# Runs the whole QEMU suite against every block-allocation strategy.
#
#   ./tests/alloc_matrix.sh                 # linear heap
#   ./tests/alloc_matrix.sh heap            # just one
#   MATRIX_TESTS="smoke alloc" ./tests/alloc_matrix.sh
#
# The strategy is a compile-time choice (see BSFS_ALLOC in the Makefile), so
# "does the new allocator break anything" cannot be answered by one boot: the
# module has to be rebuilt per strategy and the suite replayed. That is all this
# does - rebuild, run every guest script, and print the grid.
#
# Counters are compiled in (BSFS_ALLOC_STATS=y) so tests/alloc_guest.sh can
# check that the strategy is actually engaged. Pass BSFS_ALLOC_STATS= to
# measure the build the benchmarks should use instead.
#
# Every cell keeps its own console log under $MATRIX_OUT, because
# tests/smoke.sh and tests/crash.sh each overwrite a single fixed path.
#
# Exits 0 only if every cell passed.

set -u
REPO="$(cd "$(dirname "$0")/.." && pwd)"
cd "$REPO" || exit 2

STRATEGIES=("$@")
[ ${#STRATEGIES[@]} -eq 0 ] && STRATEGIES=(linear heap)

# name:timeout. The timeouts are wall-clock for a KASAN kernel in QEMU, and the
# slow ones are slow for a reason: reclaim and alloc each fill a 32 MB image
# block by block, several times over, which is exactly the workload the
# allocator is being judged on.
DEFAULT_TESTS="smoke:120 stress:300 persist:300 cow:600 alias:300 fuzz:300 alloc:1200 reclaim:1200 crash:900 enospc_crash:900"
TESTS="${MATRIX_TESTS_RAW:-$DEFAULT_TESTS}"

# MATRIX_TESTS is the friendly form: a plain list of names, timeouts looked up
# from the defaults above.
if [ -n "${MATRIX_TESTS:-}" ]; then
  SELECTED=""
  for want in $MATRIX_TESTS; do
    found=""
    for entry in $DEFAULT_TESTS; do
      [ "${entry%%:*}" = "$want" ] && found="$entry"
    done
    [ -n "$found" ] || {
      echo "unknown test '$want'. Known: $(echo "$DEFAULT_TESTS" | tr ' ' '\n' | cut -d: -f1 | tr '\n' ' ')"
      exit 2
    }
    SELECTED="$SELECTED $found"
  done
  TESTS="$SELECTED"
fi

OUT="${MATRIX_OUT:-/tmp/bsfs_matrix}"
STATS="${BSFS_ALLOC_STATS-y}"

rm -rf "$OUT"
mkdir -p "$OUT"

# results[strategy/test] = PASS | FAIL | INFRA | BUILD
declare -A results
declare -A notes
overall=0

run_cell() {
  local strategy="$1" name="$2" timeout="$3"
  local log="$OUT/$strategy/$name.log"
  local rc out

  printf '  %-14s ' "$name"

  case "$name" in
  crash | enospc_crash)
    # two boots over one virtio disk; its own runner, its own log path
    out=$(CRASH_TIMEOUT="$timeout" ./tests/crash.sh "$name" 2>&1)
    rc=$?
    cp /tmp/bsfs_crash.log "$log" 2>/dev/null
    ;;
  *)
    out=$(SMOKE_TIMEOUT="$timeout" ./tests/smoke.sh "tests/${name}_guest.sh" 2>&1)
    rc=$?
    cp /tmp/bsfs_smoke.log "$log" 2>/dev/null
    ;;
  esac

  case $rc in
  0) results["$strategy/$name"]=PASS ;;
  1)
    results["$strategy/$name"]=FAIL
    notes["$strategy/$name"]=$(echo "$out" | grep '^SMOKE_FAIL:' | head -1)
    overall=1
    ;;
  *)
    results["$strategy/$name"]=INFRA
    notes["$strategy/$name"]="runner exit $rc (timeout ${timeout}s?)"
    overall=1
    ;;
  esac

  # A memory error can leave every assertion passing, so the log is checked
  # separately - a KASAN report is a failure whatever the test concluded.
  if [ -f "$log" ] && grep -qE 'BUG: KASAN|general protection fault|kernel BUG at|Unable to handle kernel' "$log"; then
    results["$strategy/$name"]=KASAN
    notes["$strategy/$name"]=$(grep -m1 -E 'BUG: KASAN|general protection fault|kernel BUG at|Unable to handle kernel' "$log" | tr -d '\r')
    overall=1
  fi

  echo "${results["$strategy/$name"]}"
}

for strategy in "${STRATEGIES[@]}"; do
  echo
  echo "=== $strategy (BSFS_ALLOC_STATS=${STATS:-off}) ==="
  mkdir -p "$OUT/$strategy"

  if ! make BSFS_ALLOC="$strategy" BSFS_ALLOC_STATS="$STATS" >"$OUT/$strategy/build.log" 2>&1; then
    echo "  BUILD FAILED (see $OUT/$strategy/build.log)"
    tail -15 "$OUT/$strategy/build.log" | sed 's/^/    /'
    for entry in $TESTS; do results["$strategy/${entry%%:*}"]=BUILD; done
    overall=1
    continue
  fi

  for entry in $TESTS; do
    run_cell "$strategy" "${entry%%:*}" "${entry##*:}"
  done
done

echo
echo "================= matrix ================="
printf '%-16s' "test"
for strategy in "${STRATEGIES[@]}"; do printf '%-12s' "$strategy"; done
echo
for entry in $TESTS; do
  name="${entry%%:*}"
  printf '%-16s' "$name"
  for strategy in "${STRATEGIES[@]}"; do
    printf '%-12s' "${results["$strategy/$name"]:-?}"
  done
  echo
done

echo
for key in "${!notes[@]}"; do
  echo "  $key: ${notes[$key]}"
done | sort

echo
echo "logs: $OUT"
[ $overall -eq 0 ] && echo "ALL PASS" || echo "FAILURES ABOVE"
exit $overall
