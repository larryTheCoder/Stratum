"""Turns parsed mcdoc into the noise-settings tables Stratum compiles in.

Why this is derived rather than written out by hand, concretely: at 1.21.9
the noise router's `initial_density_without_jaggedness` became
`preliminary_surface_level`. Anyone writing the field list from memory would
almost certainly have written the older name, the loader would have refused
every vanilla noise settings file, and the cause would not have been
obvious. The schema knows; memory does not.

Like schema.py this is deliberately intolerant. A type it cannot map, a
range it cannot read, a member shape it has not met: all raise. A table that
is quietly missing a field is a loader that reads that field as absent.
"""

from __future__ import annotations

import re
from dataclasses import dataclass
from typing import Optional

from mcdoc import Document, Field, MapEntry, McdocError, Spread, Struct
from schema import SchemaBuilder, _split_union, _strip_gate, _unwrap

# The mcdoc type name each settings field carries, mapped to what this engine
# does with it. Kept explicit rather than inferred: "MaterialRuleRef becomes
# a rule we do not interpret yet" is a decision, and it should be written
# down somewhere a reader can find it.
LEAF_KINDS = {
    "BlockState": "BlockState",
    "boolean": "Boolean",
    "int": "Int",
    "NoiseSettings": "Geometry",
    "NoiseRouter": "Router",
    # Surface rules are M4. Loaded, kept, not interpreted.
    "MaterialRuleRef": "MaterialRule",
    # Spawn targets are M4, and are a list of climate parameter points.
    "[ClimateParameters]": "SpawnTarget",
}


@dataclass
class SettingsField:
    name: str
    kind: str
    optional: bool = False
    minimum: Optional[int] = None
    maximum: Optional[int] = None


def _split_range(text: str) -> tuple[str, Optional[int], Optional[int]]:
    """Splits `int @ -2048..2047` into its type and its bounds."""
    if "@" not in text:
        return text.strip(), None, None
    name, _, bounds = text.partition("@")
    match = re.fullmatch(r"\s*(-?\d+)\.\.(-?\d+)\s*", bounds)
    if match is None:
        # `@ 0..` and `@ ..255` are legal mcdoc and simply have not turned up
        # here yet. Raising beats guessing which end was meant.
        raise McdocError(f"cannot read the range {bounds.strip()!r}")
    return name.strip(), int(match.group(1)), int(match.group(2))


class SettingsSchemaBuilder(SchemaBuilder):
    """Reads plain named structs, where SchemaBuilder reads dispatch unions."""

    def _map_settings_type(self, text: str, where: str) -> SettingsField:
        text = _unwrap(text)

        alternatives = _split_union(text)
        if len(alternatives) > 1:
            live = []
            for alternative in alternatives:
                gate, body = _strip_gate(alternative)
                if gate.applies_to(self.version) and body:
                    live.append(body)
            if len(live) != 1:
                raise McdocError(
                    f"{where}: expected exactly one alternative at this version, got {live!r}")
            return self._map_settings_type(live[0], where)

        name, minimum, maximum = _split_range(text)
        if name == "DensityFunctionRef":
            return SettingsField("", "Function")
        if name in LEAF_KINDS:
            return SettingsField("", LEAF_KINDS[name], minimum=minimum, maximum=maximum)
        raise McdocError(f"{where}: no mapping for type {name!r}")

    def build_struct(self, name: str) -> list[SettingsField]:
        """The fields of a named struct at this version, in declaration order."""
        struct = self.document.structs.get(name)
        if struct is None:
            raise McdocError(f"no struct named {name!r}; has the schema moved?")
        return self._flatten_settings(struct, name, {name})

    def _flatten_settings(self, struct: Struct, where: str,
                          seen: set[str]) -> list[SettingsField]:
        fields: list[SettingsField] = []
        for member in struct.members:
            if not member.gate.applies_to(self.version):
                continue

            if isinstance(member, MapEntry):
                # A struct keyed by arbitrary strings is not a field list and
                # cannot be validated as one. Nothing at the pinned version
                # reaches this; if something starts to, it must be handled
                # rather than flattened away.
                raise McdocError(
                    f"{where}: a map-keyed member ([{member.key_type_text}]) is not something "
                    "this generator can turn into a field list")

            if isinstance(member, Spread):
                if member.target is None:
                    fields.extend(
                        self._flatten_settings(Struct(None, member.inline_fields), where, seen))
                    continue
                if member.target in seen:
                    raise McdocError(f"{where}: spread cycle through {member.target!r}")
                target = self.document.structs.get(member.target)
                if target is None:
                    raise McdocError(f"{where}: spread of unknown struct {member.target!r}")
                fields.extend(
                    self._flatten_settings(target, where, seen | {member.target}))
                continue

            assert isinstance(member, Field)
            mapped = self._map_settings_type(member.type_text, f"{where}.{member.name}")
            mapped.name = member.name
            mapped.optional = member.optional
            fields.append(mapped)
        return fields
