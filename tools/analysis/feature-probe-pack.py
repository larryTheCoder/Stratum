#!/usr/bin/env python3
"""Stratum — the datapack of one feature probe (tools/analysis/feature-probe.sh).
Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0

  feature-probe-pack.py <variant> <vanilla worldgen dir> <overrides dir>

Writes, in the worldgen tree's own layout, the redefinitions a variant makes:
its placed features (`minecraft:stratum_*`, or shipped ones under their own
names), and every vanilla biome with its `features` replaced by the variant's
lists (carvers and everything else untouched). The probe copies the directory into its datapack
and keeps it beside the regions, so a test builds the same pack.

A variant is every biome's list of generation steps (or one per biome, or
none to leave the biomes alone), each a list of placed features; a
MARKER is a placed feature whose configured feature is `minecraft:simple_block`
writing one block state no terrain holds, behind the placement modifiers under
test. Where the marker lands is what the modifiers drew.

Nothing this writes is committed: the biomes are Mojang-derived (SPEC §12).
"""
import json
import pathlib
import sys

WOOL = ["white", "orange", "magenta", "light_blue", "yellow", "lime", "pink", "gray",
        "light_gray", "cyan", "purple", "blue", "brown", "green", "red", "black"]


def marker(block, placement):
    """A placed feature writing @p block behind @p placement."""
    return {"feature": {"type": "minecraft:simple_block",
                        "config": {"to_place": {"type": "minecraft:simple_state_provider",
                                                "state": {"Name": block}}}},
            "placement": placement}


def at_y(y):
    return {"type": "minecraft:height_range", "height": {"absolute": y}}


IN_SQUARE = {"type": "minecraft:in_square"}


def variant_seed():
    """The decoration seed: one marker a step behind `in_square` alone, two in
    the first step, each at its own height in its own colour."""
    steps, placed, index = [], {}, 0
    for step in range(11):
        names = []
        for _ in range(2 if step == 0 else 1):
            name = f"seed_{index}"
            placed[f"stratum_{name}"] = marker(f"minecraft:{WOOL[index]}_wool", [IN_SQUARE, at_y(200 + index)])
            names.append(f"minecraft:stratum_{name}")
            index += 1
        steps.append(names)
    return steps, placed


CONCRETE = [f"minecraft:{colour}_concrete" for colour in WOOL]


def uniform(low, high):
    return {"type": "minecraft:uniform", "min_inclusive": low, "max_inclusive": high}


def count(provider):
    return {"type": "minecraft:count", "count": provider}


def height(provider):
    return {"type": "minecraft:height_range", "height": provider}


def absolute(y):
    return {"absolute": y}


def weighted_wool():
    """A feature whose state provider draws: one of sixteen wools, weights 1..16."""
    return {"type": "minecraft:simple_block",
            "config": {"to_place": {"type": "minecraft:weighted_state_provider", "entries": [
                {"weight": weight + 1, "data": {"Name": f"minecraft:{colour}_wool"}}
                for weight, colour in enumerate(WOOL)]}}}


