#!/usr/bin/env bash
set -euo pipefail

BIN="$1"
WORLD="worlds/world_2021.h5"
CONFIG="configs/config_2021/simulation.yaml"
TMP="$(mktemp -d /private/tmp/june2-cli-smoke.XXXXXX)"
trap 'rm -rf "$TMP"' EXIT

printf '%s\n' 'infection_seeds: []' > "$TMP/override.yaml"

# Check the positional world-file form.
"$BIN" "$WORLD" --config "$CONFIG" --days 1 --seed 12345 \
  --runs-dir "$TMP/runs" --run-id positional > "$TMP/positional.log" 2>&1
grep -q 'Running in SERIAL mode' "$TMP/positional.log"

# Check the explicit form.
"$BIN" --config "$CONFIG" --world "$WORLD" --days 1 --seed 12345 \
  --runs-dir "$TMP/runs" --run-id explicit > "$TMP/explicit.log" 2>&1
grep -q 'Running in SERIAL mode' "$TMP/explicit.log"

# Check the legacy alias and that the manifest records the override file.
"$BIN" --sim_config "$CONFIG" --world "$WORLD" \
  --infection_seeds "$TMP/override.yaml" --days 1 --seed 12345 \
  --runs-dir "$TMP/runs" --run-id alias > "$TMP/alias.log" 2>&1
grep -q 'Running in SERIAL mode' "$TMP/alias.log"
grep -q "original: $TMP/override.yaml" "$TMP/runs/alias/manifest.yaml"

if "$BIN" --config "$CONFIG" --world "$WORLD" --world "$WORLD" \
  --days 1 > "$TMP/duplicate.log" 2>&1; then
  echo 'duplicate --world unexpectedly succeeded' >&2
  exit 1
fi
grep -q 'may only be specified once' "$TMP/duplicate.log"

if "$BIN" --config "$CONFIG" --world "$WORLD" --not-a-real-option \
  > "$TMP/unknown.log" 2>&1; then
  echo 'unknown option unexpectedly succeeded' >&2
  exit 1
fi
grep -q "unknown command-line option '--not-a-real-option'" "$TMP/unknown.log"

if "$BIN" --config "$CONFIG" --world > "$TMP/incomplete.log" 2>&1; then
  echo 'incomplete --world unexpectedly succeeded' >&2
  exit 1
fi
grep -q -- '--world requires an argument' "$TMP/incomplete.log"

if "$BIN" --days 1 > "$TMP/missing.log" 2>&1; then
  echo 'missing required paths unexpectedly succeeded' >&2
  exit 1
fi
grep -q -- '--config <path/to/simulation.yaml> is required' "$TMP/missing.log"
grep -q -- '--world <path/to/world.h5> is required' "$TMP/missing.log"

if "$BIN" --config "$CONFIG" --world "$WORLD" --seed 12x \
  > "$TMP/invalid-seed.log" 2>&1; then
  echo 'invalid --seed unexpectedly succeeded' >&2
  exit 1
fi
grep -q -- '--seed expects an integer' "$TMP/invalid-seed.log"

if "$BIN" --config "$CONFIG" --world "$WORLD" --days 3x \
  > "$TMP/invalid-days.log" 2>&1; then
  echo 'invalid --days unexpectedly succeeded' >&2
  exit 1
fi
grep -q -- '--days expects a non-negative integer' "$TMP/invalid-days.log"

echo 'CLI compatibility smoke tests passed'
