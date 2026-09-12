#!/usr/bin/env python3
"""Stratum — what the mcdoc reader and the schema generator must not lose.

Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0

The generated tables are checked for *reproducibility* by CI, which catches
a hand-edit but not a generator that has quietly narrowed the schema: a table
regenerated from the same wrong reader diffs clean. This is the other half.
Everything below is written against schema text held here rather than against
the pinned upstream file, so it is hermetic and stays meaningful when a
version gate turns something in that file off.

The two losses it exists to prevent, both of which happened:

  * a union collapsed to one of its arms. `NoiseParametersRef` is
    `#[id="worldgen/noise"] string | NoiseParameters`, and matching the alias
    by name recorded only the identifier, so a datapack that inlines its
    noise parameters was refused for no reason;
  * a `///` doc comment skipped as trivia. mcdoc puts semantics a type
    expression cannot hold in doc comments — the per-value formulas of
    DistanceMetric are only there — so a reader that dropped them made those
    unreachable from the generated tables.
"""

from __future__ import annotations

import pathlib
import sys
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[2] / "tools" / "mcdoc"))

import mcdoc  # noqa: E402
from emit import emit_schema, emit_surface_schema  # noqa: E402
from schema import SchemaBuilder  # noqa: E402
from surface import SurfaceSchemaBuilder  # noqa: E402

VERSION = "1.21.11"

# The declarations under test, copied from upstream shape rather than from
# upstream text: what matters is the shape, and a test that fetched the real
# file would need the network to say anything at all.
NOISE_REF = '''
type NoiseParametersRef = (#[id="worldgen/noise"] string | NoiseParameters)

dispatch minecraft:density_function[noise] to struct Noise {
	noise: NoiseParametersRef,
	xz_scale: float,
	y_scale: float,
}

dispatch minecraft:density_function[shift, shift_a] to struct Shift {
	argument: NoiseParametersRef,
}
'''


def build(text: str, version: str = VERSION):
    document = mcdoc.parse(text, "<test>")
    return document, SchemaBuilder(document, version)


def fields_of(types, key: str):
    for resolved in types:
        if resolved.key == key:
            return {schema_field.name: schema_field for schema_field in resolved.fields}
    raise AssertionError(f"no type {key!r} in {[one.key for one in types]}")


class NoiseUnionTest(unittest.TestCase):
    def test_both_spellings_are_recorded(self) -> None:
        _, builder = build(NOISE_REF)
        types = builder.build("density_function")

        for key, field_name in (("noise", "noise"), ("shift", "argument"),
                                ("shift_a", "argument")):
            with self.subTest(node=key):
                field = fields_of(types, key)[field_name]
                self.assertEqual(field.kind, "Noise")
                # The whole point: an identifier is one legal spelling, not
                # the only one.
                self.assertTrue(field.allows_reference)

    def test_a_bare_noise_is_inline_only(self) -> None:
        # No union, so no identifier: a field declared as the struct itself
        # must not be widened into accepting a name.
        _, builder = build('''
dispatch minecraft:density_function[noise] to struct Noise {
	noise: NoiseParameters,
}
''')
        field = fields_of(builder.build("density_function"), "noise")["noise"]
        self.assertEqual(field.kind, "Noise")
        self.assertFalse(field.allows_reference)

    def test_the_generated_table_says_so(self) -> None:
        _, builder = build(NOISE_REF)
        emitted = emit_schema(builder.build("density_function"), "cmd", VERSION, "deadbeef")
        self.assertIn(
            '{.name = "noise", .kind = FieldKind::Noise, .optional = false, '
            ".allowsReference = true", emitted)

    def test_an_unmappable_union_still_raises(self) -> None:
        # The widening must not have turned into a reader that shrugs: a
        # union it cannot account for is the case this whole generator exists
        # to refuse.
        _, builder = build('''
dispatch minecraft:density_function[noise] to struct Noise {
	noise: (float | NoiseParameters | CubicSpline),
}
''')
        with self.assertRaises(mcdoc.McdocError):
            builder.build("density_function")


