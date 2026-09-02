"""Emits the generated C++ for Stratum's density-function schema."""

from __future__ import annotations

from schema import ResolvedType

_KIND_TO_CPP = {
    "Function": "FieldKind::Function",
    "Number": "FieldKind::Number",
    "NoiseRef": "FieldKind::NoiseRef",
    "Selector": "FieldKind::Selector",
    "Spline": "FieldKind::Spline",
}


def _enumerator(key: str) -> str:
    """`old_blended_noise` -> `OldBlendedNoise`."""
    return "".join(part.capitalize() for part in key.split("_"))


def _header(command: str) -> str:
    return (
        "// GENERATED FILE — DO NOT EDIT BY HAND.\n"
        "//\n"
        "// Derived from the mcdoc schema (SpyglassMC/vanilla-mcdoc, MIT) for the\n"
        "// pinned Minecraft version, as SPEC section 11 requires: the schema is\n"
        "// authoritative about which density functions exist and what fields they\n"
        "// take, and hand-copying it is how a field name drifts.\n"
        "//\n"
        f"// Regenerate with: {command}\n"
    )


def emit_node_types(types: list[ResolvedType], command: str) -> str:
    lines = [_header(command), ""]
    for resolved in types:
        lines.append(f"{_enumerator(resolved.key)},  // minecraft:{resolved.key}")
    return "\n".join(lines) + "\n"


def emit_schema(types: list[ResolvedType], command: str, version: str, commit: str) -> str:
    """Emits named arrays plus a table of spans over them.

    Field lists vary in length, so each type gets its own array and the table
    holds spans; a fixed-width table would need padding and a count, which is
    a second thing to get wrong.
    """
    lines = [
        _header(command),
        f"// Minecraft version: {version}",
        f"// vanilla-mcdoc commit: {commit}",
        "",
        "// clang-format off",
        "",
    ]

    for resolved in types:
        enumerator = _enumerator(resolved.key)
        for schema_field in resolved.fields:
            if schema_field.selector_values:
                values = ", ".join(f'"{value}"' for value in schema_field.selector_values)
                lines.append(
                    "constexpr std::array<std::string_view, %d> kSelector%s%s = {%s};"
                    % (len(schema_field.selector_values), enumerator,
                       _enumerator(schema_field.name), values))
        if not resolved.fields:
            continue
        lines.append("constexpr std::array<SchemaField, %d> kFields%s = {{"
                     % (len(resolved.fields), enumerator))
        for schema_field in resolved.fields:
            selector = ("kSelector%s%s" % (enumerator, _enumerator(schema_field.name))
                        if schema_field.selector_values else "{}")
            lines.append(
                '    {.name = "%s", .kind = %s, .optional = %s, .allowsReference = %s, '
                ".selectorValues = %s},"
                % (schema_field.name, _KIND_TO_CPP[schema_field.kind],
                   "true" if schema_field.optional else "false",
                   "true" if schema_field.allows_reference else "false", selector))
        lines.append("}};")

    lines.append("")
    lines.append("constexpr std::array<TypeInfo, %d> kTypeTable = {{" % len(types))
    for resolved in types:
        enumerator = _enumerator(resolved.key)
        fields = "kFields%s" % enumerator if resolved.fields else "{}"
        lines.append('    {.type = NodeType::%s, .name = "minecraft:%s", .fields = %s},'
                     % (enumerator, resolved.key, fields))
    lines.append("}};")
    lines.append("")
    lines.append("// clang-format on")
    return "\n".join(lines) + "\n"
