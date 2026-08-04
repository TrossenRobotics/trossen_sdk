#!/usr/bin/env python3
"""Report the on-disk compression and message counts of TrossenMCAP episodes.

Used to verify that a recording session actually applied the compression the
webapp asked for, and that switching compression did not change what was
recorded (message counts must match across runs -- MCAP compression is
lossless and per-chunk).

Usage:
    python3 scripts/tools/mcap_compression_report.py <dataset_dir_or_file> [...]

Example:
    python3 scripts/tools/mcap_compression_report.py \
        ~/.trossen_sdk/cmp_none ~/.trossen_sdk/cmp_lz4 ~/.trossen_sdk/cmp_zstd

Requires the `mcap` Python package (pip install mcap).
"""

from __future__ import annotations

import sys
from collections import Counter
from pathlib import Path

from mcap.reader import make_reader


def report_file(path: Path) -> dict:
    """Return per-file compression / size / message-count facts."""
    with path.open("rb") as f:
        reader = make_reader(f)
        summary = reader.get_summary()

        # Every chunk records the codec used for its payload. An empty string
        # means the chunk was stored uncompressed.
        codecs = Counter(ci.compression or "none" for ci in summary.chunk_indexes)

        # Sum of pre-compression chunk payload sizes vs post-compression, which
        # gives the true codec ratio independent of the file's index/metadata
        # overhead.
        uncompressed = sum(ci.uncompressed_size for ci in summary.chunk_indexes)
        compressed = sum(ci.compressed_size for ci in summary.chunk_indexes)

        stats = summary.statistics
        per_topic = {}
        if stats is not None:
            for chan_id, count in stats.channel_message_counts.items():
                chan = summary.channels.get(chan_id)
                per_topic[chan.topic if chan else f"<id {chan_id}>"] = count

    return {
        "file": path.name,
        "file_size": path.stat().st_size,
        "codecs": dict(codecs),
        "chunks": len(summary.chunk_indexes),
        "chunk_uncompressed": uncompressed,
        "chunk_compressed": compressed,
        "messages": stats.message_count if stats else None,
        "per_topic": per_topic,
    }


def mb(n: int) -> str:
    return f"{n / 1024 / 1024:8.2f} MB"


def main(argv: list[str]) -> int:
    if len(argv) < 2:
        print(__doc__)
        return 2

    targets: list[Path] = []
    for arg in argv[1:]:
        p = Path(arg).expanduser()
        if p.is_dir():
            targets.extend(sorted(p.glob("*.mcap")))
        elif p.is_file():
            targets.append(p)
        else:
            print(f"skip (not found): {p}", file=sys.stderr)

    if not targets:
        print("No .mcap files found.", file=sys.stderr)
        return 1

    grand_size = 0
    grand_msgs = 0
    for path in targets:
        try:
            r = report_file(path)
        except Exception as exc:  # noqa: BLE001 - report and keep going
            print(f"{path}: FAILED to read: {exc}", file=sys.stderr)
            continue

        ratio = (
            r["chunk_uncompressed"] / r["chunk_compressed"]
            if r["chunk_compressed"]
            else 0.0
        )
        codec = "+".join(sorted(r["codecs"])) or "none"
        grand_size += r["file_size"]
        grand_msgs += r["messages"] or 0

        print(f"\n{path}")
        print(f"  codec            : {codec}  ({r['chunks']} chunks)")
        print(f"  file size        : {mb(r['file_size'])}")
        print(f"  chunk payload    : {mb(r['chunk_uncompressed'])} raw"
              f" -> {mb(r['chunk_compressed'])} stored   (ratio {ratio:.2f}x)")
        print(f"  messages         : {r['messages']}")
        for topic, count in sorted(r["per_topic"].items()):
            print(f"      {topic:<48} {count}")

    print(f"\nTOTAL: {len(targets)} file(s), {mb(grand_size)}, {grand_msgs} messages")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