class DocCommentTest(unittest.TestCase):
    ENUM = '''
enum(string) DistanceMetric {
	/// `sqrt(dx^2 + dy^2 + dz^2)`
	Euclidean = "euclidean",
	// A note to whoever edits the schema, not documentation.
	Manhattan = "manhattan",
}

dispatch minecraft:density_function[distance_to_point] to struct DistanceToPoint {
	metric: DistanceMetric,
	/// Defaults to constant 1.
	multiple?: DensityFunctionRef,
}

type DensityFunctionRef = (#[id="worldgen/density_function"] string | DensityFunction)

type DensityFunction = (float | CubicSpline)
'''

    def test_enum_values_keep_their_formulas(self) -> None:
        document, _ = build(self.ENUM)
        members = document.enums["DistanceMetric"].members
        self.assertEqual([member.value for member in members], ["euclidean", "manhattan"])
        self.assertEqual(members[0].docs, ["`sqrt(dx^2 + dy^2 + dz^2)`"])
        # A plain `//` comment is addressed to a schema author and is not
        # documentation; keeping it would put editorial notes in the tables.
        self.assertEqual(members[1].docs, [])

    def test_a_fields_docs_reach_the_resolved_field(self) -> None:
        _, builder = build(self.ENUM)
        types = builder.build("density_function")
        fields = fields_of(types, "distance_to_point")
        self.assertEqual(fields["multiple"].docs, ("Defaults to constant 1.",))
        self.assertEqual(fields["metric"].docs, ())
        self.assertEqual(fields["metric"].selector_docs,
                         ("`sqrt(dx^2 + dy^2 + dz^2)`", ""))

    def test_the_generated_table_carries_them(self) -> None:
        _, builder = build(self.ENUM)
        emitted = emit_schema(builder.build("density_function"), "cmd", VERSION, "deadbeef")
        self.assertIn('// "euclidean": `sqrt(dx^2 + dy^2 + dz^2)`', emitted)
        self.assertIn("    // Defaults to constant 1.", emitted)

    def test_docs_do_not_drift_onto_the_next_member(self) -> None:
        # A `///` written above a closing brace documents nothing. Left
        # pending it would be claimed by whatever was read next, which is how
        # a formula ends up attached to the wrong value.
        document, _ = build('''
struct First {
	a: float,
	/// Stranded.
}

struct Second {
	b: float,
}
''')
        self.assertEqual(document.structs["Second"].members[0].docs, [])


class EnumGateTest(unittest.TestCase):
    SCHEMA = '''
enum(string) RarityType {
	Type1 = "type_1",
	#[since="26.3"] Type3 = "type_3",
	#[until="1.19"] Old = "old",
}

dispatch minecraft:density_function[weird_scaled_sampler] to struct WeirdScaledSampler {
	rarity_value_mapper: RarityType,
}
'''

    def test_a_value_from_another_version_is_not_offered(self) -> None:
        _, builder = build(self.SCHEMA)
        field = fields_of(builder.build("density_function"),
                          "weird_scaled_sampler")["rarity_value_mapper"]
        # `type_3` does not exist yet and `old` is gone: a table listing
        # either would accept a string the pinned server refuses.
        self.assertEqual(field.selector_values, ("type_1",))

    def test_a_version_with_no_values_left_raises(self) -> None:
        # An enum every one of whose values belongs to another version is not
        # a selector with no values, it is a schema this reader has
        # misunderstood -- so it raises rather than emitting an empty table
        # that would refuse every string.
        _, builder = build('''
enum(string) LaterOnly {
	#[since="26.3"] Only = "only",
}

dispatch minecraft:density_function[weird_scaled_sampler] to struct WeirdScaledSampler {
	rarity_value_mapper: LaterOnly,
}
''')
        with self.assertRaises(mcdoc.McdocError):
            builder.build("density_function")


# -- surface rules ---------------------------------------------------------

