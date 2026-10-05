#!/usr/bin/env python3
"""Canonical HDF5 records for the determinism gates."""
import difflib
import sys

import h5py
import numpy as np


def _value(value):
    if isinstance(value, np.generic):
        value = value.item()
    if isinstance(value, bytes):
        return value.decode("utf-8", errors="replace").rstrip("\x00")
    if isinstance(value, float):
        return round(value, 9)
    return value


def _row_to_text(row, names):
    return " ".join(f"{name}={_value(row[name])!r}" for name in names)


def canonical_rows(h5, path, fields=None):
    """Return sorted canonical records, or None when *path* is missing."""
    if path not in h5:
        return None
    dataset = h5[path]
    if dataset.shape == () or dataset.size == 0:
        return []

    values = dataset[:]
    if values.dtype.names:
        names = list(values.dtype.names)
        if fields is not None:
            missing = [field for field in fields if field not in names]
            if missing:
                raise ValueError(
                    f"fields {missing} not in dataset {path}: {names}"
                )
            names = fields
        rows = [_row_to_text(values[index], names)
                for index in range(values.shape[0])]
    else:
        rows = [repr(_value(value)) for value in values.flatten()]
    return sorted(rows)


def canonical_events(filename):
    """Read every event dataset in *filename* in canonical form."""
    events = {}
    with h5py.File(filename, "r") as h5:
        if "events" not in h5:
            return events
        for name in h5["events"]:
            fields = None
            if name == "coordinated_encounters":
                fields = ["person_a", "person_b", "time", "encounter_type_id",
                          "slot"]
            events[name] = canonical_rows(h5, f"/events/{name}", fields)
    return events


def compare_events(baseline, checkpoint, resumed, required=()):
    """Compare checkpoint + resume records with the uninterrupted baseline."""
    keys = set(baseline) | set(checkpoint) | set(resumed) | set(required)
    ok = True
    for name in sorted(keys):
        base = baseline.get(name) or []
        first = checkpoint.get(name) or []
        second = resumed.get(name) or []
        if not base and not first and not second:
            if name in required:
                print(f"    {name:24s} ABSENT from every run, but the config says "
                      "it is written: the gate is not checking it")
                ok = False
            else:
                print(f"    {name:24s} no rows in any run, nothing to compare")
            continue
        good = sorted(first + second) == base
        ok &= good
        print(f"    {name:24s} base={len(base):7d} "
              f"B={len(first):7d} C={len(second):7d}  "
              f"{'OK' if good else 'MISMATCH'}")
    return ok


def _dump_dataset(h5, path, output, fields=None):
    rows = canonical_rows(h5, path, fields)
    if rows is None:
        output.write(f"# MISSING {path}\n")
    elif not rows:
        output.write(f"# EMPTY {path}\n")
    else:
        for row in rows:
            output.write(row + "\n")


def _dump_group(h5, group_path, output):
    if group_path not in h5:
        output.write(f"# MISSING {group_path}\n")
        return

    def walker(name, obj):
        if isinstance(obj, h5py.Dataset):
            path = f"{group_path}/{name}"
            output.write(f"\n## {path}\n")
            _dump_dataset(h5, path, output)

    h5[group_path].visititems(walker)


def _fields(arguments):
    for argument in arguments:
        if argument.startswith("fields="):
            return argument.split("=", 1)[1].split(",")
    return None


def main():
    command = sys.argv[1]
    if command == "dump":
        filename, kind, path, output_filename = sys.argv[2:6]
        fields = _fields(sys.argv[6:])
        with h5py.File(filename, "r") as h5, open(output_filename, "w") as output:
            if kind == "dataset":
                _dump_dataset(h5, path, output, fields)
            elif kind == "group":
                _dump_group(h5, path, output)
            else:
                raise SystemExit("kind must be dataset|group")
        return 0

    if command == "compare":
        left_filename, right_filename, path = sys.argv[2:5]
        fields = _fields(sys.argv[5:])
        with h5py.File(left_filename, "r") as left, h5py.File(
                right_filename, "r") as right:
            left_rows = canonical_rows(left, path, fields)
            right_rows = canonical_rows(right, path, fields)
        if left_rows == right_rows:
            return 0
        for index, line in enumerate(difflib.unified_diff(
                left_rows or [], right_rows or [],
                fromfile=left_filename, tofile=right_filename,
                lineterm="")):
            if index == 30:
                break
            print(line)
        return 1

    raise SystemExit("command must be dump|compare")


if __name__ == "__main__":
    sys.exit(main())
