"""The two shared text formats.

Both are specified in ``docs/matching-semantics.md`` and implemented twice on
purpose -- once here and once in ``engine/command_parser.hpp`` /
``engine/event_format.hpp``. A format implemented once and shared as a
library would not be a check on anything.
"""

from __future__ import annotations

from dataclasses import dataclass

# --- commands --------------------------------------------------------------

NEW = "NEW"
CANCEL = "CANCEL"
REPLACE = "REPLACE"


@dataclass
class Command:
    verb: str
    ts: int = 0
    id: int = 0
    side: str = "B"
    price: int = 0
    qty: int = 0
    type: str = "Limit"
    tif: str = "Day"
    participant: int = 0
    symbol: int = 0

    def to_line(self) -> str:
        if self.verb == NEW:
            return (
                f"NEW ts={self.ts} id={self.id} side={self.side} "
                f"price={self.price} qty={self.qty} type={self.type} "
                f"tif={self.tif} participant={self.participant} "
                f"symbol={self.symbol}"
            )
        if self.verb == CANCEL:
            return f"CANCEL ts={self.ts} id={self.id}"
        return f"REPLACE ts={self.ts} id={self.id} price={self.price} qty={self.qty}"


class ParseError(Exception):
    def __init__(self, line_no: int, message: str) -> None:
        super().__init__(f"line {line_no}: {message}")
        self.line_no = line_no
        self.message = message


_SIDES = {"B": "B", "S": "S", "Buy": "B", "Sell": "S"}
_TYPES = {"Limit", "Market"}
_TIFS = {"Day", "IOC", "FOK", "PostOnly"}


def parse_command_line(line: str, line_no: int = 0) -> Command | None:
    """Returns None for blank and comment lines."""
    line = line.split("#", 1)[0].strip()
    if not line:
        return None
    tokens = line.split()
    verb = tokens[0]
    if verb not in (NEW, CANCEL, REPLACE):
        raise ParseError(line_no, f"unknown command '{verb}'")

    cmd = Command(verb=verb)
    have_side = False
    have_qty = False
    for token in tokens[1:]:
        if "=" not in token:
            raise ParseError(line_no, f"expected key=value, got '{token}'")
        key, value = token.split("=", 1)
        if key == "ts":
            cmd.ts = int(value)
        elif key == "id":
            cmd.id = int(value)
        elif key == "price":
            cmd.price = int(value)
        elif key == "qty":
            cmd.qty = int(value)
            have_qty = True
        elif key == "participant":
            cmd.participant = int(value)
        elif key == "symbol":
            cmd.symbol = int(value)
        elif key == "side":
            if value not in _SIDES:
                raise ParseError(line_no, f"bad value for 'side': '{value}'")
            cmd.side = _SIDES[value]
            have_side = True
        elif key == "type":
            if value not in _TYPES:
                raise ParseError(line_no, f"bad value for 'type': '{value}'")
            cmd.type = value
        elif key == "tif":
            if value not in _TIFS:
                raise ParseError(line_no, f"bad value for 'tif': '{value}'")
            cmd.tif = value
        else:
            raise ParseError(line_no, f"unknown field '{key}'")

    if verb == NEW and not have_side:
        raise ParseError(line_no, "NEW requires side")
    if verb in (NEW, REPLACE) and not have_qty:
        raise ParseError(line_no, f"{verb} requires qty")
    return cmd


def parse_commands(text: str) -> list[Command]:
    out = []
    for line_no, line in enumerate(text.splitlines(), start=1):
        cmd = parse_command_line(line, line_no)
        if cmd is not None:
            out.append(cmd)
    return out


def write_commands(commands: list[Command]) -> str:
    return "".join(c.to_line() + "\n" for c in commands)


# --- events ----------------------------------------------------------------


@dataclass
class Event:
    seq: int
    type: str
    id: int = 0
    counter_id: int = 0
    match_id: int = 0
    side: str = "B"
    price: int = 0
    qty: int = 0
    qty_remaining: int = 0
    reason: str = "None"
    is_market: bool = False

    def to_line(self) -> str:
        """The canonical event line. Byte-identical to format_event() in C++."""
        return (
            f"{self.seq} {self.type} id={self.id} counter={self.counter_id} "
            f"match={self.match_id} side={self.side} price={self.price} "
            f"qty={self.qty} rem={self.qty_remaining} reason={self.reason}"
        )
