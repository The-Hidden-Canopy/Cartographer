#!/usr/bin/env python3
"""Rebuild the original First Block example with the Cartographer CLI."""

import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import tempfile


VERTICES = (
    (-1.0, 0.0, -0.75),
    (1.0, 0.0, -0.75),
    (1.0, 0.5, -0.75),
    (-1.0, 0.5, -0.75),
    (-1.0, 0.0, 0.75),
    (1.0, 0.0, 0.75),
    (1.0, 0.5, 0.75),
    (-1.0, 0.5, 0.75),
)

FACES = (
    (1, 4, 3, 2),
    (5, 6, 7, 8),
    (1, 2, 6, 5),
    (4, 8, 7, 3),
    (1, 5, 8, 4),
    (2, 3, 7, 6),
)

SOURCE_NAMESPACE = "cartographer.project.examples.first-block"


def number(value):
    return str(int(value)) if value.is_integer() else str(value)


def obj_text():
    lines = [
        "# First Block - original Cartographer first-use example",
        "# Created for this package; no external source assets or textures.",
        "# Units: meters. Coordinate system: right-handed, +Y up.",
        "# CARTOGRAPHER_FACE_MATERIAL_REGIONS 1 condition.material_region",
    ]
    lines.extend(
        f"# CARTOGRAPHER_FACE_MATERIAL {index} 1"
        for index in range(1, len(FACES) + 1)
    )
    lines.extend(
        f"v {number(x)} {number(y)} {number(z)}" for x, y, z in VERTICES
    )
    lines.extend("f " + " ".join(map(str, face)) for face in FACES)
    return "\n".join(lines) + "\n"


def run(command):
    subprocess.run([str(part) for part in command], check=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", type=Path, help="new or empty output directory")
    parser.add_argument(
        "--cli",
        required=True,
        type=Path,
        help="path to cartographer_cli (or cartographer_cli.exe)",
    )
    args = parser.parse_args()
    if not args.cli.is_file():
        parser.error(f"Cartographer CLI does not exist: {args.cli}")
    args.output.mkdir(parents=True, exist_ok=True)

    destinations = {
        name: args.output / name
        for name in ("first-block.carto", "first-block.obj", "manifest.json")
    }
    occupied = [str(path) for path in destinations.values() if path.exists()]
    if occupied:
        parser.error("refusing to overwrite existing files: " + ", ".join(occupied))

    cli_version = subprocess.check_output(
        [str(args.cli), "--version"], text=True, encoding="utf-8"
    ).strip()
    destinations["first-block.obj"].write_text(
        obj_text(), encoding="utf-8", newline="\n"
    )
    with tempfile.TemporaryDirectory(prefix="cartographer-first-block-") as temporary:
        generated = Path(temporary) / "first-block.carto"
        run((args.cli, "import-obj", destinations["first-block.obj"], generated))
        project_text = generated.read_text(encoding="utf-8")
        lines = project_text.splitlines()
        namespace_indices = [
            index for index, line in enumerate(lines)
            if line.startswith("SOURCE_NAMESPACE ")
        ]
        if len(namespace_indices) != 1:
            raise RuntimeError("generated project has no unique SOURCE_NAMESPACE record")
        lines[namespace_indices[0]] = f'SOURCE_NAMESPACE "{SOURCE_NAMESPACE}"'
        generated.write_text(
            "\n".join(lines) + "\n", encoding="utf-8", newline="\n"
        )
        run((args.cli, "validate", generated))
        destinations["first-block.carto"].write_bytes(generated.read_bytes())

    artifacts = {
        name: destinations[name].read_bytes()
        for name in ("first-block.carto", "first-block.obj")
    }
    manifest = {
        "example": "First Block",
        "generator": 2,
        "generator_cli_version": cli_version,
        "project_format": artifacts["first-block.carto"].split(b"\n", 1)[0].decode("ascii"),
        "source_namespace": SOURCE_NAMESPACE,
        "external_asset_dependencies": [],
        "files": {
            name: {"bytes": len(data), "sha256": hashlib.sha256(data).hexdigest()}
            for name, data in artifacts.items()
        },
    }
    destinations["manifest.json"].write_text(
        json.dumps(manifest, indent=2) + "\n", encoding="utf-8", newline="\n"
    )
    print(f"Created and validated First Block in {args.output.resolve()}")


if __name__ == "__main__":
    main()
