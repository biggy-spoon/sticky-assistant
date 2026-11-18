#!/usr/bin/env python3
"""Package a firmware build for the Seeed Sticky Playground and GitHub releases.

    python3 scripts/package_release.py 1.0.0 [--build firmware/build] [--out dist]

Reads the build's flasher_args.json and writes, into <out>/<version>/:

- bootloader.bin, partition-table.bin and sticky-assistant.bin, the three parts
  at the offsets the build flashes them to;
- manifest.json, the Playground's flash manifest: each part's offset, size and
  SHA-256 (the browser flasher checks them before writing);
- sticky-assistant-full.bin, if the build has merged.bin: one image written at
  0x0, which is what the Playground's website form takes.

Uses only the standard library, so it runs anywhere Python 3.9+ does.
"""
import argparse
import hashlib
import json
import re
import shutil
import sys
from pathlib import Path

NAME = "Sticky Assistant"
# The names the parts are published under, by the build's file name.
PART_NAMES = {
    "bootloader.bin": "bootloader.bin",
    "partition-table.bin": "partition-table.bin",
    "sticky_assistant.bin": "sticky-assistant.bin",
}
MAX_PART_BYTES = 32 * 1024 * 1024  # the Playground's limit per file


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for block in iter(lambda: handle.read(1 << 16), b""):
            digest.update(block)
    return digest.hexdigest()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("version", help="release version, such as 1.0.0")
    parser.add_argument("--build", default="firmware/build", type=Path)
    parser.add_argument("--out", default="dist", type=Path)
    args = parser.parse_args()

    version = args.version.removeprefix("v")
    if not re.fullmatch(r"\d+\.\d+\.\d+(-[0-9A-Za-z.]+)?", version):
        sys.exit(f"version {args.version!r} isn't like 1.2.3")
    flasher = json.loads((args.build / "flasher_args.json").read_text())
    settings = flasher["flash_settings"]
    out = args.out / version
    out.mkdir(parents=True, exist_ok=True)

    parts = []
    for offset, relative in sorted(flasher["flash_files"].items(), key=lambda kv: int(kv[0], 16)):
        source = args.build / relative
        name = PART_NAMES.get(source.name)
        if name is None:
            sys.exit(f"unexpected flash file {relative}; add it to PART_NAMES")
        size = source.stat().st_size
        if size > MAX_PART_BYTES:
            sys.exit(f"{relative} is {size} bytes, over the 32MB limit")
        shutil.copyfile(source, out / name)
        parts.append({"path": name, "offset": int(offset, 16), "size": size,
                      "sha256": sha256(source)})
    for earlier, later in zip(parts, parts[1:]):
        if earlier["offset"] + earlier["size"] > later["offset"]:
            sys.exit(f"{earlier['path']} overlaps {later['path']}")

    manifest = {
        "name": NAME,
        "version": version,
        "flashSize": settings["flash_size"],
        "flashMode": settings["flash_mode"],
        "flashFreq": settings["flash_freq"],
        "baudRate": 460800,
        # A first install should erase: it clears settings left by other firmware.
        "new_install_prompt_erase": True,
        "builds": [{"chipFamily": "ESP32-S3", "parts": parts}],
    }
    (out / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")

    merged = args.build / "merged.bin"
    if merged.exists():
        shutil.copyfile(merged, out / "sticky-assistant-full.bin")
    for path in sorted(out.iterdir()):
        print(f"{path}  {path.stat().st_size} bytes")
    return 0


if __name__ == "__main__":
    sys.exit(main())
