"""An independent reference implementation of the matching semantics.

Written from ``docs/matching-semantics.md``. Never imports from, links
against, or shells out to the C++ build except in ``differential.py``, which
runs the compiled ``engine_replay`` as a black box and compares its text
output.
"""
