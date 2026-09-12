"""A strict, partial reader for mcdoc schema files.

Stratum derives its density-function schema from mcdoc rather than writing it
by hand (SPEC section 11). mcdoc is a real language and this reads only the
subset the worldgen schemas use.

The important property is that it is STRICT: anything it does not understand
raises, rather than being skipped. A reader that quietly ignored a dispatch
it could not parse would emit a table missing a node type, and the engine
would then reject a datapack that vanilla accepts -- the silent-partial
failure SPEC section 8 calls the most severe class of bug.

Doc comments are read rather than discarded. mcdoc uses `///` to document
semantics a type expression cannot carry -- density_function.mcdoc writes the
per-value formulas of DistanceMetric that way -- so a reader that skipped
them would make those unreachable from the generated tables. Plain `//`
comments are addressed to whoever is editing the schema and are skipped.
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
    # The `///` lines written above this field, one per line, in order.
    docs: list[str] = field(default_factory=list)


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
    """`...OtherStruct`, `...struct { ... }`, or `...registry[[field]]`.

    `target` is kept as the text that was written, not as a resolved name:
    the third spelling is a DISPATCH on the value of a sibling field —
    `...minecraft:material_condition[[type]]`, which material_condition.mcdoc
    uses to say "the rest of this struct is whatever `type` selected" — and
    there is no single struct it names. A caller that flattens a struct by
    name will not find one called that and will say so, which is the right
    answer: the concrete types behind such a spread are enumerated from
    `Document.dispatches`, never by following the spread.
    """

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
    # The `<T>` of a generic dispatch, as written, or "". Recorded rather
    # than discarded: a dispatch that is generic in its payload is a
    # different thing from one that is not, and a builder that read the two
    # the same way would be describing a type it had not looked at.
    parameters: str = ""


@dataclass
class EnumValue:
    """One `Name = "value"` of a string enum.

    Its docs and its gate are kept because both change what the enum means:
    density_function.mcdoc documents DistanceMetric's four metrics only in
    `///` lines, and a value can be added or removed by version.
    """

    name: str
    value: str
    docs: list[str] = field(default_factory=list)
    gate: Gate = field(default_factory=Gate)


@dataclass
class Enum:
    name: str
    # In declaration order, including values that belong to other versions.
    # A caller must filter by each member's gate -- see
    # SchemaBuilder._map_type. There is deliberately no accessor that hands
    # out the literals unfiltered: offering one is how a value from a version
    # this build does not target got into a selector table.
    members: list[EnumValue]


@dataclass
class Document:
    structs: dict[str, Struct] = field(default_factory=dict)
    enums: dict[str, Enum] = field(default_factory=dict)
    dispatches: list[Dispatch] = field(default_factory=list)
    aliases: dict[str, str] = field(default_factory=dict)
    # `type Name<T> = ...`. Kept apart from `aliases` on purpose: the body of
    # one mentions its own parameters, so expanding it for a caller that
    # looked it up by bare name would hand back a type mentioning `T`. A
    # lookup here misses, and the caller raises "no mapping for type", which
    # is the honest answer until something actually needs generics.
    generic_aliases: dict[str, str] = field(default_factory=dict)

    def merge(self, other: "Document", source: str = "<mcdoc>") -> None:
        """Folds @p other's declarations into this one.

        mcdoc is written as several files that refer to each other by bare
        name across `use` lines — material_condition.mcdoc's `CaveSurface` is
        declared in mod.mcdoc — and this reader resolves by bare name rather
        than by import provenance. Merging is therefore how a caller gets one
        namespace to resolve against.

        A name declared twice is an error rather than a last-one-wins
        overwrite: two files disagreeing about what a name means is exactly
        the case where silently picking one emits a table that describes
        neither.
        """
        for label, mine, theirs in (("struct", self.structs, other.structs),
                                    ("enum", self.enums, other.enums),
                                    ("type", self.aliases, other.aliases),
                                    ("generic type", self.generic_aliases, other.generic_aliases)):
            for name, declaration in theirs.items():
                if name in mine:
                    raise McdocError(f"{source}: {label} {name!r} is declared in two files")
                mine[name] = declaration
        self.dispatches.extend(other.dispatches)


class _Reader:
    """A character cursor with just enough lookahead for this subset."""

    def __init__(self, text: str, source: str) -> None:
        # Structs declared inside another struct's body, by `...struct Name {}`.
        self.declared_structs: dict[str, "Struct"] = {}
        self.text = text
        self.source = source
        self.position = 0
        # `///` lines seen since the last take_docs(). Trivia is skipped from
        # several places, so the docs are accumulated here and claimed by
        # whichever member reader runs next.
        self.pending_docs: list[str] = []

    def fail(self, what: str) -> "McdocError":
        line = self.text.count("\n", 0, self.position) + 1
        return McdocError(f"{self.source}:{line}: {what}")

    def skip_trivia(self) -> None:
        while self.position < len(self.text):
            character = self.text[self.position]
            if character.isspace():
                self.position += 1
            elif self.text.startswith("///", self.position):
                self.position += 3
                self.pending_docs.append(self.rest_of_line().strip())
            elif self.text.startswith("//", self.position):
                end = self.text.find("\n", self.position)
                self.position = len(self.text) if end < 0 else end
            else:
                return

    def take_docs(self) -> list[str]:
        """The doc comments seen since the last call, and forgets them."""
        docs = self.pending_docs
        self.pending_docs = []
        return docs

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

    def generic_parameters(self) -> str:
        """A `<...>` parameter list written immediately after a name, or "".

        Adjacency is required rather than skipped-to. A `<` on the next line
        is not a parameter list in any mcdoc this reader has met, and taking
        one would silently swallow the start of the next declaration.
        """
        if self.position >= len(self.text) or self.text[self.position] != "<":
            return ""
        return self.balanced("<", ">")

    def dispatch_target(self) -> str:
        """A spread's target as written: `Name`, or `namespace:name[[field]]`.

        The second shape dispatches on a sibling field's value, so it names no
        single struct; the text is kept and the caller decides. See Spread.
        """
        self.skip_trivia()
        start = self.position
        match = re.compile(r"[A-Za-z_][A-Za-z0-9_]*(?::[A-Za-z_][A-Za-z0-9_]*)*").match(
            self.text, self.position)
        if match is None:
            raise self.fail("expected a spread target")
        self.position = match.end()
        if self.position < len(self.text) and self.text[self.position] == "[":
            self.balanced("[", "]")
        self.generic_parameters()
        return self.text[start : self.position]

    def string_literal(self) -> str:
        self.skip_trivia()
        if not self.text.startswith('"', self.position):
            raise self.fail("expected a string literal")
        end = self.text.find('"', self.position + 1)
        if end < 0:
            raise self.fail("unterminated string literal")
        value = self.text[self.position + 1 : end]
        self.position = end + 1
        return value

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
        if reader.take("}"):
            # Docs written against nothing -- above the closing brace --
            # belong to no member and must not drift onto the next one.
            reader.take_docs()
            return members

        gate = _read_attributes(reader)
        # After the attributes, so that a field documented above its
        # `#[since=...]` and one documented below it read the same.
        docs = reader.take_docs()

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
                members.append(Spread(target=reader.dispatch_target(), gate=gate))
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
            members.append(Field(name=name, type_text=_read_type(reader), optional=optional,
                                 gate=gate, docs=docs))

        reader.take(",")


def _read_enum_body(reader: _Reader) -> list[EnumValue]:
    """Reads `{ Name = "value", ... }`, keeping each value's docs and gate.

    Read member by member rather than by a regex over the body text, which
    is what this used to be: a regex finds the string literals and nothing
    else, so the `///` formulas density_function.mcdoc attaches to
    DistanceMetric's values had nowhere to go.
    """
    members: list[EnumValue] = []
    reader.expect("{")
    while True:
        reader.skip_trivia()
        if reader.take("}"):
            reader.take_docs()
            return members

        gate = _read_attributes(reader)
        docs = reader.take_docs()
        name = reader.identifier()
        reader.expect("=")
        members.append(EnumValue(name=name, value=reader.string_literal(), docs=docs, gate=gate))
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


def parse_inline_struct(text: str, source: str = "<mcdoc>") -> Optional[Struct]:
    """A `struct { ... }` written inside a type expression, or None.

    Type expressions are kept as text (see _read_type), so a caller that has
    to look INSIDE one reads it back through here rather than with a regex
    over braces. mcdoc spells a vertical anchor as a union of one-field
    structs, and which field each of those declares is the whole content of
    the type -- `absolute`, `above_bottom`, `below_top`.

    Anything after the closing brace raises: a caller asking what this struct
    declares would otherwise be told, and would act on, half a type.
    """
    reader = _Reader(text.strip(), source)
    if not reader.take("struct"):
        return None
    reader.skip_trivia()
    name = None if reader.peek("{") else reader.identifier()
    members = _read_struct_body(reader)
    if not reader.at_end():
        raise reader.fail("trailing text after a struct written inside a type")
    return Struct(name=name, members=members)


def parse(text: str, source: str = "<mcdoc>") -> Document:
    """Reads a whole mcdoc file. Raises on any construct not covered here."""
    document = Document()
    reader = _Reader(text, source)

    while not reader.at_end():
        gate = _read_attributes(reader)
        reader.skip_trivia()
        # Docs on a top-level construct are read so they cannot drift onto
        # the next one; nothing in the generated tables carries them yet.
        reader.take_docs()

        if reader.peek("use "):
            reader.rest_of_line()
            continue

        if reader.take("dispatch"):
            reader.skip_trivia()
            target_registry = reader.rest_of_line_until("[")
            keys = [key.strip() for key in reader.balanced("[", "]").split(",") if key.strip()]
            # `dispatch minecraft:int_provider[constant]<T> to ...` — a
            # dispatch can be generic in the type it carries.
            parameters = reader.generic_parameters()
            reader.skip_trivia()
            reader.expect("to")
            reader.skip_trivia()
            if reader.peek("struct"):
                reader.expect("struct")
                reader.skip_trivia()
                # A dispatch may name its struct or declare it anonymously,
                # as `%unknown` does with an empty one.
                if reader.peek("{"):
                    # Named for its file as well as its position: several
                    # files declare one, and Document.merge refuses a name
                    # declared twice — correctly, since two files disagreeing
                    # about a name is exactly what it is there to catch.
                    name = f"<anonymous {source}:{len(document.structs)}>"
                else:
                    name = reader.identifier()
                document.structs[name] = Struct(name=name, members=_read_struct_body(reader))
                document.dispatches.append(Dispatch(keys=keys, target=name, gate=gate))
            elif reader.peek("("):
                # A union of alternatives rather than one struct — the int
                # providers are written this way. The text is kept whole, and
                # a caller that tries to flatten it will not find a struct
                # under that name and will say so. That refusal is correct
                # until something Stratum generates actually dispatches into a
                # union, which nothing at the pinned version does.
                document.dispatches.append(
                    Dispatch(keys=keys, target="(" + reader.balanced("(", ")") + ")", gate=gate))
            else:
                target = reader.identifier() + reader.generic_parameters()
                document.dispatches.append(Dispatch(keys=keys, target=target, gate=gate))
            document.dispatches[-1].registry = target_registry.strip()
            document.dispatches[-1].parameters = parameters
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
            members = _read_enum_body(reader)
            if not members:
                raise reader.fail(f"enum {name} has no values")
            document.enums[name] = Enum(name=name, members=members)
            continue

        if reader.take("type"):
            reader.skip_trivia()
            name = reader.identifier()
            parameters = reader.generic_parameters()
            reader.expect("=")
            body = _read_type(reader)
            if parameters:
                document.generic_aliases[name] = body
            else:
                document.aliases[name] = body
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
