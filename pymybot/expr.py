"""Bounded .mt expression parser, with no Python code evaluation."""
from __future__ import annotations

import math
import re
from collections.abc import Callable

from .errors import ScriptError

Value = float | str | bool
_IDENT = re.compile(r"[A-Za-z_][A-Za-z_0-9]*")
_DECIMAL = re.compile(r"(?:[0-9]+(?:\.[0-9]*)?|\.[0-9]+)(?:[eE][+-]?[0-9]+)?")
_HEX = re.compile(r"[+-]?0[xX](?:[0-9a-fA-F]+(?:\.[0-9a-fA-F]*)?|\.[0-9a-fA-F]+)(?:[pP][+-]?[0-9]+)?")
_PRECEDENCE = {"||": 1, "&&": 2, "<": 3, ">": 3, "<=": 3, ">=": 3,
               "==": 3, "!=": 3, "+": 4, "-": 4, "*": 5, "/": 5, "%": 5}


def expr_body(text: str) -> str | None:
    return text[2:-1] if text.startswith("$(") and text.endswith(")") else None


def render(value: Value) -> str:
    if isinstance(value, str):
        return value
    if isinstance(value, bool):
        return "true" if value else "false"
    if not isinstance(value, (float, int)):
        raise ScriptError("Only string/number/bool values can be rendered")
    try:
        number = float(value)
    except OverflowError:
        raise ScriptError("expression evaluates to a non-finite value") from None
    if not math.isfinite(number):
        raise ScriptError("expression evaluates to a non-finite value")
    return format(number, ".15g")


def parse_number(text: str) -> float:
    """Strict finite strtod-like literals for matrix cells (not expressions)."""
    raw = text.lstrip(" \t\r\n\v\f")
    decimal = raw[1:] if raw.startswith(("+", "-")) else raw
    try:
        if _DECIMAL.fullmatch(decimal):
            value = float(raw)
        elif _HEX.fullmatch(raw):
            value = float.fromhex(raw)
        else:
            raise ScriptError(f"Cannot parse matrix cell {text[:160]!r} as Float64")
    except (ValueError, OverflowError):
        raise ScriptError(f"Invalid or non-finite numeric literal {text[:160]!r}") from None
    if not math.isfinite(value):
        raise ScriptError("Numeric literal is non-finite")
    return value


def _kind(value: Value) -> str:
    if isinstance(value, bool):
        return "boolean"
    return "text" if isinstance(value, str) else "number"


def _numeric(value: Value, op: str) -> float:
    if isinstance(value, bool) or not isinstance(value, (float, int)):
        raise ScriptError(f"expression operator '{op}' cannot be applied to a {_kind(value)} operand")
    return float(value)


def _logical(value: Value, op: str) -> bool:
    if not isinstance(value, bool):
        raise ScriptError(f"expression operator '{op}' requires logical operands; "
                          "no implicit true/false conversion")
    return value


def _binary(op: str, a: Value, b: Value) -> Value:
    if op == "+" and (isinstance(a, str) or isinstance(b, str)):
        if isinstance(a, bool) or isinstance(b, bool):
            raise ScriptError("expression '+' cannot be applied to a boolean operand")
        return render(a) + render(b)
    if op in {"==", "!="}:
        if _kind(a) != _kind(b):
            raise ScriptError(f"expression '{op}' cannot compare a {_kind(a)} with a {_kind(b)}")
        if _kind(a) == "number":
            render(a)
            render(b)
        equal = a == b
        return equal if op == "==" else not equal
    x, y = _numeric(a, op), _numeric(b, op)
    if op in {"<", ">", "<=", ">="}:
        render(x)
        render(y)
        if op == "<":
            return x < y
        if op == ">":
            return x > y
        if op == "<=":
            return x <= y
        return x >= y
    if op == "+":
        return x + y
    if op == "-":
        return x - y
    if op == "*":
        return x * y
    if y == 0:
        raise ScriptError("expression division by zero" if op == "/" else "expression modulo by zero")
    if op == "/":
        return x / y
    try:
        return math.fmod(x, y)  # C++ remainder, not Python's floor modulo.
    except ValueError:
        raise ScriptError("expression evaluates to a non-finite value") from None


