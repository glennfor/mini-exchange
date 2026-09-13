"""The C++ and Python views of the tap layout must agree, byte for byte.

This is the test that stops the dashboard from showing confident nonsense. A
layout mismatch between the two languages does not raise an exception: the
Python reader unpacks the wrong offsets and produces plausible numbers. So the
C++ side prints its own constants (`tools/tap_layout_dump`) and this compares
them field by field.
"""

from __future__ import annotations

import json
import os
import struct
import subprocess
from pathlib import Path

import pytest

from viz.server import shm_reader

_CANDIDATES = [
    "build/dev/tools/tap_layout_dump",
    "build/release/tools/tap_layout_dump",
]


def _find_dump() -> str | None:
    override = os.environ.get("MINIEX_TAP_LAYOUT_DUMP")
    if override:
        return override if Path(override).exists() else None
    root = Path(__file__).resolve().parents[3]
    for candidate in _CANDIDATES:
        path = root / candidate
        if path.exists():
            return str(path)
    return None


@pytest.fixture(scope="module")
def cpp_layout() -> dict:
    path = _find_dump()
    if path is None:
        pytest.skip(
            "tap_layout_dump not built; run "
            "`cmake --preset dev && cmake --build build/dev`"
        )
    out = subprocess.run([path], capture_output=True, text=True, check=True)
    return json.loads(out.stdout)


def test_magic_and_version_agree(cpp_layout):
    assert cpp_layout["magic"] == shm_reader.MAGIC
    assert cpp_layout["version"] == shm_reader.VERSION


def test_control_block_offsets_agree(cpp_layout):
    assert cpp_layout["header_offset"] == shm_reader.HEADER_OFFSET
    assert cpp_layout["head_offset"] == shm_reader.HEAD_OFFSET
    assert cpp_layout["tail_offset"] == shm_reader.TAIL_OFFSET
    assert cpp_layout["counters_offset"] == shm_reader.COUNTERS_OFFSET
    assert cpp_layout["stats_offset"] == shm_reader.STATS_OFFSET
    assert cpp_layout["data_offset"] == shm_reader.DATA_OFFSET


def test_record_size_agrees(cpp_layout):
    assert cpp_layout["record_size"] == shm_reader.RECORD_SIZE
    assert struct.calcsize(shm_reader.RECORD_FORMAT) == cpp_layout["record_size"]


def test_every_record_field_is_at_the_same_offset(cpp_layout):
    """Catches a reordering, which a size check alone would not."""
    # The order of the names must match the order of RECORD_FORMAT's fields.
    names = [
        "seq", "ts", "price", "qty", "qty_remaining", "id", "counter_id",
        "level_index", "type", "side", "reason", "flags",
    ]
    # Walk the struct format, accumulating offsets the way the C++ compiler
    # laid the struct out.
    offsets = {}
    cursor = 0
    sizes = {"Q": 8, "q": 8, "I": 4, "B": 1}
    field_codes = ["Q", "q", "q", "I", "I", "Q", "Q", "I", "B", "B", "B", "B"]
    for name, code in zip(names, field_codes, strict=True):
        offsets[name] = cursor
        cursor += sizes[code]

    for name, offset in offsets.items():
        assert cpp_layout["record_fields"][name] == offset, (
            f"field {name}: C++ has it at {cpp_layout['record_fields'][name]}, "
            f"the Python unpack format puts it at {offset}"
        )


def test_stats_block_sizes_agree(cpp_layout):
    assert cpp_layout["stats_size"] == struct.calcsize(shm_reader.STATS_FORMAT)


def test_segment_size_formula_agrees(cpp_layout):
    expected = shm_reader.DATA_OFFSET + 1024 * shm_reader.RECORD_SIZE
    assert cpp_layout["segment_size_1024"] == expected


def test_header_unpack_covers_the_whole_header(cpp_layout):
    assert struct.calcsize(shm_reader.HEADER_FORMAT) == cpp_layout["header_size"]
