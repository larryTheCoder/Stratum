"""Emits the generated C++ for Stratum's density-function schema."""

from __future__ import annotations

from schema import ResolvedType

_KIND_TO_CPP = {
    "Function": "FieldKind::Function",
    "Number": "FieldKind::Number",
    "Noise": "FieldKind::Noise",
    "Selector": "FieldKind::Selector",
    "Spline": "FieldKind::Spline",
}


def _doc_comments(docs, indent: str) -> list[str]:
    """mcdoc's `///` lines, as C++ comments.

    Carried into the generated file rather than dropped because they are
    where mcdoc puts semantics a type expression cannot hold -- a default
    value, or the formula behind an enum value -- and a reader of the
    generated table has nowhere else to find them.
    """
    return [f"{indent}// {line}" if line else f"{indent}//" for line in docs]


def _enumerator(key: str) -> str:
    """`old_blended_noise` -> `OldBlendedNoise`."""
    return "".join(part.capitalize() for part in key.split("_"))


def _header(command: str, subject: str = "density functions") -> str:
    return (
        "// GENERATED FILE — DO NOT EDIT BY HAND.\n"
        "//\n"
        "// Derived from the mcdoc schema (SpyglassMC/vanilla-mcdoc, MIT) for the\n"
        "// pinned Minecraft version, as SPEC section 11 requires: the schema is\n"
        f"// authoritative about which {subject} exist and what fields they\n"
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
                for value, doc in zip(schema_field.selector_values, schema_field.selector_docs):
                    if doc:
                        lines.extend(_doc_comments([f'"{value}": {doc}'], ""))
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
            lines.extend(_doc_comments(schema_field.docs, "    "))
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


_SURFACE_KIND_TO_CPP = {
    "Rule": "FieldKind::Rule",
    "Condition": "FieldKind::Condition",
    "BlockState": "FieldKind::BlockState",
    "Anchor": "FieldKind::Anchor",
    "Selector": "FieldKind::Selector",
    "Id": "FieldKind::Id",
    "String": "FieldKind::String",
    "Int": "FieldKind::Int",
    "Number": "FieldKind::Number",
    "Boolean": "FieldKind::Boolean",
    "List": "FieldKind::List",
}


def emit_surface_types(types: list[ResolvedType], command: str, subject: str) -> str:
    """One dispatch family's types as enumerators, in the schema's own order."""
    lines = [_header(command, subject), ""]
    for resolved in types:
        lines.append(f"{_enumerator(resolved.key)},  // minecraft:{resolved.key}")
    return "\n".join(lines) + "\n"


def _surface_fields(resolved: ResolvedType, family: str) -> list[str]:
    """The named arrays one surface-rule type needs, and its field array."""
    lines: list[str] = []
    enumerator = _enumerator(resolved.key)
    for schema_field in resolved.fields:
        if not schema_field.selector_values:
            continue
        for value, doc in zip(schema_field.selector_values, schema_field.selector_docs):
            if doc:
                lines.extend(_doc_comments([f'"{value}": {doc}'], ""))
        values = ", ".join(f'"{value}"' for value in schema_field.selector_values)
        lines.append(
            "constexpr std::array<std::string_view, %d> k%sValues%s%s = {%s};"
            % (len(schema_field.selector_values), family, enumerator,
               _enumerator(schema_field.name), values))

    if not resolved.fields:
        return lines

    lines.append("constexpr std::array<SchemaField, %d> k%sFields%s = {{"
                 % (len(resolved.fields), family, enumerator))
    for schema_field in resolved.fields:
        values = ("k%sValues%s%s" % (family, enumerator, _enumerator(schema_field.name))
                  if schema_field.selector_values else "{}")
        # .elementKind is written only where it means something. A list is
        # the one kind whose entry is incomplete without it, and giving every
        # other field a default element kind would be inviting a reader to
        # believe one.
        element = ("" if not schema_field.element_kind
                   else ".elementKind = %s, " % _SURFACE_KIND_TO_CPP[schema_field.element_kind])
        lines.extend(_doc_comments(schema_field.docs, "    "))
        lines.append(
            '    {.name = "%s", .kind = %s, .optional = %s, .allowsReference = %s, '
            "%s.values = %s},"
            % (schema_field.name, _SURFACE_KIND_TO_CPP[schema_field.kind],
               "true" if schema_field.optional else "false",
               "true" if schema_field.allows_reference else "false", element, values))
    lines.append("}};")
    return lines


def emit_surface_schema(rules: list[ResolvedType], conditions: list[ResolvedType], command: str,
                        version: str, commit: str) -> str:
    """Both surface-rule dispatch families, as field arrays and two tables."""
    lines = [
        _header(command, "surface rules and conditions"),
        f"// Minecraft version: {version}",
        f"// vanilla-mcdoc commit: {commit}",
        "//",
        "// The types with no fields below are the five absent from mcdoc — see",
        "// tools/mcdoc/surface.py. They are in the tables so that every type the",
        "// engine knows has a row, and they carry no fields because they have none.",
        "",
        "// clang-format off",
        "",
    ]

    for family, types, enum_name in (("Rule", rules, "RuleType"),
                                     ("Condition", conditions, "ConditionType")):
        for resolved in types:
            lines.extend(_surface_fields(resolved, family))
        lines.append("")
        lines.append("constexpr std::array<TypeInfo<%s>, %d> k%sTable = {{"
                     % (enum_name, len(types), family))
        for resolved in types:
            enumerator = _enumerator(resolved.key)
            fields = "k%sFields%s" % (family, enumerator) if resolved.fields else "{}"
            lines.append('    {.type = %s::%s, .name = "minecraft:%s", .fields = %s},'
                         % (enum_name, enumerator, resolved.key, fields))
        lines.append("}};")
        lines.append("")

    lines.append("// clang-format on")
    return "\n".join(lines) + "\n"


_SETTINGS_KIND_TO_CPP = {
    "BlockState": "SettingsKind::BlockState",
    "Boolean": "SettingsKind::Boolean",
    "Int": "SettingsKind::Int",
    "Geometry": "SettingsKind::Geometry",
    "Router": "SettingsKind::Router",
    "MaterialRule": "SettingsKind::MaterialRule",
    "SpawnTarget": "SettingsKind::SpawnTarget",
}


def emit_router_entries(router: list, command: str) -> str:
    """The router's entries as enumerators, in the schema's own order.

    Generated rather than written out because the names move: at 1.21.9
    `initial_density_without_jaggedness` became `preliminary_surface_level`.
    An enumerator makes that a compile error at every use site rather than a
    lookup that quietly finds nothing.
    """
    lines = [_header(command), ""]
    for field in router:
        lines.append(f"{_enumerator(field.name)},  // {field.name}")
    return "\n".join(lines) + "\n"


def emit_settings_schema(settings: list, geometry: list, router: list, command: str,
                         version: str, commit: str) -> str:
    lines = [
        _header(command),
        "",
        f"// Minecraft version: {version}",
        f"// vanilla-mcdoc commit: {commit}",
        "",
        "// clang-format off",
        "",
    ]

    lines.append(f"constexpr std::array<SettingsField, {len(settings)}> kSettingsFields = {{{{")
    for field in settings:
        kind = _SETTINGS_KIND_TO_CPP[field.kind]
        lines.append(
            f'    {{.name = "{field.name}", .kind = {kind}, '
            f".optional = {'true' if field.optional else 'false'}}},")
    lines.append("}};")
    lines.append("")

    lines.append(f"constexpr std::array<GeometryField, {len(geometry)}> kGeometryFields = {{{{")
    for field in geometry:
        if field.minimum is None or field.maximum is None:
            raise SystemExit(f"error: {field.name} has no range; the loader needs one")
        lines.append(
            f'    {{.name = "{field.name}", .minimum = {field.minimum}, '
            f".maximum = {field.maximum}}},")
    lines.append("}};")
    lines.append("")

    lines.append(f"constexpr std::array<std::string_view, {len(router)}> kRouterFields = {{{{")
    for field in router:
        lines.append(f'    "{field.name}",')
    lines.append("}};")
    lines.append("")
    lines.append("// clang-format on")
    return "\n".join(lines) + "\n"