# The shapes upstream writes, copied from their form rather than their text so
# that these stay meaningful when a version gate turns one of them off. Every
# construct here is one the reader could not handle before the surface tables
# existed: a `use` line, a dispatch spread computed from a sibling field, a
# type declared in another file, a generic alias, an array field, and a union
# of one-field structs.
SURFACE_MOD = '''
use ::java::util::NonEmptyWeightedList

enum(string) CaveSurface {
	Floor = "floor",
	Ceiling = "ceiling",
}

type UniformInt<Base, Spread> = (
	Base |
	struct {
		base: Base,
		spread: Spread,
	} |
)

dispatch minecraft:int_provider[constant]<T> to (
	#[until="1.20.5"] struct {
		value: T
	} |
	T |
)

type VerticalAnchor = (
	struct {
		absolute: int,
	} |
	struct {
		above_bottom: int,
	} |
	struct {
		below_top: int,
	} |
	#[since="26.3"] struct {
		relative_to_sea_level: int,
	} |
)
'''

SURFACE_CONDITION = '''
use super::CaveSurface
use super::VerticalAnchor

struct MaterialCondition {
	type: (
		#[until="26.3"] #[id="worldgen/material_condition"] string |
		#[since="26.3"] #[id="worldgen/material_condition_type"] string |
	),
	...minecraft:material_condition[[type]],
}
type MaterialConditionRef = (
	#[since="26.3"] #[id="material_condition"] string |
	MaterialCondition |
)

dispatch minecraft:material_condition[biome] to struct BiomeCondition {
	biome_is: (
		[#[id="worldgen/biome"] string] |
		#[since="26.2"] #[id="worldgen/biome"] string |
	),
}

dispatch minecraft:material_condition[noise_threshold] to struct NoiseThresholdCondition {
	noise: #[id="worldgen/noise"] string,
	min_threshold: float,
	max_threshold: float,
	#[since="26.2"]
	is_3d?: boolean,
}

dispatch minecraft:material_condition[not] to struct NotCondition {
	invert: MaterialConditionRef,
}

dispatch minecraft:material_condition[stone_depth] to struct StoneDepthCondition {
	offset: int,
	surface_type: CaveSurface,
	add_surface_depth: boolean,
	#[until="1.18.2"]
	add_surface_secondary_depth: boolean,
	#[since="1.18.2"]
	secondary_depth_range: int,
}

dispatch minecraft:material_condition[vertical_gradient] to struct VerticalGradientCondition {
	random_name: string,
	true_at_and_below: VerticalAnchor,
	false_at_and_above: VerticalAnchor,
}
'''

SURFACE_RULE = '''
use ::java::util::block_state::BlockState
use super::material_condition::MaterialConditionRef

struct MaterialRule {
	type: (
		#[until="26.3"] #[id="worldgen/material_rule"] string |
		#[since="26.3"] #[id="worldgen/material_rule_type"] string |
	),
	...minecraft:material_rule[[type]],
}
type MaterialRuleRef = (
	#[since="26.3"] #[id="material_rule"] string |
	MaterialRule |
)

dispatch minecraft:material_rule[%unknown] to struct {}

dispatch minecraft:material_rule[block] to struct BlockRule {
	result_state: BlockState,
}

dispatch minecraft:material_rule[condition] to struct ConditionRule {
	if_true: MaterialConditionRef,
	then_run: MaterialRuleRef,
}

#[since="26.3"]
dispatch minecraft:material_rule[ore_vein] to struct OreVeinifier {
	ore_block: BlockState,
	raw_ore_chance: float @ 0..1,
}

dispatch minecraft:material_rule[sequence] to struct SequenceRule {
	sequence: [MaterialRuleRef],
}
'''


def build_surface(version: str = VERSION, sources=None):
    document = mcdoc.Document()
    for index, text in enumerate(sources or (SURFACE_MOD, SURFACE_CONDITION, SURFACE_RULE)):
        document.merge(mcdoc.parse(text, f"<test {index}>"), f"<test {index}>")
    return document, SurfaceSchemaBuilder(document, version)