def variant_draws():
    """What the modifiers and providers draw, and in what order. One step;
    a PLANE marker is alone on its y, so its block is free to carry a draw;
    a HEIGHT marker is told by its block, its y being what is measured."""
    planes = [
        [count(3), IN_SQUARE],
        [count(uniform(0, 5)), IN_SQUARE],
        [{"type": "minecraft:rarity_filter", "chance": 3}, IN_SQUARE],
        [IN_SQUARE, {"type": "minecraft:random_offset", "xz_spread": uniform(-3, 3), "y_spread": 0}],
        [count({"type": "minecraft:biased_to_bottom", "min_inclusive": 0, "max_inclusive": 7}),
         IN_SQUARE],
        [count({"type": "minecraft:clamped", "source": uniform(0, 10),
                "min_inclusive": 2, "max_inclusive": 6}), IN_SQUARE],
        [count({"type": "minecraft:clamped_normal", "mean": 4.0, "deviation": 2.0,
                "min_inclusive": 0, "max_inclusive": 8}), IN_SQUARE],
        [count({"type": "minecraft:weighted_list", "distribution": [
            {"data": 1, "weight": 3}, {"data": 4, "weight": 1}, {"data": 0, "weight": 2}]}),
         IN_SQUARE],
        [count(3), IN_SQUARE],  # plane 8: with the drawing state provider, below
        [IN_SQUARE, count(3),
         {"type": "minecraft:random_offset", "xz_spread": uniform(-2, 2), "y_spread": 0}],
        [count(2), {"type": "minecraft:rarity_filter", "chance": 2}, IN_SQUARE],
    ]
    steps, placed = [[]], {}
    for index, placement in enumerate(planes):
        name = f"plane_{index}"
        placed[f"stratum_{name}"] = marker("minecraft:white_wool",
                                           placement + [at_y(260 + 2 * index)])
        if index == 8:
            placed[f"stratum_{name}"]["feature"] = weighted_wool()
        steps[0].append(f"minecraft:stratum_{name}")
    # y drawn by random_offset, about 300.
    placed["stratum_offset_y"] = marker("minecraft:white_wool", [
        IN_SQUARE, at_y(300),
        {"type": "minecraft:random_offset", "xz_spread": uniform(-3, 3), "y_spread": uniform(-2, 2)}])
    steps[0].append("minecraft:stratum_offset_y")
    heights = [
        uniform(absolute(-64), absolute(250)),
        {"type": "minecraft:trapezoid", "min_inclusive": absolute(0), "max_inclusive": absolute(200)},
        {"type": "minecraft:trapezoid", "min_inclusive": absolute(0), "max_inclusive": absolute(200),
         "plateau": 60},
        {"type": "minecraft:biased_to_bottom", "min_inclusive": absolute(0),
         "max_inclusive": absolute(200)},
        {"type": "minecraft:very_biased_to_bottom", "min_inclusive": absolute(0),
         "max_inclusive": absolute(200)},
        uniform({"above_bottom": 10}, {"below_top": 70}),
        {"type": "minecraft:biased_to_bottom", "min_inclusive": absolute(0),
         "max_inclusive": absolute(200), "inner": 5},
        {"type": "minecraft:very_biased_to_bottom", "min_inclusive": absolute(0),
         "max_inclusive": absolute(200), "inner": 5},
        {"type": "minecraft:weighted_list", "distribution": [
            {"data": absolute(10), "weight": 1},
            {"data": uniform(absolute(100), absolute(110)), "weight": 2}]},
        {"type": "minecraft:trapezoid", "min_inclusive": absolute(0), "max_inclusive": absolute(201)},
        {"type": "minecraft:trapezoid", "min_inclusive": absolute(0), "max_inclusive": absolute(201),
         "plateau": 61},
    ]
    for index, provider in enumerate(heights):
        name = f"height_{index}"
        placed[f"stratum_{name}"] = marker(CONCRETE[index], [IN_SQUARE, height(provider)])
        steps[0].append(f"minecraft:stratum_{name}")
    return steps, placed


TERRACOTTA = [f"minecraft:{colour}_terracotta" for colour in WOOL]
GLASS = [f"minecraft:{colour}_stained_glass" for colour in WOOL]


def heightmap(kind):
    return {"type": "minecraft:heightmap", "heightmap": kind}


def predicate(test):
    return {"type": "minecraft:block_predicate_filter", "predicate": test}


def blocks_are(names, offset=None):
    test = {"type": "minecraft:matching_blocks", "blocks": names}
    if offset is not None:
        test["offset"] = offset
    return test


