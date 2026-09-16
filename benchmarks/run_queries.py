#!/usr/bin/env python3
"""Five trials; optionally alternate two binaries to reduce ordering bias."""
import csv
from contextlib import ExitStack
import subprocess
import sys

if len(sys.argv) not in (3, 5):
    raise SystemExit("Usage: run_queries.py BINARY OUTPUT.csv [OTHER_BINARY OTHER.csv]")
with ExitStack() as stack:
    targets = []
    for i in range(1, len(sys.argv), 2):
        output = stack.enter_context(open(sys.argv[i + 1], "w", newline=""))
        writer = csv.writer(output, lineterminator="\n")
        writer.writerow(["trial", "workload", "rows", "repeats", "mean_ms", "result_rows_total"])
        targets.append((sys.argv[i], writer, output))
    for trial in range(5):
        for mode in ("scan", "filter", "sort", "top10", "vector_top10", "text_filter", "text_sort", "text_top10"):
            rows = 10000 if mode == "vector_top10" or mode.startswith("text_") else 100000
            for binary, writer, output in (targets if trial % 2 == 0 else targets[::-1]):
                line = subprocess.check_output([binary, mode, str(rows), "20"], text=True).strip()
                writer.writerow([trial + 1, *line.split(",")])
                output.flush()
