"""A strict, partial reader for mcdoc schema files.

Stratum derives its density-function schema from mcdoc rather than writing it
by hand (SPEC section 11). mcdoc is a real language and this reads only the
subset the worldgen schemas use.

The important property is that it is STRICT: anything it does not understand
raises, rather than being skipped. A reader that quietly ignored a dispatch
it could not parse would emit a table missing a node type, and the engine
would then reject a datapack that vanilla accepts -- the silent-partial
failure SPEC section 8 calls the most severe class of bug.
"""

from __future__ import annotations

import re
from dataclasses import dataclass, field
from typing import Optional


class McdocError(Exception):
    """Raised for anything this reader cannot account for."""


def parse_version(text: str) -> tuple[int, ...]:
    """`1.21.11` -> (1, 21, 11). Compared component-wise, so 1.21.11 sorts
    before 26.3 and 1.21.2 before 1.21.11 -- neither of which is true of a
    plain string comparison."""
    if not re.fullmatch(r"\d+(\.\d+)*", text):
        raise McdocError(f"cannot read {text!r} as a version")
    return tuple(int(part) for part in text.split("."))


@dataclass
class Gate:
    """A `#[since=...]` / `#[until=...]` pair. `until` is exclusive."""

    since: Optional[str] = None
    until: Optional[str] = None

    def applies_to(self, version: tuple[int, ...]) -> bool:
        if self.since is not None and version < parse_version(self.since):
            return False
        if self.until is not None and version >= parse_version(self.until):
            return False
        return True


@dataclass
class Field:
    name: str
    type_text: str
    optional: bool = False
    gate: Gate = field(default_factory=Gate)


@dataclass
class MapEntry:
    """`[KeyType]: ValueType` — a struct keyed by an arbitrary string rather
    than by a fixed set of field names. Recorded rather than skipped: a
    schema reader that quietly dropped one would describe a struct with no
    fields, which is a shape the engine would then accept anything for."""

    key_type_text: str
    value_type_text: str
    gate: "Gate"


@dataclass
class Spread:
    """`...OtherStruct` or `...struct { ... }` inside a struct body."""

    target: Optional[str]
    inline_fields: list["Field | Spread"] = field(default_factory=list)
    gate: Gate = field(default_factory=Gate)


@dataclass
class Struct:
    name: Optional[str]
    members: list["Field | Spread | MapEntry"] = field(default_factory=list)


@dataclass
class Dispatch:
    keys: list[str]
    target: str
    gate: Gate = field(default_factory=Gate)
    registry: str = ""


@dataclass
class Enum:
    name: str
    values: list[str]


@dataclass
class Document:
    structs: dict[str, Struct] = field(default_factory=dict)
    enums: dict[str, Enum] = field(default_factory=dict)
    dispatches: list[Dispatch] = field(default_factory=list)
    aliases: dict[str, str] = field(default_factory=dict)


