#!/usr/bin/env python3
import argparse
import csv
import statistics
from collections import defaultdict


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("input")
    parser.add_argument("output")
    parser.add_argument("--mode", choices=("native", "wasm"), required=True)
    parser.add_argument("--expected-runs", type=int, required=True)
    args = parser.parse_args()

    groups = defaultdict(list)
    aggregate = defaultdict(list)
    with open(args.input, newline="", encoding="utf-8") as handle:
        for row in csv.reader((line for line in handle if not line.startswith("#")), delimiter="\t"):
            if not row:
                continue
            if args.mode == "native":
                key = (row[0], row[7])
                value = float(row[6])
                aggregate[("__all__", row[7])].append(value)
            else:
                key = (row[0], row[3], row[9])
                value = float(row[8])
                aggregate[("__all__", row[3], row[9])].append(value)
            groups[key].append(value)

    if not groups:
        raise SystemExit("no benchmark rows found")
    for key, values in groups.items():
        if len(values) != args.expected_runs:
            raise SystemExit(f"{key}: expected {args.expected_runs} runs, got {len(values)}")
    groups.update(aggregate)

    with open(args.output, "w", newline="", encoding="utf-8") as handle:
        writer = csv.writer(handle, delimiter="\t")
        if args.mode == "native":
            writer.writerow(("file", "impl", "runs", "median_realtime_x", "min", "max"))
        else:
            writer.writerow(("file", "block", "config", "runs", "median_realtime_x", "min", "max"))
        for key in sorted(groups):
            values = groups[key]
            writer.writerow((*key, len(values), f"{statistics.median(values):.3f}",
                             f"{min(values):.3f}", f"{max(values):.3f}"))


if __name__ == "__main__":
    main()
