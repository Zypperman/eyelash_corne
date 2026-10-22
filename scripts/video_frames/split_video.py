#!/usr/bin/env python3
"""Split a video into N equal sections, run a frame-selection algorithm on
each section, and save the frames it returns into a subfolder.

The algorithm is injected: any callable matching ``FrameAlgorithm`` works.
Pass it directly via ``process_video(..., algorithm=fn)`` or on the command
line as ``--algorithm module:function`` (default: first_frame:first_frame).

    python split_video.py input.mp4
    python split_video.py input.mp4 -n 30 -o frames --algorithm my_algo:pick
"""

from __future__ import annotations

import argparse
import importlib
import sys
from pathlib import Path
from typing import Callable, Iterable, Iterator, List, Tuple

import cv2
import numpy as np

# (absolute frame index in the video, BGR image)
Frame = Tuple[int, np.ndarray]

# An algorithm receives a lazy iterator over one section's frames plus the
# section index, and returns the frames to keep. Frames are streamed so an
# algorithm that only needs a few (like first_frame) never decodes the rest;
# one that needs random access can simply call list() on the iterator.
FrameAlgorithm = Callable[[Iterator[Frame], int], Iterable[Frame]]


def section_bounds(total_frames: int, num_sections: int) -> List[Tuple[int, int]]:
    """Return [start, end) frame ranges splitting the video as evenly as possible."""
    if total_frames <= 0:
        raise ValueError("video has no frames")
    num_sections = min(num_sections, total_frames)
    edges = np.linspace(0, total_frames, num_sections + 1).astype(int)
    return [(int(edges[i]), int(edges[i + 1])) for i in range(num_sections)]


def iter_section(cap: cv2.VideoCapture, start: int, end: int) -> Iterator[Frame]:
    """Yield frames in [start, end), seeking to ``start`` first."""
    cap.set(cv2.CAP_PROP_POS_FRAMES, start)
    for idx in range(start, end):
        ok, frame = cap.read()
        if not ok:
            return
        yield idx, frame


def process_video(
    video_path: Path,
    output_dir: Path,
    algorithm: FrameAlgorithm,
    num_sections: int = 30,
    image_ext: str = "png",
) -> List[Path]:
    cap = cv2.VideoCapture(str(video_path))
    if not cap.isOpened():
        raise RuntimeError(f"could not open video: {video_path}")

    try:
        total = int(cap.get(cv2.CAP_PROP_FRAME_COUNT))
        output_dir.mkdir(parents=True, exist_ok=True)
        saved: List[Path] = []

        for section, (start, end) in enumerate(section_bounds(total, num_sections)):
            frames = iter_section(cap, start, end)
            for frame_idx, image in algorithm(frames, section):
                out = output_dir / f"section_{section:02d}_frame_{frame_idx:06d}.{image_ext}"
                if not cv2.imwrite(str(out), image):
                    raise RuntimeError(f"failed to write {out}")
                saved.append(out)
        return saved
    finally:
        cap.release()


def load_algorithm(spec: str) -> FrameAlgorithm:
    """Resolve 'module:function' to a callable, importing from this folder too."""
    module_name, _, func_name = spec.partition(":")
    if not func_name:
        raise ValueError(f"algorithm must be 'module:function', got {spec!r}")
    sys.path.insert(0, str(Path(__file__).resolve().parent))
    return getattr(importlib.import_module(module_name), func_name)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("video", type=Path)
    parser.add_argument("-n", "--sections", type=int, default=30)
    parser.add_argument("-o", "--output", type=Path, help="default: <video name>_frames/ next to the video")
    parser.add_argument("--algorithm", default="first_frame:first_frame")
    parser.add_argument("--ext", default="png", help="image format for saved frames")
    args = parser.parse_args()

    output = args.output or args.video.with_name(f"{args.video.stem}_frames")
    saved = process_video(args.video, output, load_algorithm(args.algorithm), args.sections, args.ext)
    print(f"saved {len(saved)} frames to {output}")


if __name__ == "__main__":
    main()