class SurfaceParsingTest(unittest.TestCase):
    """The three constructs that stopped the reader at the surface-rule files."""

    def test_a_dispatch_spread_parses(self) -> None:
        # `...minecraft:material_condition[[type]]` says "the rest of this
        # struct is whatever `type` selected". The reader wanted a bare
        # identifier here and stopped on the colon, which is why neither
        # surface-rule file could be read at all.
        document = mcdoc.parse(SURFACE_CONDITION, "<test>")
        spread = document.structs["MaterialCondition"].members[1]
        self.assertEqual(spread.target, "minecraft:material_condition[[type]]")
        # Kept as text, not resolved: no single struct is named by it, and
        # the concrete types come from the dispatches instead.
        self.assertNotIn(spread.target, document.structs)

    def test_use_lines_are_skipped(self) -> None:
        document = mcdoc.parse(SURFACE_RULE, "<test>")
        self.assertIn("BlockRule", document.structs)

    def test_a_generic_alias_is_not_resolvable_by_bare_name(self) -> None:
        # Its body mentions its own parameters, so expanding it for a caller
        # who looked it up by bare name would hand back a type mentioning
        # `Base`. It is recorded, and apart.
        document = mcdoc.parse(SURFACE_MOD, "<test>")
        self.assertIn("UniformInt", document.generic_aliases)
        self.assertNotIn("UniformInt", document.aliases)

    def test_merging_refuses_a_name_declared_twice(self) -> None:
        # Two files disagreeing about what a name means is exactly the case
        # where last-one-wins emits a table describing neither.
        with self.assertRaises(mcdoc.McdocError):
            build_surface(sources=(SURFACE_MOD, SURFACE_MOD))