def variant_terrain():
    """The modifiers that read the world: heightmaps, the two surface filters,
    block predicates, the scan, fixed positions. Every marker its own block."""
    below = [0, -1, 0]
    water = {"type": "minecraft:matching_fluids", "fluids": "minecraft:water"}
    air = blocks_are("minecraft:air")
    stone = blocks_are(["minecraft:stone", "minecraft:deepslate"])
    spread = {"type": "minecraft:clamped_normal", "mean": 0.0, "deviation": 4.0,
              "min_inclusive": -8, "max_inclusive": 8}
    chains = [
        [IN_SQUARE, heightmap("WORLD_SURFACE_WG")],
        [IN_SQUARE, heightmap("OCEAN_FLOOR_WG")],
        [IN_SQUARE, heightmap("MOTION_BLOCKING")],
        [IN_SQUARE, heightmap("MOTION_BLOCKING_NO_LEAVES")],
        [IN_SQUARE, heightmap("WORLD_SURFACE")],
        [IN_SQUARE, heightmap("OCEAN_FLOOR")],
        [count(4), IN_SQUARE, heightmap("OCEAN_FLOOR_WG"),
         {"type": "minecraft:surface_water_depth_filter", "max_water_depth": 0}],
        [count(4), IN_SQUARE, heightmap("OCEAN_FLOOR_WG"),
         {"type": "minecraft:surface_water_depth_filter", "max_water_depth": 3}],
        [count(8), IN_SQUARE, height(uniform(absolute(30), absolute(110))),
         {"type": "minecraft:surface_relative_threshold_filter", "heightmap": "OCEAN_FLOOR_WG",
          "min_inclusive": -10, "max_inclusive": -2}],
        [count(8), IN_SQUARE, height(uniform(absolute(30), absolute(110))),
         {"type": "minecraft:surface_relative_threshold_filter", "heightmap": "WORLD_SURFACE_WG",
          "max_inclusive": -20}],
        [count(4), IN_SQUARE, height(uniform(absolute(40), absolute(100))), predicate(stone)],
        [count(4), IN_SQUARE, heightmap("WORLD_SURFACE_WG"),
         predicate(blocks_are(["minecraft:grass_block", "minecraft:sand"], below))],
        [count(4), IN_SQUARE, heightmap("WORLD_SURFACE_WG"),
         predicate({"type": "minecraft:matching_block_tag", "tag": "minecraft:dirt",
                    "offset": below})],
        [count(4), IN_SQUARE, height(uniform(absolute(40), absolute(70))), predicate(water)],
        [count(4), IN_SQUARE, height(uniform(absolute(50), absolute(80))),
         predicate({"type": "minecraft:all_of", "predicates": [
             {"type": "minecraft:any_of", "predicates": [air, water]},
             {"type": "minecraft:not", "predicate": blocks_are("minecraft:air", below)}]})],
        [count(4), IN_SQUARE, height(uniform(absolute(295), absolute(319))),
         predicate({"type": "minecraft:inside_world_bounds", "offset": [0, 15, 0]})],
        [count(4), IN_SQUARE, height(uniform(absolute(0), absolute(120))),
         {"type": "minecraft:environment_scan", "direction_of_search": "down",
          "target_condition": stone, "max_steps": 12}],
        [count(4), IN_SQUARE, height(uniform(absolute(0), absolute(120))),
         {"type": "minecraft:environment_scan", "direction_of_search": "up",
          "target_condition": air, "allowed_search_condition": stone, "max_steps": 8}],
        [{"type": "minecraft:fixed_placement", "positions": [
            [130, 250, 130], [131, 250, 200], [-380, 250, -380], [-370, 251, -300],
            [1000, 250, 1000]]}],
        [IN_SQUARE, at_y(255),
         {"type": "minecraft:random_offset", "xz_spread": spread, "y_spread": 0}],
        [count(4), IN_SQUARE, height(uniform(absolute(0), absolute(120))),
         {"type": "minecraft:environment_scan", "direction_of_search": "down",
          "target_condition": stone, "allowed_search_condition": air, "max_steps": 32}],
    ]
    palette = CONCRETE + TERRACOTTA + GLASS
    steps, placed = [[]], {}
    for index, placement in enumerate(chains):
        name = f"terrain_{index}"
        placed[f"stratum_{name}"] = marker(palette[index], placement)
        steps[0].append(f"minecraft:stratum_{name}")
    return steps, placed


GLAZED = [f"minecraft:{colour}_glazed_terracotta" for colour in WOOL]
#: Eighty blocks no terrain holds, to tell markers apart by.
PALETTE = CONCRETE + TERRACOTTA + GLASS + GLAZED + [f"minecraft:{colour}_wool" for colour in WOOL]


