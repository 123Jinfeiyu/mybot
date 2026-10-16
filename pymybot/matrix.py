"""The .mt character grammar and its ordered matrix/block representation."""
from __future__ import annotations

from dataclasses import dataclass, field

from .errors import ScriptError

MAX_BLOCK_DEPTH = 32
MAX_SCRIPT_BYTES = 8 * 1024 * 1024


def is_control(name: str) -> bool:
    name = name.strip()
    return name in {"if", "while"} or name.startswith(("if(", "while("))


@dataclass
class Matrix:
    name: str
    rows: list[list[str]] = field(default_factory=list)
    children: list[Matrix] = field(default_factory=list)

    def __post_init__(self) -> None:
        if self.rows and any(len(row) != len(self.rows[0]) for row in self.rows):
            raise ScriptError(f"@{self.name}: every row must match the header column count")

    @property
    def header(self) -> list[str]:
        return self.rows[0] if self.rows else []

    @property
    def data(self) -> list[list[str]]:
        return self.rows[1:]

    def inherited(self, row: int, col: int) -> str:
        if row < 0 or col < 0 or row >= len(self.rows) or col >= len(self.rows[row]):
            raise ScriptError(f"Matrix access out of bounds: row={row} col={col}")
        # Rows are public and may be edited by hot SQL; do not cache stale values.
        while row > 1 and self.rows[row][col] == "-":
            row -= 1
        return self.rows[row][col]

    def require_col(self, name: str) -> int:
        try:
            return self.header.index(name)
        except ValueError:
            raise ScriptError(f"@{self.name} has no '{name}' column") from None


def split_action_column(text: str) -> tuple[str, str]:
    # Deliberately not expression-aware: C++ rejects decimal points in headers.
    if text.count(".") != 1:
        raise ScriptError(f"Action column {text[:160]!r}: expected <device>.<param>")
    device, param = text.split(".")
    if not device or not param:
        raise ScriptError("Action column requires a nonempty device and parameter")
    return device, param


@dataclass
class _Frame:
    matrix: Matrix
    pending: list[list[str]] = field(default_factory=list)
    cells: list[str] = field(default_factory=list)
    token: list[str] = field(default_factory=list)

    def item(self) -> None:
        if self.token:
            self.cells.append("".join(self.token))
            self.token.clear()

    def line(self, line: int) -> None:
        self.item()
        if not self.cells:
            return
        rows = self.pending if is_control(self.matrix.name) else self.matrix.rows
        if rows and len(rows[0]) != len(self.cells):
            raise ScriptError(f"Matrix line {line}: got {len(self.cells)} cells, expected "
                              f"{len(rows[0])} (header column count)")
        rows.append(self.cells)
        self.cells = []

    def flush(self) -> None:
        if self.cells or self.token:
            raise ScriptError("Data row must end with ';' before an @ block boundary")
        if self.pending:
            self.matrix.children.append(Matrix("Action", self.pending))
            self.pending = []


def parse_matrices(text: str) -> list[Matrix]:
    """Parse all top-level blocks; whitespace never quotes an expression cell."""
    if len(text.encode("utf-8")) > MAX_SCRIPT_BYTES:
        raise ScriptError(f"Script exceeds the {MAX_SCRIPT_BYTES} byte limit")
    # CRLF and LF have identical semantics on every host.
    text = text.replace("\r\n", "\n")
    stack: list[_Frame] = []
    out: list[Matrix] = []
    scratch = _Frame(Matrix(""))
    header: list[str] = []
    line = 1
    at_start, comment, naming = True, False, False

    def builder() -> _Frame:
        return stack[-1] if stack else scratch

    def commit_header() -> None:
        name = "".join(header)
        header.clear()
        if not name:
            if not stack:
                raise ScriptError(f"Line {line}: extra '@', no block to close")
            frame = stack.pop()
            frame.flush()
            (stack[-1].matrix.children if stack else out).append(frame.matrix)
        else:
            if stack:
                if not is_control(stack[-1].matrix.name):
                    raise ScriptError(f"Line {line}: leaf @{stack[-1].matrix.name} "
                                      f"cannot contain nested @{name[:160]}")
                stack[-1].flush()
            if len(stack) >= MAX_BLOCK_DEPTH:
                raise ScriptError(f"Block nesting depth exceeds {MAX_BLOCK_DEPTH}")
            stack.append(_Frame(Matrix(name)))

    for char in text:
        if not comment and char == "#":
            comment = True
            if naming:
                commit_header()
                naming = False
            else:
                builder().item()
        elif comment:
            if char == "\n":
                at_start, comment = True, False
                line += 1
        elif at_start and char in " \t\r":
            continue
        elif at_start and char == "\n":
            line += 1
        elif at_start and char == "@":
            at_start, naming = False, True
            header.clear()
        elif at_start:
            at_start = False
            if char == ";":
                builder().line(line)
                at_start = True
            else:
                builder().token.append(char)
        elif naming and char in " \t\r\n":
            commit_header()
            naming, at_start = False, True
            if char == "\n":
                line += 1
        elif naming:
            header.append(char)
        elif char in " \t\r\n":
            builder().item()
            if char == "\n":
                line += 1
        elif char == ";":
            builder().line(line)
            at_start = True
        else:
            builder().token.append(char)
    if naming:
        commit_header()
    if stack:
        raise ScriptError(f"EOF at line {line}: unclosed block @{stack[-1].matrix.name[:160]}")
    return out
