#!/usr/bin/env python3
"""Minimal reader for the flat export written by bus_export.

Only the Python standard library is used. Field semantics and status rules are
documented in docs/interface-for-analysis.md. This script reads a directory that
contains parameters_flat.jsonl or parameters_flat.csv and prints the field list
and the status counts.

    python examples/read_flat.py runs/example_export
"""
import csv
import json
import os
import sys
from collections import Counter

FIELDS = [
    "run_id", "session_id", "source_id", "channel", "protocol_id", "parameter_id",
    "unit", "value_type", "value_num", "value_str", "validity", "status",
    "observation_time_ns", "available_time_ns", "ingest_time_ns", "clock_group",
    "offset_ns", "uncertainty_ns", "nominal_period_ns", "sequence", "sequence_step",
    "raw_record_index", "origin_source", "origin_generation", "decoder_version",
    "config_version",
]


def read_jsonl(path):
    rows = []
    with open(path, "r", encoding="utf-8") as handle:
        for line in handle:
            line = line.strip()
            if line:
                rows.append(json.loads(line))
    return rows


def read_csv(path):
    with open(path, "r", encoding="utf-8", newline="") as handle:
        return list(csv.DictReader(handle))


def main(argv):
    if len(argv) != 2:
        print(__doc__)
        return 2
    directory = argv[1]
    jsonl_path = os.path.join(directory, "parameters_flat.jsonl")
    csv_path = os.path.join(directory, "parameters_flat.csv")
    if os.path.exists(jsonl_path):
        rows, kind = read_jsonl(jsonl_path), "jsonl"
    elif os.path.exists(csv_path):
        rows, kind = read_csv(csv_path), "csv"
    else:
        print("no parameters_flat.jsonl or parameters_flat.csv in", directory)
        return 1

    print("source :", kind)
    print("rows   :", len(rows))
    print("fields :", ", ".join(FIELDS))
    present = sorted(rows[0].keys()) if rows else []
    missing = [name for name in FIELDS if name not in present]
    if missing:
        print("missing fields:", ", ".join(missing))
    counts = Counter(row.get("status", "?") for row in rows)
    print("status :", ", ".join("%s=%d" % (name, counts[name]) for name in sorted(counts)))
    if rows:
        print("first  :", json.dumps(rows[0], ensure_ascii=False))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
