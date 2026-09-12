"""Turns parsed mcdoc into the density-function table Stratum compiles in.

Everything here is deliberately intolerant. A type expression this cannot
map, a spread it cannot follow, a struct it cannot find: all raise. The
alternative is emitting a table that is quietly missing a field, which the
engine would then read as "absent" and refuse a datapack vanilla accepts.
"""

from __future__ import annotations

import re
from dataclasses import dataclass
from typing import Optional

from mcdoc import Document, Field, Gate, McdocError, Spread, Struct, parse_version

# mcdoc's `%unknown` fallback validates any unlisted type as an empty struct,
# so the schema cannot tell us which no-field types exist. These three are
# observed in vanilla's own data and take no fields; SPEC section 11 allows
# hand-written checks exactly where mcdoc is insufficient, and this is that.
NO_FIELD_TYPES = ("blend_alpha", "blend_offset", "end_islands")


@dataclass
class ResolvedField:
    name: str
    kind: str
    optional: bool
    allows_reference: bool = False
    # For a "List" kind, what one element of it is; "" for everything else.
    # A list is a wrapper rather than a kind of its own so that every scalar
    # kind composes with it — `[MaterialRuleRef]` and `[biome id]` are the
    # same construct over different elements, and a schema that spelled each
    # array field as its own kind would need a new one per element type.
    element_kind: str = ""
    selector_values: tuple[str, ...] = ()
    # One entry per selector value, in the same order: the `///` lines mcdoc
    # documents that value with, joined, or "" for a value with none.
    selector_docs: tuple[str, ...] = ()
    # The `///` lines written above the field itself.
    docs: tuple[str, ...] = ()


@dataclass
class ResolvedType:
    key: str
    fields: tuple[ResolvedField, ...]


def _split_union(text: str) -> list[str]:
    """Splits a union at top-level `|`, respecting brackets and quotes."""
    parts: list[str] = []
    depth = 0
    current = ""
    index = 0
    while index < len(text):
        character = text[index]
        if character in "([{":
            depth += 1
        elif character in ")]}":
            depth -= 1
        elif character == '"':
            end = text.index('"', index + 1)
            current += text[index : end + 1]
            index = end + 1
            continue
        if character == "|" and depth == 0:
            parts.append(current.strip())
            current = ""
        else:
            current += character
        index += 1
    if current.strip():
        parts.append(current.strip())
    return [part for part in parts if part]


_LEADING_ATTRIBUTES = re.compile(r'^((?:#\[\s*\w+\s*=\s*"[^"]*"\s*\]\s*)*)')
_ATTRIBUTE = re.compile(r'#\[\s*(\w+)\s*=\s*"([^"]*)"\s*\]')


def _strip_gate(text: str) -> tuple[Gate, str]:
    match = _LEADING_ATTRIBUTES.match(text)
    gate = Gate()
    if match is not None and match.group(1):
        for name, value in _ATTRIBUTE.findall(match.group(1)):
            if name == "since":
                gate.since = value
            elif name == "until":
                gate.until = value
            elif name != "id":
                # An attribute can narrow or widen what a type accepts, so
                # one this reader does not know is refused rather than
                # dropped -- silently ignoring it is how a generated table
                # comes to disagree with the schema it claims to follow.
                raise McdocError(f"unsupported attribute #[{name}=...] on type {text!r}")
        text = text[match.end() :].strip()
    return gate, text


def _is_identifier(text: str) -> str:
    """Whether a union arm is the bare `#[id="..."] string` half of one.

    Attributes stripped first, so that the gate and the registry annotation
    do not change the answer -- what is left has to be the word `string` and
    nothing else. `[#[id="..."] string]`, an ARRAY of identifiers, is not.
    """
    _, body = _strip_gate(text)
    return body.strip() == "string"


def _unwrap(text: str) -> str:
    text = text.strip()
    while text.startswith("(") and text.endswith(")"):
        inner = text[1:-1].strip()
        if _split_union(inner) == [inner] or len(_split_union(inner)) > 1:
            text = inner
        else:
            break
    return text.strip()