class _Reader:
    """A character cursor with just enough lookahead for this subset."""

    def __init__(self, text: str, source: str) -> None:
        # Structs declared inside another struct's body, by `...struct Name {}`.
        self.declared_structs: dict[str, "Struct"] = {}
        self.text = text
        self.source = source
        self.position = 0

    def fail(self, what: str) -> "McdocError":
        line = self.text.count("\n", 0, self.position) + 1
        return McdocError(f"{self.source}:{line}: {what}")

    def skip_trivia(self) -> None:
        while self.position < len(self.text):
            character = self.text[self.position]
            if character.isspace():
                self.position += 1
            elif self.text.startswith("//", self.position):
                end = self.text.find("\n", self.position)
                self.position = len(self.text) if end < 0 else end
            else:
                return

    def at_end(self) -> bool:
        self.skip_trivia()
        return self.position >= len(self.text)

    def peek(self, literal: str) -> bool:
        self.skip_trivia()
        return self.text.startswith(literal, self.position)

    def take(self, literal: str) -> bool:
        if self.peek(literal):
            self.position += len(literal)
            return True
        return False

    def expect(self, literal: str) -> None:
        if not self.take(literal):
            raise self.fail(f"expected {literal!r}")

    def identifier(self) -> str:
        self.skip_trivia()
        match = re.compile(r"[A-Za-z_][A-Za-z0-9_]*").match(self.text, self.position)
        if match is None:
            raise self.fail("expected an identifier")
        self.position = match.end()
        return match.group(0)

    def rest_of_line(self) -> str:
        end = self.text.find("\n", self.position)
        end = len(self.text) if end < 0 else end
        line = self.text[self.position : end]
        self.position = end
        return line

    def rest_of_line_until(self, stop: str) -> str:
        self.skip_trivia()
        end = self.text.find(stop, self.position)
        if end < 0:
            raise self.fail(f"expected {stop!r} on this line")
        text = self.text[self.position : end]
        self.position = end
        return text

    def balanced(self, open_character: str, close_character: str) -> str:
        """Reads a bracketed run, returning its inside, brackets balanced."""
        self.expect(open_character)
        start = self.position
        depth = 1
        while self.position < len(self.text):
            character = self.text[self.position]
            if character == open_character:
                depth += 1
            elif character == close_character:
                depth -= 1
                if depth == 0:
                    inside = self.text[start : self.position]
                    self.position += 1
                    return inside
            elif character == '"':
                self.position = self.text.index('"', self.position + 1)
            self.position += 1
        raise self.fail(f"unbalanced {open_character!r}")


_ATTRIBUTE = re.compile(r'#\[\s*(since|until|id)\s*=\s*"([^"]*)"\s*\]')


def _read_attributes(reader: _Reader) -> Gate:
    """Consumes any run of `#[...]` attributes, keeping the version gates.

    An attribute this reader does not know is an error: attributes change
    meaning, and ignoring one could silently widen or narrow the schema.
    """
    gate = Gate()
    while True:
        reader.skip_trivia()
        if not reader.peek("#["):
            return gate
        match = _ATTRIBUTE.match(reader.text, reader.position)
        if match is None:
            raise reader.fail(f"unsupported attribute {reader.rest_of_line()!r}")
        name, value = match.group(1), match.group(2)
        if name == "since":
            gate.since = value
        elif name == "until":
            gate.until = value
        # `id` marks a string as a registry reference; the type text keeps it.
        reader.position = match.end()


def _read_struct_body(reader: _Reader) -> list["Field | Spread | MapEntry"]:
    """Reads `{ ... }`, returning its members in declaration order."""
    members: list[Field | Spread | MapEntry] = []
    reader.expect("{")
    while True:
        reader.skip_trivia()
        while reader.peek("///"):
            reader.rest_of_line()
            reader.skip_trivia()
        if reader.take("}"):
            return members

        gate = _read_attributes(reader)
        reader.skip_trivia()
        while reader.peek("///"):
            reader.rest_of_line()
            reader.skip_trivia()

        if reader.take("..."):
            reader.skip_trivia()
            if reader.peek("struct"):
                reader.expect("struct")
                reader.skip_trivia()
                # `...struct Name { ... }` both declares a struct and spreads
                # it. The name is not decoration: NoiseGeneratorFlags is
                # written this way, and a reader that took the braces and
                # dropped the name would still be right here — but would be
                # silently wrong the first time something referred to it.
                declared = ""
                if not reader.peek("{"):
                    declared = reader.identifier()
                    reader.skip_trivia()
                fields = _read_struct_body(reader)
                if declared:
                    reader.declared_structs[declared] = Struct(name=declared, members=fields)
                members.append(Spread(target=None, inline_fields=fields, gate=gate))
            else:
                members.append(Spread(target=reader.identifier(), gate=gate))
        elif reader.peek("["):
            key_type = reader.balanced("[", "]")
            reader.skip_trivia()
            reader.expect(":")
            members.append(MapEntry(key_type_text=key_type.strip(),
                                    value_type_text=_read_type(reader), gate=gate))
        else:
            name = reader.identifier()
            optional = reader.take("?")
            reader.expect(":")
            members.append(Field(name=name, type_text=_read_type(reader), optional=optional, gate=gate))

        reader.take(",")