class _Parser:
    def __init__(self, text: str):
        self.text, self.pos, self.depth, self.ops = text, 0, 0, 0
        self.identifiers: set[str] = set()

    def fail(self, message: str) -> None:
        excerpt = repr(self.text[:120]) + ("..." if len(self.text) > 120 else "")
        raise ScriptError(f"invalid expression {excerpt}: {message} at position {self.pos}")

    def whitespace(self) -> None:
        while self.pos < len(self.text) and self.text[self.pos] in " \t\r\n":
            self.pos += 1

    def peek(self) -> str:
        return self.text[self.pos:self.pos + 1]

    def parse(self, minimum: int = 1):
        node = self.primary()
        while True:
            self.whitespace()
            op = self.text[self.pos:self.pos + 2]
            if op not in _PRECEDENCE:
                op = self.peek()
            if op == "=":
                self.fail("single '=' is not an operator; equality is '=='")
            priority = _PRECEDENCE.get(op, 0)
            if priority < minimum:
                break
            self.pos += len(op)
            self.ops += 1
            if self.ops > 512:
                self.fail("too many operators in one expression")
            node = (op, node, self.parse(priority + 1))
        return node

    def primary(self):
        # Source grammar folds a run of unary signs and nots by parity, in
        # that order (even !! and unary + introduce no type check).
        negative, nots = False, False
        while True:
            self.whitespace()
            c = self.peek()
            if c == "+":
                self.pos += 1
            elif c == "-":
                negative = not negative
                self.pos += 1
            elif c == "!" and self.text[self.pos:self.pos + 2] != "!=":
                nots = not nots
                self.pos += 1
            else:
                break
        c = self.peek()
        if not c:
            self.fail("unexpected end of expression")
        if c == "(":
            self.depth += 1
            if self.depth > 128:
                self.fail("expression nesting too deep")
            self.pos += 1
            node = self.parse()
            self.whitespace()
            if self.peek() != ")":
                self.fail("expected ')'")
            self.pos += 1
            self.depth -= 1
        elif c == '"':
            node = ("literal", self.string())
        elif match := _DECIMAL.match(self.text, self.pos):
            self.pos = match.end()
            value = float(match.group())
            if not math.isfinite(value):
                self.fail("invalid numeric literal (non-finite)")
            node = ("literal", value)
        elif match := _IDENT.match(self.text, self.pos):
            # A hyphen is subtraction, never part of an identifier.
            self.pos = match.end()
            self.identifiers.add(match.group())
            node = ("variable", match.group())
        else:
            self.fail(f"unexpected character {c!r}")
        if negative:
            node = ("negate", node)
        if nots:
            node = ("not", node)
        return node

    def string(self) -> str:
        self.pos += 1
        chars: list[str] = []
        escapes = {"n": "\n", "t": "\t", "r": "\r", "\\": "\\", '"': '"'}
        while self.pos < len(self.text):
            c = self.text[self.pos]
            self.pos += 1
            if c == '"':
                return "".join(chars)
            if c == "\\":
                if self.pos >= len(self.text):
                    break
                c = self.text[self.pos]
                self.pos += 1
                if c not in escapes:
                    self.fail(f"unknown escape sequence {chr(92) + c!r}")
                c = escapes[c]
            chars.append(c)
        self.fail("unterminated string literal")
        return ""  # fail always raises


class Expression:
    def __init__(self, body: str):
        self.body = body
        parser = _Parser(body)
        parser.whitespace()
        if parser.pos == len(body):
            parser.fail("empty expression")
        self._root = parser.parse()
        parser.whitespace()
        if parser.pos != len(body):
            parser.fail("unexpected trailing input")
        self.identifiers = parser.identifiers

    def evaluate(self, resolve: Callable[[str], Value]) -> Value:
        # Explicit evaluation frames keep a 512-operator left-deep tree off
        # Python's call stack, while preserving lazy &&/|| evaluation.
        todo = [(self._root, 0)]
        values: list[Value] = []
        while todo:
            node, phase = todo.pop()
            op = node[0]
            if op == "literal":
                values.append(node[1])
            elif op == "variable":
                value = resolve(node[1])
                if not isinstance(value, (str, bool, int, float)):
                    raise ScriptError(f"Identifier {node[1]!r} did not resolve to a scalar")
                values.append(float(value) if type(value) is int else value)
            elif phase == 0:
                todo.extend([(node, 1), (node[1], 0)])
            elif op in {"negate", "not"}:
                value = values.pop()
                values.append(-_numeric(value, "-") if op == "negate" else not _logical(value, "!"))
            elif phase == 1:
                if op in {"&&", "||"}:
                    left = _logical(values[-1], op)
                    if (op == "&&" and not left) or (op == "||" and left):
                        continue
                todo.extend([(node, 2), (node[2], 0)])
            else:
                right, left = values.pop(), values.pop()
                values.append(_logical(right, op) if op in {"&&", "||"} else _binary(op, left, right))
        value = values.pop()
        if isinstance(value, (float, int)) and not isinstance(value, bool):
            render(value)  # finite-result guard
        return value
