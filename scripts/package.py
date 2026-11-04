#!/usr/bin/env python3
"""Package the two built VST3 bundles without platform-specific shell commands."""
from __future__ import annotations

import argparse
import os
from pathlib import Path
import stat
import zipfile


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--build-dir", type=Path, required=True)
    parser.add_argument("--configuration", default="Release")
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    bundles: list[Path] = []
    for target, product in (("NOPEAKS_Master", "NOPEAKS Master"), ("NOPEAKS_Track", "NOPEAKS Track")):
        root = args.build_dir / f"{target}_artefacts"
        choices = [root / args.configuration / "VST3" / f"{product}.vst3", root / "VST3" / f"{product}.vst3"]
        bundle = next((path for path in choices if path.exists()), None)
        if bundle is None:
            raise SystemExit(f"Missing built VST3 bundle: {choices[0]}")
        bundles.append(bundle)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(args.output, "w", compression=zipfile.ZIP_DEFLATED) as archive:
        for bundle in bundles:
            files = sorted(bundle.rglob("*")) if bundle.is_dir() else [bundle]
            if not files:
                raise SystemExit(f"Empty VST3 bundle: {bundle}")
            for path in files:
                if path.is_dir():
                    continue
                name = path.relative_to(bundle.parent).as_posix()
                info = zipfile.ZipInfo.from_file(path, arcname=name)
                info.compress_type = zipfile.ZIP_DEFLATED
                if path.is_symlink():
                    info.create_system = 3
                    info.external_attr = (stat.S_IFLNK | 0o777) << 16
                    archive.writestr(info, os.readlink(path))
                else:
                    archive.writestr(info, path.read_bytes())
        archive.writestr("INSTALL.txt", "NOPEAKS 0.1.0 research preview\n\n"
                         "Install BOTH Master and Track VST3 bundles.\n"
                         "Windows: C:\\Program Files\\Common Files\\VST3\\\n"
                         "macOS: ~/Library/Audio/Plug-Ins/VST3/\n"
                         "Linux: ~/.vst3/\n\n"
                         "These CI binaries are unsigned and not notarized.\n"
                         "See the repository README and docs/BUILD.md for setup and limitations.\n")
    print(args.output)


if __name__ == "__main__":
    main()
