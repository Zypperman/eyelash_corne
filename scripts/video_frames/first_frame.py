"""Placeholder frame-selection algorithm: keep the first frame of each section."""

from __future__ import annotations

from typing import Iterator, List

from split_video import Frame


def first_frame(frames: Iterator[Frame], section: int) -> List[Frame]:
    first = next(frames, None)
    return [first] if first is not None else []
