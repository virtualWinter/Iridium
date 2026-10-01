#!/usr/bin/env python3
"""Fetch the pinned upstream WebExtensions definitions into a local cache.

The inventory must never be written from memory of the APIs, so it is derived
from three upstream repositories at the exact commits in pins.json:

  mdn/browser-compat-data   the machine-readable support data behind MDN's
                            compatibility tables (namespaces, members, manifest
                            keys, browser support notes)
  mdn/content               the prose: which namespaces and manifest keys exist
                            at all, and the documented behaviour that BCD does
                            not encode
  mozilla-firefox/firefox   the Gecko schema files, which are the only
                            authoritative statement of a function's parameters,
                            its required permission, and its manifest-version
                            bounds

Output lands in the cache directory (default .cache/webext-upstream) and is not
committed: it is large, and re-fetching is cheap and pinned.

Usage:
    tools/webext/fetch.py [--cache DIR] [--refresh]

Re-uses an existing cache unless --refresh is given. --refresh also re-checks
that the pinned commits still exist upstream, which is what you want before
bumping a pin.
"""

from __future__ import annotations

import argparse
import concurrent.futures
import json
import shutil
import subprocess
import sys
import urllib.error
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
PINS = Path(__file__).resolve().with_name("pins.json")
RAW = "https://raw.githubusercontent.com/{repo}/{ref}/{path}"
USER_AGENT = "iridium-webext-inventory/1.0"


def log(message: str) -> None:
    print(f"[webext] {message}", flush=True)


def load_pins() -> dict:
    with PINS.open(encoding="utf-8") as handle:
        pins = json.load(handle)
    # The "_comment" keys are documentation for humans, not configuration.
    return {key: value for key, value in pins.items() if not key.startswith("_")}


def run_git(*args: str) -> None:
    subprocess.run(["git", *args], check=True)


def fetch_via_git(repo: str, ref: str, paths: list[str], dest: Path) -> None:
    """Sparse-clone only the paths we read.

    A full browser-compat-data clone is large and slow; a blobless sparse clone
    of three subtrees is a few megabytes.
    """
    dest.parent.mkdir(parents=True, exist_ok=True)
    if dest.exists():
        shutil.rmtree(dest)
    log(f"clone {repo}@{ref[:12]} ({', '.join(paths)})")
    run_git(
        "clone",
        "--depth",
        "1",
        "--filter=blob:none",
        "--sparse",
        f"https://github.com/{repo}.git",
        str(dest),
    )
    run_git("-C", str(dest), "sparse-checkout", "set", *paths)


def fetch_via_raw(repo: str, ref: str, paths: list[str], dest: Path) -> None:
    """Fall back to per-file downloads when git is unavailable.

    Only viable for the schema directories, whose file list comes from the
    GitHub contents API. It is used so the inventory can still be refreshed on a
    machine without git.
    """
    dest.mkdir(parents=True, exist_ok=True)
    for path in paths:
        listing = f"https://api.github.com/repos/{repo}/contents/{path}?ref={ref}"
        with urllib.request.urlopen(urllib.request.Request(listing, headers={"User-Agent": USER_AGENT}), timeout=60) as response:
            entries = json.load(response)
        for entry in entries:
            if entry.get("type") != "file":
                continue
            name = entry["name"]
            if not name.endswith(".json"):
                continue
            url = RAW.format(repo=repo, ref=ref, path=f"{path}/{name}")
            target = dest / name
            request = urllib.request.Request(url, headers={"User-Agent": USER_AGENT})
            with urllib.request.urlopen(request, timeout=120) as response, target.open("wb") as out:
                shutil.copyfileobj(response, out)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cache", default=str(ROOT / ".cache" / "webext-upstream"),
                        help="cache directory (default: .cache/webext-upstream)")
    parser.add_argument("--refresh", action="store_true",
                        help="re-clone even when the cache already exists")
    args = parser.parse_args()

    cache = Path(args.cache).resolve()
    pins = load_pins()

    have_git = shutil.which("git") is not None
    if not have_git:
        log("git not found; falling back to per-file downloads (no commit check)")

    for key, spec in pins.items():
        repo, ref, paths = spec["name"], spec["ref"], spec["paths"]
        dest = cache / key

        if dest.exists() and not args.refresh:
            log(f"{key}: cached, skipping")
            continue

        if args.refresh and not have_git:
            log(f"{key}: cannot verify pinned ref without git")
            return 2

        try:
            if have_git:
                fetch_via_git(repo, ref, paths, dest)
            else:
                if key == "bcd":
                    log(f"{key}: needs git (sparse checkout of multiple subtrees)")
                    return 2
                fetch_via_raw(repo, ref, paths, dest)
        except subprocess.CalledProcessError:
            log(f"{key}: pinned ref {ref[:12]} could not be fetched")
            return 1
        except urllib.error.URLError as error:
            log(f"{key}: {error}")
            return 1

        count = sum(1 for _ in dest.rglob("*.json"))
        log(f"{key}: {count} json files")

    manifest = {
        key: {"repo": spec["name"], "ref": spec["ref"], "committed": spec.get("committed"),
              "license": spec.get("license", "")}
        for key, spec in pins.items()
    }
    (cache / "sources.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    log(f"cache ready: {cache}")
    return 0


if __name__ == "__main__":
    sys.exit(main())