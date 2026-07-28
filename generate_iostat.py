#!/usr/bin/env python3

import argparse
import csv
from collections import defaultdict
from pathlib import Path


PICOSECONDS_PER_SECOND = 1_000_000_000_000
BYTES_PER_MIB = 1_048_576


def new_stats():
    return {
        "read_bytes": 0,
        "write_bytes": 0,
        "total_bytes": 0,
        "iops": 0,
        "latency_ticks": 0,
    }


def add_request(stats, operation, length, latency_ticks):
    stats["total_bytes"] += length
    stats["iops"] += 1
    stats["latency_ticks"] += latency_ticks

    if operation == "READ":
        stats["read_bytes"] += length
    elif operation == "WRITE":
        stats["write_bytes"] += length


def average_latency_ms(stats):
    if not stats["iops"]:
        return 0
    return stats["latency_ticks"] / stats["iops"] / 1_000_000_000


def parse_args():
    parser = argparse.ArgumentParser(
        description="Generate per-simulation-second iostat CSV from SimpleSSD."
    )
    parser.add_argument(
        "input",
        nargs="?",
        type=Path,
        default=Path("output/request_latency.csv"),
        help="SimpleSSD request latency CSV (default: output/request_latency.csv)",
    )
    parser.add_argument(
        "-o",
        "--output",
        type=Path,
        default=Path("output/iostat.csv"),
        help="Output iostat CSV (default: output/iostat.csv)",
    )
    return parser.parse_args()


def generate_iostat(input_path, output_path):
    buckets = defaultdict(
        lambda: {
            "all": new_stats(),
            "slc": new_stats(),
            "tlc": new_stats(),
        }
    )
    max_second = -1

    with input_path.open(newline="") as input_file:
        reader = csv.DictReader(input_file)
        required = {"op", "tier", "length", "complete_tick", "latency_tick"}
        missing = required.difference(reader.fieldnames or [])
        if missing:
            raise ValueError(f"Missing required columns: {', '.join(sorted(missing))}")

        for row in reader:
            second = int(row["complete_tick"]) // PICOSECONDS_PER_SECOND
            length = int(row["length"])
            operation = row["op"].upper()
            latency_ticks = int(row["latency_tick"])

            add_request(buckets[second]["all"], operation, length, latency_ticks)

            tier = {"0": "slc", "1": "tlc"}.get(row["tier"])
            if tier:
                add_request(buckets[second][tier], operation, length, latency_ticks)

            max_second = max(max_second, second)

    output_path.parent.mkdir(parents=True, exist_ok=True)
    with output_path.open("w", newline="") as output_file:
        fieldnames = [
            "second",
            "read_MiB_s",
            "write_MiB_s",
            "total_MiB_s",
            "IOPS",
            "avg_latency_ms",
            "slc_read_MiB_s",
            "slc_write_MiB_s",
            "slc_total_MiB_s",
            "slc_IOPS",
            "slc_avg_latency_ms",
            "tlc_read_MiB_s",
            "tlc_write_MiB_s",
            "tlc_total_MiB_s",
            "tlc_IOPS",
            "tlc_avg_latency_ms",
        ]
        writer = csv.DictWriter(output_file, fieldnames=fieldnames)
        writer.writeheader()

        for second in range(max_second + 1):
            total = buckets[second]["all"]
            slc = buckets[second]["slc"]
            tlc = buckets[second]["tlc"]
            writer.writerow(
                {
                    "second": second,
                    "read_MiB_s": f"{total['read_bytes'] / BYTES_PER_MIB:.2f}",
                    "write_MiB_s": f"{total['write_bytes'] / BYTES_PER_MIB:.2f}",
                    "total_MiB_s": f"{total['total_bytes'] / BYTES_PER_MIB:.2f}",
                    "IOPS": total["iops"],
                    "avg_latency_ms": f"{average_latency_ms(total):.3f}",
                    "slc_read_MiB_s": f"{slc['read_bytes'] / BYTES_PER_MIB:.2f}",
                    "slc_write_MiB_s": f"{slc['write_bytes'] / BYTES_PER_MIB:.2f}",
                    "slc_total_MiB_s": f"{slc['total_bytes'] / BYTES_PER_MIB:.2f}",
                    "slc_IOPS": slc["iops"],
                    "slc_avg_latency_ms": f"{average_latency_ms(slc):.3f}",
                    "tlc_read_MiB_s": f"{tlc['read_bytes'] / BYTES_PER_MIB:.2f}",
                    "tlc_write_MiB_s": f"{tlc['write_bytes'] / BYTES_PER_MIB:.2f}",
                    "tlc_total_MiB_s": f"{tlc['total_bytes'] / BYTES_PER_MIB:.2f}",
                    "tlc_IOPS": tlc["iops"],
                    "tlc_avg_latency_ms": f"{average_latency_ms(tlc):.3f}",
                }
            )

    totals = {
        tier: {
            "bytes": sum(bucket[tier]["total_bytes"] for bucket in buckets.values()),
            "iops": sum(bucket[tier]["iops"] for bucket in buckets.values()),
        }
        for tier in ("all", "slc", "tlc")
    }
    seconds = max_second + 1
    peak_second, peak = max(
        buckets.items(),
        key=lambda item: item[1]["all"]["total_bytes"],
        default=(0, {"all": {"total_bytes": 0}}),
    )

    print(f"Generated {output_path}")
    print(f"Simulation seconds: {seconds}")
    for tier in ("all", "slc", "tlc"):
        label = tier.upper()
        bandwidth = totals[tier]["bytes"] / BYTES_PER_MIB / seconds if seconds else 0
        print(
            f"{label}: {totals[tier]['bytes'] / BYTES_PER_MIB:.2f} MiB, "
            f"{totals[tier]['iops']} I/O, {bandwidth:.2f} MiB/s average"
        )
    print(
        f"Peak bandwidth: {peak['all']['total_bytes'] / BYTES_PER_MIB:.2f} "
        f"MiB/s at second {peak_second}"
    )


def main():
    args = parse_args()
    try:
        generate_iostat(args.input, args.output)
    except (OSError, ValueError) as error:
        raise SystemExit(f"Error: {error}")


if __name__ == "__main__":
    main()