class SurfaceSchemaTest(unittest.TestCase):
    def test_every_field_name_and_kind(self) -> None:
        # The whole point of deriving this table: a field name or a field
        # kind that drifted would be read out of the JSON as the wrong thing,
        # or not read at all.
        _, builder = build_surface()
        expected = {
            "biome": {"biome_is": "List"},
            "noise_threshold": {"noise": "Id", "min_threshold": "Number",
                                "max_threshold": "Number"},
            "not": {"invert": "Condition"},
            "stone_depth": {"offset": "Int", "surface_type": "Selector",
                            "add_surface_depth": "Boolean", "secondary_depth_range": "Int"},
            "vertical_gradient": {"random_name": "String", "true_at_and_below": "Anchor",
                                  "false_at_and_above": "Anchor"},
        }
        types = builder.build_conditions()
        for key, fields in expected.items():
            with self.subTest(condition=key):
                self.assertEqual({name: one.kind
                                  for name, one in fields_of(types, key).items()}, fields)

        rules = builder.build_rules()
        self.assertEqual({name: one.kind for name, one in fields_of(rules, "block").items()},
                         {"result_state": "BlockState"})
        self.assertEqual({name: one.kind for name, one in fields_of(rules, "condition").items()},
                         {"if_true": "Condition", "then_run": "Rule"})
        self.assertEqual({name: one.kind for name, one in fields_of(rules, "sequence").items()},
                         {"sequence": "List"})

    def test_an_int_is_not_a_float(self) -> None:
        # `offset` is read out of the JSON as a whole number and
        # `min_threshold` as a double. Collapsing the two kinds the way the
        # density table does would make the schema stop saying which.
        _, builder = build_surface()
        conditions = builder.build_conditions()
        self.assertEqual(fields_of(conditions, "stone_depth")["offset"].kind, "Int")
        self.assertEqual(fields_of(conditions, "noise_threshold")["min_threshold"].kind, "Number")

    def test_an_identifier_is_not_a_bare_string(self) -> None:
        # `#[id="worldgen/noise"] string` names a registry entry and is
        # parsed as a resource location; `random_name` is a bare string used
        # as an RNG salt. Only the attribute tells them apart.
        _, builder = build_surface()
        conditions = builder.build_conditions()
        self.assertEqual(fields_of(conditions, "noise_threshold")["noise"].kind, "Id")
        self.assertEqual(fields_of(conditions, "vertical_gradient")["random_name"].kind, "String")

    def test_lists_carry_their_element(self) -> None:
        _, builder = build_surface()
        biome_is = fields_of(builder.build_conditions(), "biome")["biome_is"]
        self.assertEqual((biome_is.kind, biome_is.element_kind), ("List", "Id"))
        sequence = fields_of(builder.build_rules(), "sequence")["sequence"]
        self.assertEqual((sequence.kind, sequence.element_kind), ("List", "Rule"))

    def test_the_anchor_union_is_expanded_not_named(self) -> None:
        # mcdoc spells a vertical anchor as a union of one-field structs, so
        # the spellings the loader accepts are read out of it. Matching the
        # alias by name instead is the narrowing SPEC section 11 records
        # having cost a legal input once already.
        _, builder = build_surface()
        field = fields_of(builder.build_conditions(), "vertical_gradient")["true_at_and_below"]
        self.assertEqual(field.kind, "Anchor")
        self.assertEqual(field.selector_values, ("absolute", "above_bottom", "below_top"))

    def test_a_later_versions_anchor_spelling_is_not_offered(self) -> None:
        # `relative_to_sea_level` arrives at 26.3. A table listing it now
        # would accept an anchor the pinned server refuses.
        _, builder = build_surface(version="26.3")
        field = fields_of(builder.build_conditions(), "vertical_gradient")["true_at_and_below"]
        self.assertIn("relative_to_sea_level", field.selector_values)

    def test_a_nested_rule_is_inline_only_at_this_version(self) -> None:
        # MaterialRuleRef gains its identifier spelling at 26.3 and does not
        # have one now, and that falls out of the union rather than being
        # decided here.
        _, builder = build_surface()
        self.assertFalse(fields_of(builder.build_rules(), "condition")["then_run"].allows_reference)
        _, later = build_surface(version="26.3")
        self.assertTrue(fields_of(later.build_rules(), "condition")["then_run"].allows_reference)

    def test_fields_and_types_from_other_versions_are_absent(self) -> None:
        _, builder = build_surface()
        # `is_3d` arrives at 26.2 and `add_surface_secondary_depth` left at
        # 1.18.2; `ore_vein` is a whole type that arrives at 26.3.
        conditions = builder.build_conditions()
        self.assertNotIn("is_3d", fields_of(conditions, "noise_threshold"))
        self.assertNotIn("add_surface_secondary_depth", fields_of(conditions, "stone_depth"))
        self.assertNotIn("ore_vein", [one.key for one in builder.build_rules()])

    def test_the_types_absent_from_mcdoc_are_listed_with_no_fields(self) -> None:
        # They have to be in the tables — the engine knows them and they are
        # 5 of the 15 — and mcdoc says nothing about them, so what is written
        # by hand is the name and nothing else.
        _, builder = build_surface()
        self.assertEqual(fields_of(builder.build_rules(), "bandlands"), {})
        for key in ("above_preliminary_surface", "hole", "steep", "temperature"):
            with self.subTest(condition=key):
                self.assertEqual(fields_of(builder.build_conditions(), key), {})

    def test_the_generated_table_says_so(self) -> None:
        _, builder = build_surface()
        emitted = emit_surface_schema(builder.build_rules(), builder.build_conditions(),
                                      "cmd", VERSION, "deadbeef")
        self.assertIn('{.name = "sequence", .kind = FieldKind::List, .optional = false, '
                      ".allowsReference = false, .elementKind = FieldKind::Rule", emitted)
        self.assertIn('kConditionValuesStoneDepthSurfaceType = {"floor", "ceiling"}', emitted)
        # Every type the engine knows has a row, fields or not.
        self.assertIn('{.type = RuleType::Bandlands, .name = "minecraft:bandlands", '
                      ".fields = {}}", emitted)


if __name__ == "__main__":
    unittest.main(verbosity=2)
