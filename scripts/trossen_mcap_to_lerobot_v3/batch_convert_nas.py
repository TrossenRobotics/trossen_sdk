#!/usr/bin/env python3
"""Batch-convert a tree of TrossenMCAP recordings into LeRobot v3.0 datasets.

Each immediate subfolder of the input root that contains ``*.mcap`` files is
treated as one dataset and handed to the ``trossen_mcap_to_lerobot_v3`` binary,
which produces ``<output>/<repository_id>/<dataset_id>/``. The folder name
becomes the (sanitized) ``dataset_id``.

Typical use is a sweep over the NAS share (AFP/SMB mounted via gvfs), e.g.::

    hamza/Datasets/Mcap/
        push cup to the black tape/            episode_000000.mcap ...
        trossen_mobile_ai_pull_black_handle.../ episode_000000.mcap ...

Two parallelism axes multiply, so the script splits them deliberately:
  * ``--concurrency C`` dataset conversions run at once (one binary per dataset),
  * ``--jobs J`` worker threads inside each conversion (decode/extract/encode).
SVT-AV1 is itself multi-threaded, so the defaults keep ``C * J`` well under the
core count to avoid oversubscription. Tune both if you know your workload.

Run it with the converter already built (``cmake --build build --target
trossen_mcap_to_lerobot_v3``); no need to run from the repo root — the script
locates the repo and config itself.
"""

from __future__ import annotations

import argparse
import concurrent.futures
import dataclasses
import os
import re
import subprocess
import sys
import time
from pathlib import Path

# Repo root is three levels up from this file: scripts/trossen_mcap_to_lerobot_v3/.
REPO_ROOT = Path(__file__).resolve().parents[2]
DEFAULT_BINARY = REPO_ROOT / "build" / "scripts" / "trossen_mcap_to_lerobot_v3"
DEFAULT_CONFIG = REPO_ROOT / "scripts" / "trossen_mcap_to_lerobot_v3" / "config.json"

# Where hamza's MCAP datasets live on the Trossen_Cloud AFP share (gvfs mount).
# Override with --input if the mount path differs.
DEFAULT_NAS_INPUT = (
    "/run/user/1000/gvfs/"
    "afp-volume:host=Trossen_Cloud.local,user=anonymous,volume=Data%20Collection/"
    "hamza/Datasets/Mcap"
)


def sanitize_dataset_id(name: str) -> str:
    """Turn a folder name into a filesystem/HuggingFace-friendly dataset id.

    Lowercases nothing (ids are case-sensitive on the Hub) but collapses runs of
    non-alphanumeric characters to single underscores and trims them, so
    ``"push cup to the black tape"`` becomes ``"push_cup_to_the_black_tape"``.
    """
    cleaned = re.sub(r"[^0-9A-Za-z._-]+", "_", name).strip("_")
    return cleaned or "dataset"


def find_dataset_dirs(input_root: Path) -> list[Path]:
    """Return sorted immediate subdirectories of input_root that hold MCAP files."""
    dirs: list[Path] = []
    for child in sorted(input_root.iterdir()):
        if not child.is_dir():
            continue
        # next() avoids materializing all 200 paths just to test for presence.
        if next(child.glob("*.mcap"), None) is not None:
            dirs.append(child)
    return dirs


@dataclasses.dataclass
class DatasetResult:
    """Outcome of converting one dataset folder."""

    name: str
    dataset_id: str
    status: str  # "ok" | "failed" | "skipped"
    episodes: int
    seconds: float
    attempts: int
    log_path: Path | None


def output_dataset_dir(args: argparse.Namespace, dataset_id: str) -> Path:
    """Mirror the binary's <root>/<repository_id>/<dataset_id> layout."""
    return Path(args.output).expanduser() / args.repository_id / dataset_id


