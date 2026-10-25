#!/usr/bin/env python3
"""Convert a stopped debugger's cc_trace ring dump to chronological CSV."""
import argparse
import csv
import pathlib
import struct


def decode(data, count):
    record = struct.Struct("<IfffI")
    if len(data) == 0 or len(data) % record.size:
        raise ValueError("文件长度必须是 CCTrace 的 20 字节记录的整数倍")
    size = len(data) // record.size
    if count < 0:
        raise ValueError("count 不能为负数")
    rows = list(record.iter_unpack(data))
    used = min(count, size)
    start = count % size if count >= size else 0
    ordered = [rows[(start + i) % size] for i in range(used)]
    if not ordered:
        return []
    first = ordered[0][0]
    return [(((ms - first) & 0xffffffff) / 1000.0, iout, target, dac, state)
            for ms, iout, target, dac, state in ordered]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("dump", type=pathlib.Path)
    parser.add_argument("--count", type=int, required=True, help="Watch 中的 cc_trace_count")
    parser.add_argument("--output", type=pathlib.Path, default=pathlib.Path("cc_trace.csv"))
    args = parser.parse_args()
    try:
        rows = decode(args.dump.read_bytes(), args.count)
    except ValueError as error:
        parser.error(str(error))
    with args.output.open("w", newline="") as file:
        writer = csv.writer(file)
        writer.writerow(["time_s", "iout_A", "target_A", "dac_V", "state"])
        writer.writerows(rows)
    print(f"已导出 {len(rows)} 点：{args.output.resolve()}")


if __name__ == "__main__":
    main()
