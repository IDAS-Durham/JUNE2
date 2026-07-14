#!/usr/bin/env python3
"""Checkpoint/restart determinism gate.

Proves that a run split into a checkpoint leg + a resume leg is
set-identical to an uninterrupted baseline, for every event dataset, and
that this holds across an arbitrary (write_ranks -> resume_ranks) matrix
(rank-count independence — the project's hard invariant).

For each (wN, rN) pair:
  B = run days [0, ckpt_day]   at wN ranks, checkpoint at ckpt_day
  C = resume from B's checkpoint, run to `days`, at rN ranks
  assert  set(B.events) + set(C.events) == set(baseline.events)
          per dataset, modulo coordinated_encounters.group_id
          (documented rank-stamped field).

A dataset that carries no rows in any of the three runs cannot be compared, so
it is reported rather than passed over. If the config says the dataset is being
written, that silence is a failure: the gate would otherwise report PASS while
quietly checking one dataset fewer than you think.

Usage:
  checkpoint_determinism_check.py --bin BUILD/disease_sim \
      --config configs/config_2021/simulation.yaml \
      --world worlds/world_2021.h5 --days 5 --ckpt-day 2 \
      --matrix 1:1,2:2,1:2,2:1

Exit status: 0 = PASS, 1 = FAIL, 77 = SKIP (world/bin missing).
"""
import argparse
import os
import re
import shutil
import subprocess
import sys
import tempfile

import h5py
import numpy as np


def write_checkpoint_config(src, dst, ckpt_day):
    """Copy src -> dst, replacing any existing top-level `checkpoint:` block
    with a fresh one that fires once at `ckpt_day`. The simulator's yaml-cpp
    keeps the FIRST occurrence on duplicate keys (unlike PyYAML), so we have
    to splice in place rather than append.
    """
    block = (
        "checkpoint:\n"
        "  enabled: true\n"
        "  output_dir: checkpoints/\n"
        f"  every_n_days: {ckpt_day + 1}\n"
        "  on_dates: null\n"
        "  keep_last: 3\n"
    )
    with open(src) as fh:
        text = fh.read()
    # Match the existing block: `checkpoint:` at column 0 + its indented body.
    pat = re.compile(r"(?m)^checkpoint:\n(?:[ \t]+.*\n?)*")
    if pat.search(text):
        text = pat.sub(block, text, count=1)
    else:
        if not text.endswith("\n"):
            text += "\n"
        text += "\n" + block
    with open(dst, "w") as fh:
        fh.write(text)


def run(cmd):
    p = subprocess.run(cmd, capture_output=True, text=True)
    if p.returncode != 0:
        sys.stderr.write(f"\n[cmd failed] {' '.join(cmd)}\n{p.stdout[-2000:]}\n"
                         f"{p.stderr[-2000:]}\n")
    return p.returncode == 0


def events(fn):
    out = {}
    with h5py.File(fn, "r") as f:
        if "events" not in f:
            return out
        for k in f["events"]:
            out[k] = np.asarray(f["events"][k][()])
    return out


def keyset(arr, drop=()):
    if arr.size == 0:
        return []
    flds = [n for n in arr.dtype.names if n not in drop]
    return sorted(
        tuple(round(float(x[f]), 9) if arr.dtype[f].kind == "f"
              else x[f].item() for f in flds)
        for x in arr
    )


def required_datasets(config_path):
    """Datasets this config promises to write, so their absence means the gate
    lost coverage rather than the run being legitimately quiet.

    A run always infects someone, so `infections` is always required: with no
    infection rows the whole comparison is vacuous. `coordinated_encounters` is
    only written when output.save_coordinated_encounters is on, which is off by
    default, so we ask the config rather than assume.
    """
    req = {"infections"}
    with open(config_path) as fh:
        text = fh.read()
    m = re.search(r"(?m)^\s*save_coordinated_encounters:\s*(true|false)\b", text)
    if m and m.group(1) == "true":
        req.add("coordinated_encounters")
    return req


