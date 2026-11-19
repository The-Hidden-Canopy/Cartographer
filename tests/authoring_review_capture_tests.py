#!/usr/bin/env python3
"""Exercise a native authoring review with a caller-supplied project and GPU."""

import argparse
import hashlib
import math
from pathlib import Path
import re
import struct
import subprocess


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def tone(value):
    value = max(0.0, value)
    value = value / (1.0 + value)
    srgb = value * 12.92 if value <= 0.0031308 else 1.055 * value ** (1.0 / 2.4) - 0.055
    return min(255, max(0, math.floor(srgb * 255 + 0.5)))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("executable", "project", "hdr", "new_parent"):
        parser.add_argument(name, type=Path)
    parser.add_argument("camera", nargs=7, help="target xyz, distance, yaw, pitch, vertical FOV")
    parser.add_argument("--displaced-object", type=int, help="visible mesh object for diagnostic isolation")
    args = parser.parse_args()
    args.new_parent.mkdir(exist_ok=False)
    before = hashlib.sha256(args.project.read_bytes()).hexdigest()
    hdr_digest = hashlib.sha256(args.hdr.read_bytes()).hexdigest()

    def invoke(name, width="160", height="90", frames="4", temporal="off", camera=None,
               refraction=None, hidden=None):
        root = args.new_parent / name
        command = [str(args.executable), str(args.project), str(args.hdr), str(root), width, height,
                   *(camera or args.camera), frames, temporal]
        if refraction is not None or hidden is not None:
            command.append(refraction or "scene")
        if hidden is not None:
            command.append(str(hidden))
        return root, subprocess.run(command, capture_output=True, text=True, check=False)

    root, result = invoke("off")
    require(result.returncode == 0, f"capture failed: {result.stdout}\n{result.stderr}")
    receipt = (root / "receipt.txt").read_text()
    require(f"source_sha256={before}" in receipt and f"hdr_source_sha256={hdr_digest}" in receipt and
            "source_project=unchanged" in receipt and "review_presentation=journaled_copy" in receipt and
            "transmission=environment_only" in receipt and "temporal_history=off" in receipt and
            (root / "review.journal").stat().st_size > 0, "capture receipt lost a source or limitation")
    rows = [line for line in receipt.splitlines() if line.startswith("frame=")]
    hashes = [re.search(r"display_rgba_sha256=([0-9a-f]{64})", line).group(1) for line in rows]
    require(len(hashes) == 4 and len(set(hashes)) == 4, "turntable frames were not distinct")
    for i in range(4):
        name = "frame.ppm" if i == 0 else f"frame-{i:04d}.ppm"
        require((root / name).read_bytes().startswith(b"P6\n160 90\n255\n"), "invalid frame extent")

    ppm = (root / "frame.ppm").read_bytes()[len(b"P6\n160 90\n255\n"):]
    pfm = (root / "frame.pfm").read_bytes()
    header = b"PF\n160 90\n-1.0\n"
    require(pfm.startswith(header) and len(pfm) == len(header) + 160 * 90 * 12 and len(ppm) == 160 * 90 * 3,
            "HDR or preview output is truncated")
    pfm = pfm[len(header):]
    maximum_error = 0
    for y in range(90):
        for x in range(160):
            linear = struct.unpack_from("<3f", pfm, ((89-y)*160+x)*12)
            require(all(math.isfinite(value) for value in linear), "nonfinite HDR value")
            for c in range(3):
                maximum_error = max(maximum_error, abs(tone(linear[c]) - ppm[(y*160+x)*3+c]))
    require(maximum_error <= 1, f"HDR and displayed tone mapping differ by {maximum_error} bytes")
    old_receipt = (root / "receipt.txt").read_bytes()
    _, existing = invoke("off")
    require(existing.returncode != 0 and (root / "receipt.txt").read_bytes() == old_receipt,
            "existing output was overwritten")
    for name, width, height, frames, temporal in (
            ("zero", "160", "90", "0", "off"),
            ("budget", "3840", "2160", "120", "off"),
            ("mode", "160", "90", "2", "invalid")):
        path, refused = invoke(name, width, height, frames, temporal)
        require(refused.returncode != 0 and not path.exists(), "invalid request created output")
    on_root, enabled = invoke("on", frames="4", temporal="on")
    require(enabled.returncode == 0 and "temporal_history=on" in (on_root / "receipt.txt").read_text(),
            f"temporal review failed: {enabled.stdout}\n{enabled.stderr}")
    if args.displaced_object is not None:
        invalid_root, invalid = invoke("scene-invalid-object", frames="1", hidden=0)
        require(invalid.returncode != 0 and not invalid_root.exists(),
                "invalid diagnostic object created output")
        visible_root, visible = invoke("scene-visible", frames="1", refraction="scene")
        require(visible.returncode == 0, f"scene review failed: {visible.stdout}\n{visible.stderr}")
        hidden_root, hidden = invoke("scene-hidden", frames="1", hidden=args.displaced_object)
        require(hidden.returncode == 0 and
                f"diagnostic_displaced_object={args.displaced_object}" in (hidden_root / "receipt.txt").read_text(),
                f"diagnostic isolation failed: {hidden.stdout}\n{hidden.stderr}")
        require((hidden_root / "frame.ppm").read_bytes() != (visible_root / "frame.ppm").read_bytes(),
                "diagnostic isolation did not change the captured image")
    require(hashlib.sha256(args.project.read_bytes()).hexdigest() == before,
            "review altered the authoritative project")
    print(f"authoring GPU review passed: frames=4 hdr_preview_max_error={maximum_error} "
          "source_unchanged=true temporal_modes=2 refusal_boundaries=4")


if __name__ == "__main__":
    main()
