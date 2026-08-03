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
  --profile quick \
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
  --profile quick \
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
  --profile quick \
  --seed 1234 \
  --results "$TMPDIR_BENCH/mixed.jsonl" \
  --artifact-dir "$TMPDIR_BENCH/artifacts" \
  --filesystem posix-test

test "$(wc -l <"$TMPDIR_BENCH/mixed.jsonl")" -eq 8

rm -rf "$TMPDIR_BENCH/root"
mkdir -p "$TMPDIR_BENCH/root"
"$REPO/bench/bin/bench-posix" \
  --root "$TMPDIR_BENCH/root" \
  --scenario concurrency-seq \
  --phase all \
  --profile quick \
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
  --profile quick \
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
  --profile quick \
  --seed 1234 \
  --results "$TMPDIR_BENCH/empty-recovery.jsonl" \
  --filesystem posix-test
status=$?
set -e
test "$status" -ne 0
test "$status" -lt 128
grep -q '"validation_status":"failed"' "$TMPDIR_BENCH/empty-recovery.jsonl"

truncate -s 128M "$TMPDIR_BENCH/old-bsfs.img"
for scenario in bulk metadata mixed old-recovery; do
  "$REPO/bench/bin/mkfs-old-bsfs" "$TMPDIR_BENCH/old-bsfs.img"
  "$REPO/bench/bin/bench-old" \
    --device "$TMPDIR_BENCH/old-bsfs.img" \
    --scenario "$scenario" \
    --phase all \
    --profile quick \
    --seed 1234 \
    --results "$TMPDIR_BENCH/old-$scenario.jsonl" \
    --artifact-dir "$TMPDIR_BENCH/artifacts" \
    --filesystem old-bsfs-test
done

test "$(wc -l <"$TMPDIR_BENCH/old-bulk.jsonl")" -eq 6
test "$(wc -l <"$TMPDIR_BENCH/old-metadata.jsonl")" -eq 9
test "$(wc -l <"$TMPDIR_BENCH/old-mixed.jsonl")" -eq 8
test "$(wc -l <"$TMPDIR_BENCH/old-old-recovery.jsonl")" -eq 5

echo "bench C integration tests: PASS"