def equal(baseline, b, c, required=()):
    # Union in `required` too: a dataset missing from every file is absent from
    # all three dicts, so iterating only over what's present is exactly how it
    # would go unnoticed.
    keys = set(baseline) | set(b) | set(c) | set(required)
    ok = True
    for k in sorted(keys):
        drop = ("group_id",) if k == "coordinated_encounters" else ()
        A = baseline.get(k, np.empty(0))
        B = b.get(k, np.empty(0))
        C = c.get(k, np.empty(0))
        if A.size == 0 and B.size == 0 and C.size == 0:
            if k in required:
                print(f"    {k:24s} ABSENT from every run, but the config says "
                      f"it is written: the gate is not checking it")
                ok = False
            else:
                print(f"    {k:24s} no rows in any run, nothing to compare")
            continue
        good = sorted(keyset(B, drop) + keyset(C, drop)) == keyset(A, drop)
        ok &= good
        print(f"    {k:24s} base={A.shape[0]:7d} "
              f"B={B.shape[0]:7d} C={C.shape[0]:7d}  "
              f"{'OK' if good else 'MISMATCH'}")
    return ok


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--bin", required=True)
    ap.add_argument("--config", required=True)
    ap.add_argument("--world", required=True)
    ap.add_argument("--days", type=int, default=5)
    ap.add_argument("--ckpt-day", type=int, default=2)
    ap.add_argument("--seed", type=int, default=42)
    ap.add_argument("--matrix", default="1:1,2:2,1:2,2:1")
    ap.add_argument("--mpirun", default="mpirun")
    ap.add_argument("--require", default="",
                    help="comma-separated datasets that must carry rows, on "
                         "top of the ones the config already implies. Use it "
                         "for datasets whose switch lives in a sub-config the "
                         "gate cannot see, e.g. follows (per-rule log:).")
    a = ap.parse_args()

    if not (os.path.exists(a.bin) and os.path.exists(a.world)
            and os.path.exists(a.config)):
        print(f"SKIP: missing bin/world/config "
              f"({a.bin}, {a.world}, {a.config})")
        return 77

    work = tempfile.mkdtemp(prefix="ckpt_det_")
    runs = os.path.join(work, "runs")
    # checkpoint-enabled config: original + every_n_days so it fires exactly
    # once, at the end of ckpt_day (i.e. (day+1) % (ckpt_day+1) == 0).
    cfg = os.path.join(work, "sim_ckpt.yaml")
    write_checkpoint_config(a.config, cfg, a.ckpt_day)

    def launch(np_, run_id, extra):
        cmd = []
        if np_ > 1:
            cmd = [a.mpirun, "-np", str(np_), "--oversubscribe"]
        cmd += [a.bin, "--config", cfg, "--world", a.world,
                "--seed", str(a.seed), "--runs-dir", runs,
                "--run-id", run_id] + extra
        return run(cmd)

    print(f"[baseline] np=1, uninterrupted, {a.days} days")
    if not launch(1, "base", ["--days", str(a.days)]):
        print("FAIL: baseline run failed")
        return 1
    base = events(os.path.join(runs, "base", "simulation_events.h5"))

    required = required_datasets(a.config)
    required |= {d for d in a.require.split(",") if d}
    print(f"[required] datasets that must carry rows: "
          f"{', '.join(sorted(required))}")

    overall = True
    for pair in a.matrix.split(","):
        wN, rN = (int(x) for x in pair.split(":"))
        bid, cid = f"B_{wN}", f"C_{rN}_{wN}"
        print(f"\n[matrix] write@np{wN} -> resume@np{rN}")
        ok = launch(wN, bid, ["--days", str(a.ckpt_day + 1)])
        latest = os.path.join(runs, bid, "checkpoints", "latest")
        ok = ok and launch(rN, cid, ["--days", str(a.days),
                                     "--restart-from", latest])
        if not ok:
            print(f"  FAIL: run error for {pair}")
            overall = False
            continue
        b = events(os.path.join(runs, bid, "simulation_events.h5"))
        c = events(os.path.join(runs, cid, "simulation_events.h5"))
        good = equal(base, b, c, required)
        print(f"  => {'PASS' if good else 'FAIL'}  (write@np{wN} "
              f"resume@np{rN})")
        overall &= good

    shutil.rmtree(work, ignore_errors=True)
    print("\n==== DETERMINISM GATE:",
          "PASS ====" if overall else "FAIL ====")
    return 0 if overall else 1


if __name__ == "__main__":
    sys.exit(main())