def build_command(args: argparse.Namespace, dataset_dir: Path, dataset_id: str) -> list[str]:
    """Assemble the converter invocation for one dataset."""
    cmd = [
        str(args.binary),
        str(dataset_dir),
        "--config",
        str(args.config),
        "--jobs",
        str(args.jobs),
        "--set",
        f"lerobot_v3_backend.root={Path(args.output).expanduser()}",
        "--set",
        f"lerobot_v3_backend.repository_id={args.repository_id}",
        "--set",
        f"lerobot_v3_backend.dataset_id={dataset_id}",
        "--set",
        f"lerobot_v3_backend.overwrite_existing={'true' if args.overwrite else 'false'}",
    ]
    return cmd


def convert_one(args: argparse.Namespace, dataset_dir: Path, log_dir: Path) -> DatasetResult:
    """Convert a single dataset folder, with retries and a per-dataset log file."""
    name = dataset_dir.name
    dataset_id = sanitize_dataset_id(name)
    episodes = sum(1 for _ in dataset_dir.glob("*.mcap"))
    out_dir = output_dataset_dir(args, dataset_id)

    if args.skip_existing and (out_dir / "meta" / "info.json").exists():
        return DatasetResult(name, dataset_id, "skipped", episodes, 0.0, 0, None)

    cmd = build_command(args, dataset_dir, dataset_id)
    log_path = log_dir / f"{dataset_id}.log"

    if args.dry_run:
        print(f"[dry-run] {name}  ({episodes} episodes) -> {out_dir}")
        print("          " + " ".join(cmd))
        return DatasetResult(name, dataset_id, "skipped", episodes, 0.0, 0, None)

    start = time.monotonic()
    attempts = 0
    # attempts = initial try + up to --retries extra tries.
    while attempts <= args.retries:
        attempts += 1
        with open(log_path, "w") as log:
            log.write(f"# {name} -> {dataset_id}  (attempt {attempts})\n")
            log.write("# " + " ".join(cmd) + "\n\n")
            log.flush()
            proc = subprocess.run(
                cmd,
                cwd=str(REPO_ROOT),
                stdout=log,
                stderr=subprocess.STDOUT,
                check=False,
            )
        if proc.returncode == 0:
            return DatasetResult(
                name, dataset_id, "ok", episodes, time.monotonic() - start, attempts, log_path
            )
        if attempts <= args.retries:
            # AFP/SMB reads can flake mid-run; pause briefly and retry the folder.
            time.sleep(args.retry_delay)

    return DatasetResult(
        name, dataset_id, "failed", episodes, time.monotonic() - start, attempts, log_path
    )


