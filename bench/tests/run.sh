#!/bin/sh
set -eu

REPO=$(CDPATH= cd -- "$(dirname "$0")/../.." && pwd)
TMPDIR_BENCH=$(mktemp -d)
trap 'rm -rf "$TMPDIR_BENCH"' EXIT

mkdir -p "$TMPDIR_BENCH/root" "$TMPDIR_BENCH/artifacts"

"$REPO/bench/bin/bench-posix" \
  --root "$TMPDIR_BENCH/root" \
  --scenario bulk \
  --phase all \
  --seed 1234 \
  --results "$TMPDIR_BENCH/results.jsonl" \
  --artifact-dir "$TMPDIR_BENCH/artifacts" \
  --filesystem posix-test

test "$(wc -l <"$TMPDIR_BENCH/results.jsonl")" -eq 6
grep -q '"validation_status":"passed"' "$TMPDIR_BENCH/results.jsonl"

rm -rf "$TMPDIR_BENCH/root"
mkdir -p "$TMPDIR_BENCH/root"
"$REPO/bench/bin/bench-posix" \
  --root "$TMPDIR_BENCH/root" \
  --scenario metadata \
  --phase all \
  --seed 1234 \
  --results "$TMPDIR_BENCH/metadata.jsonl" \
  --artifact-dir "$TMPDIR_BENCH/artifacts" \
  --filesystem posix-test

test "$(wc -l <"$TMPDIR_BENCH/metadata.jsonl")" -eq 9

rm -rf "$TMPDIR_BENCH/root"
mkdir -p "$TMPDIR_BENCH/root"
"$REPO/bench/bin/bench-posix" \
  --root "$TMPDIR_BENCH/root" \
  --scenario mixed \
  --phase all \
  --seed 1234 \
  --results "$TMPDIR_BENCH/mixed.jsonl" \
  --artifact-dir "$TMPDIR_BENCH/artifacts" \
  --filesystem posix-test

test "$(wc -l <"$TMPDIR_BENCH/mixed.jsonl")" -eq 8
# only phases that flush have a durable rate
test "$(grep -c '"durable_throughput_mib_s":null' "$TMPDIR_BENCH/mixed.jsonl")" -eq 6
grep '"phase":"append-4k"' "$TMPDIR_BENCH/mixed.jsonl" | grep -q '"durable_throughput_mib_s":null'
grep '"phase":"edit-cycle-durable-total"' "$TMPDIR_BENCH/mixed.jsonl" | grep -q '"has_durable_window":true'
grep '"phase":"edit-cycle-durable-total"' "$TMPDIR_BENCH/mixed.jsonl" | grep -vq '"durable_throughput_mib_s":null'

rm -rf "$TMPDIR_BENCH/root"
mkdir -p "$TMPDIR_BENCH/root"
"$REPO/bench/bin/bench-posix" \
  --root "$TMPDIR_BENCH/root" \
  --scenario concurrency-seq \
  --phase all \
  --workers 4 \
  --seed 1234 \
  --results "$TMPDIR_BENCH/concurrency-seq.jsonl" \
  --artifact-dir "$TMPDIR_BENCH/artifacts" \
  --filesystem posix-test

test "$(wc -l <"$TMPDIR_BENCH/concurrency-seq.jsonl")" -eq 10

rm -rf "$TMPDIR_BENCH/root"
mkdir -p "$TMPDIR_BENCH/root"
"$REPO/bench/bin/bench-posix" \
  --root "$TMPDIR_BENCH/root" \
  --scenario concurrency-mixed \
  --phase all \
  --workers 4 \
  --seed 1234 \
  --results "$TMPDIR_BENCH/concurrency-mixed.jsonl" \
  --artifact-dir "$TMPDIR_BENCH/artifacts" \
  --filesystem posix-test

test "$(wc -l <"$TMPDIR_BENCH/concurrency-mixed.jsonl")" -eq 20

# recovery-pressure verify against an empty tree must fail cleanly, not segfault
rm -rf "$TMPDIR_BENCH/root"
mkdir -p "$TMPDIR_BENCH/root/recovery-pressure"
set +e
"$REPO/bench/bin/bench-posix" \
  --root "$TMPDIR_BENCH/root" \
  --scenario recovery-pressure \
  --phase verify \
  --seed 1234 \
  --results "$TMPDIR_BENCH/empty-recovery.jsonl" \
  --filesystem posix-test
status=$?
set -e
test "$status" -ne 0
test "$status" -lt 128
grep -q '"validation_status":"failed"' "$TMPDIR_BENCH/empty-recovery.jsonl"

echo "bench C integration tests: PASS"
