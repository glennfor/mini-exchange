"""Locates the compiled C++ engine, if one has been built.

The reference oracle's own tests must run with no C++ build present -- that is
part of what "independent" means -- so the cross-implementation tests skip
rather than fail when the binary is absent.
"""

from __future__ import annotations

import os
from pathlib import Path

import pytest

_CANDIDATES = [
    "build/release/tools/engine_replay",
    "build/dev/tools/engine_replay",
    "build/release-portable/tools/engine_replay",
]


def _find_engine() -> str | None:
    override = os.environ.get("MINIEX_ENGINE_REPLAY")
    if override:
        return override if Path(override).exists() else None
    root = Path(__file__).resolve().parents[2]
    for candidate in _CANDIDATES:
        path = root / candidate
        if path.exists():
            return str(path)
    return None


@pytest.fixture(scope="session")
def engine_binary() -> str:
    path = _find_engine()
    if path is None:
        pytest.skip(
            "no compiled engine_replay found; build with "
            "`cmake --preset release && cmake --build build/release`"
        )
    return path
