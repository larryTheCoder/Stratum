"""Turns parsed mcdoc into the surface-rule tables Stratum compiles in.

Surface rules are a second dispatch family alongside the density functions:
`minecraft:material_rule` and `minecraft:material_condition`, each a set of
types selected by a `type` field. The shapes they take are different enough
from a density function's — a block state, a vertical anchor, a list of
rules — that this builder has its own field vocabulary rather than stretching
schema.py's, but the parts that are genuinely shared (the version gating, the
reference-or-inline union rule, enum filtering) are inherited rather than
copied. A second copy of the enum gating in particular is a second place for
a value from the wrong version to reach a table.

Like schema.py this is deliberately intolerant. A type it cannot map, a union
shape it has not met, a list of lists: all raise.

WHAT IS NOT HERE, AND WHY. Five of the fifteen types Stratum resolves are
absent from mcdoc altogether — `bandlands` is not a `material_rule` upstream
declares, and `above_preliminary_surface`, `hole`, `steep` and `temperature`
are not `material_condition`s. They are listed below so the generated enum
covers every type the engine knows, with no fields, exactly as schema.py
already does for `blend_alpha` and `end_islands`. SPEC section 11 allows
hand-written checks where mcdoc is insufficient, and a type the schema does
not mention at all is that.
"""

from __future__ import annotations

from typing import Optional

import mcdoc
from mcdoc import Document, Field, McdocError
from schema import (ResolvedField, ResolvedType, SchemaBuilder, _ATTRIBUTE,  # noqa: F401
                    _LEADING_ATTRIBUTES, _strip_gate)

# Absent from mcdoc entirely, so hand-written here whatever happens. Each
# takes no fields, which is why listing the name is the whole of it.
NO_FIELD_RULE_TYPES = ("bandlands",)
NO_FIELD_CONDITION_TYPES = ("above_preliminary_surface", "hole", "steep", "temperature")


def _split_id(text: str) -> tuple[bool, str]:
    """Splits the leading attributes off a type, saying whether one was `id`.

    `#[id="worldgen/noise"] string` and a plain `string` are both a string to
    mcdoc and are not the same thing to the engine: the first names an entry
    in a registry and is parsed as a resource location, the second is a bare
    name used as a salt (`vertical_gradient`'s `random_name`). Only the
    attribute tells them apart.
    """
    match = _LEADING_ATTRIBUTES.match(text)
    prefix = match.group(1) if match is not None else ""
    # _strip_gate does the validating — an attribute neither it nor this
    # knows raises there rather than being dropped.
    _, body = _strip_gate(text)
    return any(name == "id" for name, _ in _ATTRIBUTE.findall(prefix)), body


def _anchor_spelling(text: str) -> Optional[str]:
    """The field name of a `struct { name: int }`, or None for any other shape."""
    struct = mcdoc.parse_inline_struct(text)
    if struct is None or len(struct.members) != 1:
        return None
    member = struct.members[0]
    if not isinstance(member, Field) or member.type_text.split("@")[0].strip() != "int":
        return None
    return member.name


class SurfaceSchemaBuilder(SchemaBuilder):
    """Reads the two surface-rule dispatch families."""

    # -- types ---------------------------------------------------------

    def _map_leaf(self, text: str, where: str) -> ResolvedField:
        has_id, body = _split_id(text)

        if body.startswith("[") and body.endswith("]"):
            element = self._map_type(body[1:-1], where)
            if element.kind == "List":
                raise McdocError(f"{where}: a list of lists is not a shape this generator emits")
            # The element's own reference/inline rule is what a list carries:
            # a list field is never itself written as an identifier.
            return ResolvedField("", "List", False,
                                 allows_reference=element.allows_reference,
                                 element_kind=element.kind,
                                 selector_values=element.selector_values,
                                 selector_docs=element.selector_docs)

        name = body.split("@")[0].strip()

        # The two wrapper structs, matched by name for the same reason
        # schema.py matches `DensityFunction`: this table does not describe
        # their CONTENTS. What a rule or condition can be is the dispatch
        # family below, enumerated from the dispatches, and a field that
        # names one is a recursion into it.
        #
        # `MaterialRuleRef`/`MaterialConditionRef` are deliberately NOT
        # matched here. Each is an alias for a union that gains an identifier
        # spelling at 26.3, and matching the alias by name would hard-code an
        # answer to a question the schema already answers — the mistake the
        # noise-field bullet in SPEC section 11 records. Expanded, the union
        # sets allowsReference on its own: false at the pinned version,
        # true at 26.3, with nothing here to change.
        if name == "MaterialRule":
            return ResolvedField("", "Rule", False)
        if name == "MaterialCondition":
            return ResolvedField("", "Condition", False)
        if name == "BlockState":
            return ResolvedField("", "BlockState", False)
        if name == "boolean":
            return ResolvedField("", "Boolean", False)
        # Whole numbers are kept apart from `float`/`double`: an offset and a
        # threshold are read out of the JSON as different C++ types, and the
        # schema is the only thing that knows which is which.
        if name in ("int", "long", "byte", "short"):
            return ResolvedField("", "Int", False)
        if name in ("float", "double"):
            return ResolvedField("", "Number", False)
        if name == "string":
            return ResolvedField("", "Id" if has_id else "String", False)
        return self._map_declared(name, where)

    def _map_union(self, live: list[str], where: str) -> Optional[ResolvedField]:
        """A union of one-int-field structs: mcdoc's spelling of an anchor.

        `VerticalAnchor` is written as `struct { absolute: int } | struct {
        above_bottom: int } | ...`, so the set of spellings the engine must
        accept is derived from the union rather than written out — and the
        26.3 addition of `relative_to_sea_level` is a gated arm that drops
        out here on its own.
        """
        spellings = []
        for alternative in live:
            spelling = _anchor_spelling(alternative)
            if spelling is None:
                return None
            spellings.append(spelling)
        if not spellings:
            return None
        if len(set(spellings)) != len(spellings):
            raise McdocError(f"{where}: an anchor spells {spellings!r} twice")
        return ResolvedField("", "Anchor", False, selector_values=tuple(spellings),
                             selector_docs=tuple("" for _ in spellings))

    # -- the tables ----------------------------------------------------

    def build_rules(self) -> list[ResolvedType]:
        return self.build("material_rule", NO_FIELD_RULE_TYPES)

    def build_conditions(self) -> list[ResolvedType]:
        return self.build("material_condition", NO_FIELD_CONDITION_TYPES)


def read(paths: list, version: str) -> tuple[Document, SurfaceSchemaBuilder]:
    """Parses @p paths into one namespace and returns a builder over it.

    Several files rather than one because mcdoc refers across them by bare
    name: `CaveSurface` and `VerticalAnchor` are declared in mod.mcdoc and
    used by material_condition.mcdoc, and this reader resolves by name rather
    than by import provenance.
    """
    document = Document()
    for path in paths:
        document.merge(mcdoc.parse(path.read_text(encoding="utf-8"), str(path)), str(path))
    return document, SurfaceSchemaBuilder(document, version)