class SchemaBuilder:
    def __init__(self, document: Document, version: str) -> None:
        self.document = document
        self.version = parse_version(version)

    # -- types ---------------------------------------------------------

    def _map_type(self, text: str, where: str) -> ResolvedField:
        """Maps one mcdoc type expression to a field kind."""
        text = _unwrap(text)

        alternatives = _split_union(text)
        if len(alternatives) > 1:
            live = []
            for alternative in alternatives:
                gate, body = _strip_gate(alternative)
                if gate.applies_to(self.version) and body:
                    live.append(body)
            if not live:
                raise McdocError(f"{where}: no alternative applies at this version")
            # `id string | X` is the "reference or inline" shape.
            #
            # Which arm is the identifier is decided by what the arm IS, not
            # by whether the word "string" occurs in it. Those differ: at 26.3
            # `biome_is` is `[#[id=...] string] | #[id=...] string`, where the
            # word occurs in both arms and only one of them is an identifier.
            # The looser test could find no inline arm at all there and died
            # with a StopIteration rather than a refusal naming the field.
            identifiers = [part for part in live if _is_identifier(part)]
            if len(live) == 2 and len(identifiers) == 1:
                inline = next(part for part in live if not _is_identifier(part))
                mapped = self._map_type(inline, where)
                mapped.allows_reference = True
                return mapped
            if len(set(live)) == 1:
                return self._map_type(live[0], where)
            if len(live) == 1:
                return self._map_type(live[0], where)
            mapped = self._map_union(live, where)
            if mapped is not None:
                return mapped
            raise McdocError(f"{where}: cannot map the union {live!r}")

        return self._map_leaf(text, where)

    def _map_union(self, live: list[str], where: str) -> Optional[ResolvedField]:
        """A last look at a union none of the rules above accounted for.

        Nothing in the density-function schema needs one, so the base refuses.
        A subclass whose schema has a union shape of its own -- mcdoc spells a
        vertical anchor as a union of one-field structs -- returns it here
        instead of overriding the whole of _map_type, which is where the
        version gating and the reference/inline rule live.
        """
        del live, where
        return None

    def _map_leaf(self, text: str, where: str) -> ResolvedField:
        """One non-union type expression, by name."""
        name = text.split("@")[0].strip()

        # The names the engine knows are matched before generic alias
        # expansion, for the types whose *contents* this table does not
        # describe -- a nested density function, a spline, a noise.
        #
        # NoiseParametersRef is deliberately NOT among them. It is an alias
        # for `#[id="worldgen/noise"] string | NoiseParameters`, and matching
        # it by name here recorded only the identifier half: a datapack that
        # writes its noise parameters inline is legal to vanilla, and the
        # generated table said it was not. The alias is expanded like any
        # other and the union below is what records both spellings.
        if name == "DensityFunctionRef":
            return ResolvedField("", "Function", False, allows_reference=True)
        if name == "DensityFunction":
            # Inline only: at this version `clamp` may not take a reference.
            return ResolvedField("", "Function", False, allows_reference=False)
        if name == "NoiseParameters":
            # The inline half of NoiseParametersRef. Its own fields are
            # declared in another schema file and are not flattened into this
            # table: they are the same shape as a `worldgen/noise` entry,
            # which the engine already parses in one place. What this table
            # has to record is *that* the inline form is legal here -- see
            # the union above, which sets allows_reference for the id half.
            return ResolvedField("", "Noise", False)
        if name == "CubicSpline":
            return ResolvedField("", "Spline", False)
        if name in ("float", "double", "int", "long", "byte", "short"):
            return ResolvedField("", "Number", False)
        return self._map_declared(name, where)

    def _map_declared(self, name: str, where: str) -> ResolvedField:
        """An enum or an alias this document declares, or a refusal.

        The tail every schema shares. Enum gating in particular is subtle
        enough that a second copy of it in another builder would be a second
        place for a value from the wrong version to get into a table.
        """
        if name in self.document.enums:
            # Filtered by gate: a value added or removed by version is not
            # one this version accepts, and a selector table that listed it
            # anyway would accept a string vanilla refuses.
            live_values = [member for member in self.document.enums[name].members
                           if member.gate.applies_to(self.version)]
            if not live_values:
                raise McdocError(f"{where}: enum {name!r} has no values at this version")
            return ResolvedField(
                "", "Selector", False,
                selector_values=tuple(member.value for member in live_values),
                selector_docs=tuple(" ".join(member.docs) for member in live_values))
        if name in self.document.aliases:
            return self._map_type(self.document.aliases[name], where)
        raise McdocError(f"{where}: no mapping for type {name!r}")

    # -- structs -------------------------------------------------------

    def _flatten(self, struct: Struct, where: str, seen: set[str]) -> list[ResolvedField]:
        fields: list[ResolvedField] = []
        for member in struct.members:
            if not member.gate.applies_to(self.version):
                continue
            if isinstance(member, Spread):
                if member.target is None:
                    fields.extend(
                        self._flatten(Struct(None, member.inline_fields), where, seen))
                else:
                    if member.target in seen:
                        raise McdocError(f"{where}: spread cycle through {member.target!r}")
                    target = self.document.structs.get(member.target)
                    if target is None:
                        raise McdocError(f"{where}: spread of unknown struct {member.target!r}")
                    fields.extend(self._flatten(target, where, seen | {member.target}))
                continue

            assert isinstance(member, Field)
            mapped = self._map_type(member.type_text, f"{where}.{member.name}")
            mapped.name = member.name
            mapped.optional = member.optional
            mapped.docs = tuple(member.docs)
            fields.append(mapped)
        return fields

    # -- the table -----------------------------------------------------

    def build(self, registry: str,
              no_field_types: tuple[str, ...] = NO_FIELD_TYPES) -> list[ResolvedType]:
        by_key: dict[str, ResolvedType] = {}
        for dispatch in self.document.dispatches:
            if registry not in dispatch.registry or not dispatch.gate.applies_to(self.version):
                continue
            for key in dispatch.keys:
                if key.startswith("%"):
                    continue
                struct = self.document.structs.get(dispatch.target)
                if struct is None:
                    raise McdocError(f"dispatch {key!r} names unknown struct {dispatch.target!r}")
                fields = self._flatten(struct, key, {dispatch.target})
                if key in by_key:
                    raise McdocError(f"{key!r} is dispatched twice at this version")
                by_key[key] = ResolvedType(key=key, fields=tuple(fields))

        for key in no_field_types:
            if key not in by_key:
                by_key[key] = ResolvedType(key=key, fields=())

        return [by_key[key] for key in sorted(by_key)]