def resolve_parallelism(args: argparse.Namespace, num_datasets: int) -> None:
    """Fill in --concurrency / --jobs defaults from the core count, in place."""
    cores = os.cpu_count() or 1
    if args.concurrency is None:
        args.concurrency = max(1, min(2, num_datasets))
    args.concurrency = max(1, min(args.concurrency, max(1, num_datasets)))
    if args.jobs is None:
        # Leave headroom for SVT-AV1's own threads: keep concurrency*jobs ~ cores/2.
        args.jobs = max(1, cores // (args.concurrency * 2))


def parse_args(argv: list[str]) -> argparse.Namespace:
    p = argparse.ArgumentParser(
        description="Batch-convert TrossenMCAP folders to LeRobot v3.0 datasets.",
        formatter_class=argparse.ArgumentDefaultsHelpFormatter,
    )
    p.add_argument(
        "--input", default=DEFAULT_NAS_INPUT, help="Root holding one subfolder per dataset."
    )
    p.add_argument(
        "--output",
        default="~/.cache/huggingface/lerobot",
        help="Dataset root (the binary writes <output>/<repository_id>/<dataset_id>).",
    )
    p.add_argument("--binary", default=str(DEFAULT_BINARY), help="Path to the converter binary.")
    p.add_argument("--config", default=str(DEFAULT_CONFIG), help="Converter config.json.")
    p.add_argument(
        "--repository-id",
        default="TrossenRoboticsCommunity",
        help="repository_id path segment for every dataset.",
    )
    p.add_argument(
        "--concurrency",
        type=int,
        default=None,
        help="Dataset conversions to run at once (default: auto, capped at 2).",
    )
    p.add_argument(
        "--jobs",
        type=int,
        default=None,
        help="Worker threads inside each conversion (default: auto from cores).",
    )
    p.add_argument("--overwrite", action="store_true", help="Overwrite existing output datasets.")
    p.add_argument(
        "--skip-existing",
        action="store_true",
        help="Skip a dataset whose meta/info.json already exists.",
    )
    p.add_argument("--retries", type=int, default=1, help="Extra attempts per dataset on failure.")
    p.add_argument(
        "--retry-delay", type=float, default=5.0, help="Seconds to wait before a retry."
    )
    p.add_argument(
        "--only",
        nargs="*",
        default=None,
        metavar="SUBSTR",
        help="Only convert folders whose name contains one of these substrings.",
    )
    p.add_argument(
        "--dry-run", action="store_true", help="List what would run without converting."
    )
    return p.parse_args(argv)


def main(argv: list[str]) -> int:
    args = parse_args(argv)

    input_root = Path(args.input).expanduser()
    if not input_root.is_dir():
        print(f"Error: input root not found: {input_root}", file=sys.stderr)
        print("Is the NAS share mounted? Pass --input if the path differs.", file=sys.stderr)
        return 2
    if not args.dry_run and not Path(args.binary).is_file():
        print(f"Error: converter binary not found: {args.binary}", file=sys.stderr)
        print(
            "Build it first: cmake --build build --target trossen_mcap_to_lerobot_v3",
            file=sys.stderr,
        )
        return 2

    datasets = find_dataset_dirs(input_root)
    if args.only:
        datasets = [d for d in datasets if any(s in d.name for s in args.only)]
    if not datasets:
        print(f"No dataset folders with *.mcap found under {input_root}", file=sys.stderr)
        return 1

    resolve_parallelism(args, len(datasets))

    log_dir = Path(args.output).expanduser() / "_convert_logs"
    if not args.dry_run:
        log_dir.mkdir(parents=True, exist_ok=True)

    print(f"Found {len(datasets)} dataset(s) under {input_root}")
    print(
        f"Concurrency: {args.concurrency} dataset(s) at once x {args.jobs} worker(s) each "
        f"(cores={os.cpu_count()})"
    )
    print(f"Output root: {Path(args.output).expanduser()}  (logs in {log_dir})\n")

    results: list[DatasetResult] = []
    with concurrent.futures.ThreadPoolExecutor(max_workers=args.concurrency) as pool:
        futures = {
            pool.submit(convert_one, args, d, log_dir): d for d in datasets
        }
        for fut in concurrent.futures.as_completed(futures):
            res = fut.result()
            results.append(res)
            tag = {"ok": "[ok]", "failed": "[FAILED]", "skipped": "[skip]"}[res.status]
            detail = f"{res.episodes} ep"
            if res.status == "ok":
                detail += f", {res.seconds:.0f}s"
                if res.attempts > 1:
                    detail += f", {res.attempts} attempts"
            print(f"{tag:>9} {res.name}  ({detail})")

    # Summary, deterministic order by folder name.
    results.sort(key=lambda r: r.name)
    ok = [r for r in results if r.status == "ok"]
    failed = [r for r in results if r.status == "failed"]
    skipped = [r for r in results if r.status == "skipped"]
    print("\n" + "=" * 70)
    print(f"Converted {len(ok)}  |  failed {len(failed)}  |  skipped {len(skipped)}")
    if failed:
        print("Failed datasets (see logs):")
        for r in failed:
            print(f"  - {r.name}  ->  {r.log_path}")
    print("=" * 70)

    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