def variant_order(worldgen):
    """The order features run in: every shipped placed feature, under its own
    name, becomes a marker behind `in_square` alone — the biomes untouched, so
    each step's features and their indices are the shipped world's. A marker
    is told by its block and its y together; where it lands gives its index,
    and the chunks it appears in give whose features a chunk runs."""
    placed = {}
    names = sorted(path.stem for path in (worldgen / "placed_feature").glob("*.json"))
    for index, name in enumerate(names):
        placed[name] = marker(
            PALETTE[index % len(PALETTE)], [IN_SQUARE, at_y(200 + 2 * (index // len(PALETTE)))])
    return None, placed


def variant_biomefilter(worldgen):
    """The `biome` modifier: each biome's one feature is a marker of its own
    block behind it, forty attempts a chunk over the terrain's height."""
    placed, lists = {}, {}
    names = sorted(path.stem for path in (worldgen / "biome").glob("*.json"))
    for index, name in enumerate(names):
        placed[f"stratum_filter_{name}"] = marker(PALETTE[index], [
            count(40), IN_SQUARE, height(uniform(absolute(-60), absolute(180))),
            {"type": "minecraft:biome"}])
        lists[name] = [[f"minecraft:stratum_filter_{name}"]]
    return lists, placed



def variant_misc():
    """Loose ends: the origin's y (a marker behind `in_square` alone), the two
    noise counts, `count_on_every_layer`, and a scan that leaves the world."""
    air = blocks_are("minecraft:air")
    chains = [
        [IN_SQUARE],
        [{"type": "minecraft:noise_threshold_count", "noise_level": 0.0, "below_noise": 1,
          "above_noise": 4}, IN_SQUARE, at_y(260)],
        [{"type": "minecraft:noise_based_count", "noise_to_count_ratio": 10,
          "noise_factor": 80.0, "noise_offset": 0.3}, IN_SQUARE, at_y(262)],
        [{"type": "minecraft:count_on_every_layer", "count": 1}],
        [count(4), IN_SQUARE, height(uniform(absolute(-64), absolute(-60))),
         {"type": "minecraft:environment_scan", "direction_of_search": "down",
          "target_condition": air, "max_steps": 32}],
        [{"type": "minecraft:noise_threshold_count", "noise_level": -0.8, "below_noise": 5,
          "above_noise": 10}, IN_SQUARE, at_y(264)],
        [{"type": "minecraft:noise_based_count", "noise_to_count_ratio": 160,
          "noise_factor": 80.0, "noise_offset": 0.3}, IN_SQUARE, at_y(266)],
        [{"type": "minecraft:noise_based_count", "noise_to_count_ratio": 20,
          "noise_factor": 13.5, "noise_offset": -0.2}, IN_SQUARE, at_y(268)],
    ]
    steps, placed = [[]], {}
    for index, placement in enumerate(chains):
        name = f"misc_{index}"
        placed[f"stratum_{name}"] = marker(CONCRETE[index], placement)
        steps[0].append(f"minecraft:stratum_{name}")
    return steps, placed


def ore(size, block, discard=0.0, target=None):
    return {"type": "minecraft:ore", "config": {
        "size": size, "discard_chance_on_air_exposure": discard,
        "targets": [{"target": target or {"predicate_type": "minecraft:always_true"},
                     "state": {"Name": block}}]}}


def variant_ore(size):
    """One ore of @p size, twice a chunk, replacing whatever is there: its
    shape, free of the terrain, and — by where the second lands — how far the
    first moved the feature's random stream."""
    def build():
        placed = {"stratum_ore": {
            "feature": ore(size, "minecraft:white_concrete"),
            "placement": [count(2), IN_SQUARE, height(uniform(absolute(-48), absolute(16)))]}}
        return [["minecraft:stratum_ore"]], placed
    return build


def variant_oresky():
    """What an ore attempt in open air does to the stream: two a chunk, each
    at y 250 or underground by an even draw, so an underground blob after a
    sky attempt shows how far the sky attempt moved the stream."""
    y = {"type": "minecraft:weighted_list", "distribution": [
        {"data": absolute(250), "weight": 1},
        {"data": uniform(absolute(-48), absolute(0)), "weight": 1}]}
    placed = {"stratum_ore": {"feature": ore(9, "minecraft:white_concrete"),
                              "placement": [count(2), IN_SQUARE, height(y)]}}
    return [["minecraft:stratum_ore"]], placed


def variant_oresurface():
    """Where an ore attempt near the surface is given up: six a chunk of size
    9 and six of size 33, between y 50 and 140, replacing whatever is there,
    so an attempt that runs leaves its whole blob, air and all."""
    placement = [count(6), IN_SQUARE, height(uniform(absolute(50), absolute(140)))]
    placed = {"stratum_small": {"feature": ore(9, "minecraft:white_concrete"),
                                "placement": placement},
              "stratum_large": {"feature": ore(33, "minecraft:white_concrete"),
                                "placement": placement}}
    return [["minecraft:stratum_small"], ["minecraft:stratum_large"]], placed


def variant_orediscard():
    """`discard_chance_on_air_exposure`, at 0.5 and at 1: sixteen ores of size
    9 a chunk each, over the terrain's whole height, replacing stone and
    deepslate alone — which blocks beside air are placed, and how the stream
    moves when one is not."""
    stone = {"predicate_type": "minecraft:tag_match", "tag": "minecraft:base_stone_overworld"}
    placement = [count(16), IN_SQUARE, height(uniform(absolute(-60), absolute(100)))]
    placed = {
        "stratum_half": {"feature": ore(9, "minecraft:white_concrete", 0.5, stone),
                         "placement": placement},
        "stratum_all": {"feature": ore(9, "minecraft:orange_concrete", 1.0, stone),
                        "placement": placement},
        "stratum_none": {"feature": ore(9, "minecraft:magenta_concrete", 0.0, stone),
                         "placement": placement},
    }
    return [["minecraft:stratum_half"], ["minecraft:stratum_all"], ["minecraft:stratum_none"]], placed


def variant_oreexposed():
    """Which exposed block takes which draw: a discard chance of 0.5, at sizes
    20 and 9, where the caves are. Thirty-two features of one ore each, so
    that an ore out of step with the server's — two chunks' blobs meeting in
    another order — puts no other out of step with it."""
    stone = {"predicate_type": "minecraft:tag_match", "tag": "minecraft:base_stone_overworld"}
    placement = [IN_SQUARE, height(uniform(absolute(-56), absolute(48)))]
    placed = {f"stratum_ore_{index}": {
        "feature": ore(20 if index % 2 == 0 else 9, "minecraft:white_concrete", 0.5, stone),
        "placement": placement} for index in range(32)}
    return [[f"minecraft:stratum_ore_{index}" for index in range(32)]], placed


#: The sizes the shipped ores use.
ORE_SIZES = (3, 4, 7, 8, 9, 10, 12, 14, 17, 20, 33, 64)


def variant_oresizes():
    """Every shipped size at once, two ores a chunk each, one block for all:
    replacing whatever is there and discarding nothing, an ore draws the same
    whatever it meets, so the blobs' union is the same in any order."""
    placement = [count(2), IN_SQUARE, height(uniform(absolute(-48), absolute(16)))]
    placed = {f"stratum_ore_{size}": {"feature": ore(size, "minecraft:white_concrete"),
                                      "placement": placement} for size in ORE_SIZES}
    return [[f"minecraft:stratum_ore_{size}" for size in ORE_SIZES]], placed

def only_types(worldgen, types):
    """Every biome's lists, keeping the shipped placed features whose
    configured feature is one of @p types (by reference; none is inline)."""
    kept = set()
    for path in (worldgen / "placed_feature").glob("*.json"):
        feature = json.loads(path.read_text())["feature"]
        if not isinstance(feature, str):
            continue
        configured = worldgen / "configured_feature" / (feature.split(":")[-1] + ".json")
        if json.loads(configured.read_text())["type"] in types:
            kept.add(f"minecraft:{path.stem}")
    lists = {}
    for path in (worldgen / "biome").glob("*.json"):
        lists[path.stem] = [[name for name in step if name in kept]
                            for step in json.loads(path.read_text())["features"]]
    return lists, {}


def variant_ores(worldgen):
    """The shipped ores, as shipped, and no other feature: every biome's
    lists cut down to its `minecraft:ore` features."""
    return only_types(worldgen, {"minecraft:ore"})


def variant_springs(worldgen):
    """The shipped springs alone: every biome's `minecraft:spring_feature`
    features and nothing else."""
    return only_types(worldgen, {"minecraft:spring_feature"})


def variant_disks(worldgen):
    """The shipped disks alone: every biome's `minecraft:disk` features."""
    return only_types(worldgen, {"minecraft:disk"})


VARIANTS = {**{f"ore{size}": variant_ore(size) for size in ORE_SIZES},
            "ores": variant_ores, "springs": variant_springs, "disks": variant_disks,
            "oresky": variant_oresky, "oresizes": variant_oresizes, "oreexposed": variant_oreexposed, "oresurface": variant_oresurface, "orediscard": variant_orediscard,
            "seed": variant_seed, "draws": variant_draws, "terrain": variant_terrain,
            "order": variant_order, "biomefilter": variant_biomefilter, "misc": variant_misc}


def main():
    if len(sys.argv) != 4 or sys.argv[1] not in VARIANTS:
        sys.exit(f"usage: {sys.argv[0]} <{'|'.join(VARIANTS)}> <worldgen dir> <overrides dir>")
    worldgen, out = pathlib.Path(sys.argv[2]), pathlib.Path(sys.argv[3])
    build = VARIANTS[sys.argv[1]]
    # A variant that reads the shipped data takes the worldgen directory.
    steps, placed = build(worldgen) if build.__code__.co_argcount else build()
    features = out / "placed_feature"
    features.mkdir(parents=True, exist_ok=True)
    for name, definition in placed.items():
        (features / f"{name}.json").write_text(json.dumps(definition, indent=1) + "\n")
    count_biomes = 0
    if steps is not None:
        biomes = out / "biome"
        biomes.mkdir(parents=True, exist_ok=True)
        for source in sorted((worldgen / "biome").glob("*.json")):
            biome = json.loads(source.read_text())
            biome["features"] = steps[source.stem] if isinstance(steps, dict) else steps
            (biomes / source.name).write_text(json.dumps(biome, indent=1) + "\n")
            count_biomes += 1
        if count_biomes == 0:
            sys.exit(f"no biomes under {worldgen}/biome")
    print(f"{count_biomes} biome(s) rewritten, {len(placed)} placed feature(s)")


if __name__ == "__main__":
    main()