def _read_type(reader: _Reader) -> str:
    """Reads a type expression as text, balanced, up to its terminator.

    The text is kept rather than modelled: mapping it to something the engine
    understands is the caller's job, and a caller that meets a shape it does
    not recognise must say so rather than assume.
    """
    reader.skip_trivia()
    start = reader.position
    depth = 0
    while reader.position < len(reader.text):
        character = reader.text[reader.position]
        if character in "([{":
            depth += 1
        elif character in ")]}":
            if depth == 0:
                break
            depth -= 1
        elif character == '"':
            reader.position = reader.text.index('"', reader.position + 1)
        elif character == "," and depth == 0:
            break
        elif character == "\n" and depth == 0:
            # A field ends at its line unless a bracket is still open.
            ahead = reader.text[reader.position : reader.position + 200].lstrip()
            if not ahead.startswith(("|", "@")):
                break
        reader.position += 1
    text = reader.text[start : reader.position].strip()
    if not text:
        raise reader.fail("expected a type")
    return text


def parse(text: str, source: str = "<mcdoc>") -> Document:
    """Reads a whole mcdoc file. Raises on any construct not covered here."""
    document = Document()
    reader = _Reader(text, source)

    while not reader.at_end():
        while reader.peek("///"):
            reader.rest_of_line()
            reader.skip_trivia()
        if reader.at_end():
            break

        gate = _read_attributes(reader)
        reader.skip_trivia()

        if reader.peek("use "):
            reader.rest_of_line()
            continue

        if reader.take("dispatch"):
            reader.skip_trivia()
            target_registry = reader.rest_of_line_until("[")
            keys = [key.strip() for key in reader.balanced("[", "]").split(",") if key.strip()]
            reader.skip_trivia()
            reader.expect("to")
            reader.skip_trivia()
            if reader.peek("struct"):
                reader.expect("struct")
                reader.skip_trivia()
                # A dispatch may name its struct or declare it anonymously,
                # as `%unknown` does with an empty one.
                if reader.peek("{"):
                    name = f"<anonymous {len(document.structs)}>"
                else:
                    name = reader.identifier()
                document.structs[name] = Struct(name=name, members=_read_struct_body(reader))
                document.dispatches.append(Dispatch(keys=keys, target=name, gate=gate))
            else:
                document.dispatches.append(Dispatch(keys=keys, target=reader.identifier(), gate=gate))
            document.dispatches[-1].registry = target_registry.strip()
            continue

        if reader.take("struct"):
            reader.skip_trivia()
            name = reader.identifier()
            document.structs[name] = Struct(name=name, members=_read_struct_body(reader))
            continue

        if reader.take("enum"):
            reader.expect("(")
            kind = reader.identifier()
            reader.expect(")")
            if kind != "string":
                raise reader.fail(f"only string enums are supported, not {kind!r}")
            name = reader.identifier()
            body = reader.balanced("{", "}")
            values = re.findall(r'=\s*"([^"]*)"', body)
            if not values:
                raise reader.fail(f"enum {name} has no values")
            document.enums[name] = Enum(name=name, values=values)
            continue

        if reader.take("type"):
            reader.skip_trivia()
            name = reader.identifier()
            reader.expect("=")
            document.aliases[name] = _read_type(reader)
            continue

        raise reader.fail(f"unsupported construct: {reader.rest_of_line()[:60]!r}")

    # Structs declared inside another struct's body — `...struct Name {}` —
    # are as real as any other, and folding them in makes a later reference
    # to one resolvable rather than a mysterious unknown type.
    for declared_name, declared in reader.declared_structs.items():
        if declared_name in document.structs:
            raise McdocError(f"{source}: struct {declared_name!r} is declared twice")
        document.structs[declared_name] = declared

    return document
