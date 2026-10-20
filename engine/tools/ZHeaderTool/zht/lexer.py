"""Minimal C++ tokenizer for ZHeaderTool.

Only what the header tool needs:
  * identifiers, numbers, string / char literals (incl. raw strings),
    punctuation (single chars, plus '::')
  * comments are dropped
  * preprocessor directives are dropped from the token stream, but
    `#include` directives are recorded separately (the tool validates
    that `<Name>.generated.h` is the last include of an annotated header)

Every token carries its 1-based source line for diagnostics.
"""

from __future__ import annotations

import re
from dataclasses import dataclass, field
from typing import List

IDENT = "ident"
NUMBER = "number"
STRING = "string"
CHAR = "char"
PUNCT = "punct"


@dataclass(frozen=True)
class Token:
    kind: str
    text: str
    line: int

    def is_punct(self, text: str) -> bool:
        return self.kind == PUNCT and self.text == text

    def is_ident(self, text: str = "") -> bool:
        return self.kind == IDENT and (not text or self.text == text)


@dataclass(frozen=True)
class IncludeDirective:
    path: str
    line: int


@dataclass
class LexResult:
    tokens: List[Token] = field(default_factory=list)
    includes: List[IncludeDirective] = field(default_factory=list)


class LexError(Exception):
    def __init__(self, line: int, message: str):
        super().__init__(message)
        self.line = line
        self.message = message


_IDENT_START = re.compile(r"[A-Za-z_]")
_IDENT_BODY = re.compile(r"[A-Za-z0-9_]*")
_NUMBER = re.compile(r"(?:0[xX][0-9a-fA-F']+|0[bB][01']+|[0-9][0-9']*(?:\.[0-9']*)?(?:[eE][+-]?[0-9]+)?|\.[0-9]+(?:[eE][+-]?[0-9]+)?)[A-Za-z_]*")
_INCLUDE = re.compile(r'#\s*include\s*([<"])([^>"]+)[>"]')
# String-literal prefixes: L, u, U, u8, optionally followed by R for raw.
_STRING_PREFIX = re.compile(r"(?:u8|u|U|L)?R?\"")


def tokenize(text: str) -> LexResult:
    result = LexResult()
    i = 0
    n = len(text)
    line = 1
    at_line_start = True

    while i < n:
        c = text[i]

        if c == "\n":
            line += 1
            i += 1
            at_line_start = True
            continue
        if c in " \t\r\f\v":
            i += 1
            continue

        # Comments.
        if c == "/" and i + 1 < n:
            nxt = text[i + 1]
            if nxt == "/":
                while i < n and text[i] != "\n":
                    # Line comments continue across backslash-newline.
                    if text[i] == "\\" and i + 1 < n and text[i + 1] == "\n":
                        line += 1
                        i += 2
                        continue
                    i += 1
                continue
            if nxt == "*":
                end = text.find("*/", i + 2)
                if end < 0:
                    raise LexError(line, "unterminated block comment")
                line += text.count("\n", i, end)
                i = end + 2
                continue

        # Preprocessor directive: drop the whole logical line.
        if c == "#" and at_line_start:
            start = i
            start_line = line
            while i < n:
                if text[i] == "\\" and i + 1 < n and text[i + 1] == "\n":
                    line += 1
                    i += 2
                    continue
                if text[i] == "\n":
                    break
                # A comment may follow the directive on the same line.
                if text.startswith("/*", i):
                    end = text.find("*/", i + 2)
                    if end < 0:
                        raise LexError(line, "unterminated block comment")
                    line += text.count("\n", i, end)
                    i = end + 2
                    continue
                if text.startswith("//", i):
                    while i < n and text[i] != "\n":
                        i += 1
                    break
                i += 1
            directive = text[start:i]
            m = _INCLUDE.match(directive)
            if m:
                result.includes.append(IncludeDirective(m.group(2).strip(), start_line))
            continue

        at_line_start = False

        # String literals (with optional encoding prefix / raw).
        m = _STRING_PREFIX.match(text, i)
        if m and (c == '"' or _IDENT_START.match(c)):
            prefix = m.group(0)
            if prefix.endswith('R"'):
                i, line = _lex_raw_string(text, m.end(), line, result)
            else:
                i, line = _lex_quoted(text, m.end(), '"', line, STRING, result)
            continue

        if c == "'":
            i, line = _lex_quoted(text, i + 1, "'", line, CHAR, result)
            continue

        if _IDENT_START.match(c):
            m = _IDENT_BODY.match(text, i + 1)
            end = m.end()
            result.tokens.append(Token(IDENT, text[i:end], line))
            i = end
            continue

        if c.isdigit() or (c == "." and i + 1 < n and text[i + 1].isdigit()):
            m = _NUMBER.match(text, i)
            end = m.end() if m else i + 1
            result.tokens.append(Token(NUMBER, text[i:end], line))
            i = end
            continue

        if c == ":" and i + 1 < n and text[i + 1] == ":":
            result.tokens.append(Token(PUNCT, "::", line))
            i += 2
            continue

        result.tokens.append(Token(PUNCT, c, line))
        i += 1

    return result


def _lex_quoted(text: str, i: int, quote: str, line: int, kind: str, result: LexResult):
    start_line = line
    buf = []
    n = len(text)
    while i < n:
        c = text[i]
        if c == "\\" and i + 1 < n:
            buf.append(text[i : i + 2])
            if text[i + 1] == "\n":
                line += 1
            i += 2
            continue
        if c == quote:
            result.tokens.append(Token(kind, "".join(buf), start_line))
            return i + 1, line
        if c == "\n":
            raise LexError(start_line, "unterminated %s literal" % ("string" if quote == '"' else "character"))
        buf.append(c)
        i += 1
    raise LexError(start_line, "unterminated literal")


def _lex_raw_string(text: str, i: int, line: int, result: LexResult):
    start_line = line
    paren = text.find("(", i)
    if paren < 0:
        raise LexError(line, "malformed raw string literal")
    delim = text[i:paren]
    terminator = ")" + delim + '"'
    end = text.find(terminator, paren + 1)
    if end < 0:
        raise LexError(line, "unterminated raw string literal")
    body = text[paren + 1 : end]
    line += body.count("\n")
    result.tokens.append(Token(STRING, body, start_line))
    return end + len(terminator), line
