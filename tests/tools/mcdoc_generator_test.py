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
from emit import emit_schema  # noqa: E402
from schema import SchemaBuilder  # noqa: E402

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


if __name__ == "__main__":
    unittest.main(verbosity=2)
