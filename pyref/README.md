# `pyref/` — the independent reference oracle

This package is a second, independent implementation of the matching
semantics. Its only purpose is to disagree with the C++ engine when the C++
engine is wrong.

**It is written from `docs/matching-semantics.md`, not from the C++ source,
and it never calls into the C++ build.** That independence is the whole
value: an oracle transcribed from the implementation it is checking shares
the implementation's misreading of the spec and catches nothing.

**It passes with no C++ build present at all.** Run `pytest` in a fresh clone
with no `build/` directory and 61 tests pass while 29 skip — the skipped ones
being exactly those that run the compiled engine as a subprocess. That split
is the independence claim made checkable: if the oracle needed the binary to
run its own tests, "independent" would be a word rather than a property.

Deliberately slow and deliberately obvious. Dictionaries and lists, linear
scans, no cleverness anywhere. If a line here needs a comment explaining a
performance trade-off, it is in the wrong file.

## Layout

| File | What |
|---|---|
| `reference_book.py` | price → list of orders, nothing else |
| `reference_engine.py` | every rule in the semantics document |
| `command_io.py` | the shared command and event text formats |
| `replay.py` | CLI: read commands, write the canonical event stream |
| `differential.py` | random command generation, comparison, and shrinking |
| `tests/` | directed tests per rule, plus the differential suite |

## Running

```bash
python -m pyref.replay --commands cmds.txt          # print the event stream
python -m pyref.differential --seeds 1000 --length 1000 \
       --engine build/release/tools/engine_replay   # differential test
pytest pyref
```
