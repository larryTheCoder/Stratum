#!/usr/bin/env python3
"""Which NAMED noises does each legacy dimension actually reach?

Walks the pinned pack's noise_settings for every dimension, resolving
density-function *references* (a bare string in a density-function slot names
another entry under worldgen/density_function/) transitively, so an indirect
reach through a shared density function cannot hide from the count.

Reports, per noise_settings file:
  - per noise_router entry: every named noise reachable from it, with the path
  - the surface_rule's named noises, with the rule path each occurrence sits on

Reads only the pinned vanilla pack under .fixtures (Mojang-derived, never
committed). Output is a measurement, not a fixture.

Usage: python3 tools/analysis/legacy-named-noise-reach.py [worldgen-dir]
"""
import json
import os
import sys
from collections import OrderedDict

DEFAULT_WG = os.path.join(
    os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))),
    ".fixtures", "1.21.11", "worldgen")

WG = sys.argv[1] if len(sys.argv) > 1 else DEFAULT_WG

# A named noise can enter a density function through two different keys:
#   - "noise" on minecraft:noise / shifted_noise / weird_scaled_sampler
#   - "argument" on minecraft:shift / shift_a / shift_b, where the argument is a
#     NOISE resource location and not a density function.  This is the reach a
#     naive per-entry walk misses: shift_x/shift_z are shared density functions
#     whose argument is minecraft:offset.
NOISE_ARGUMENT_TYPES = {
    "minecraft:shift",
    "minecraft:shift_a",
    "minecraft:shift_b",
}

# String-valued keys that are enums, not references (minecraft:weird_scaled_sampler).
ENUM_KEYS = {"rarity_value_mapper"}

_df_cache = {}


def load_df(rid):
    key = rid[len("minecraft:"):] if rid.startswith("minecraft:") else rid
    if key in _df_cache:
        return _df_cache[key]
    path = os.path.join(WG, "density_function", key + ".json")
    if not os.path.exists(path):
        _df_cache[key] = None
        return None
    with open(path) as handle:
        value = json.load(handle)
    _df_cache[key] = value
    return value


def walk_density(node, out, visited, path):
    """Collect (named_noise_id, path) reachable from a density-function node."""
    if isinstance(node, str):
        if node in visited:
            return
        visited.add(node)
        sub = load_df(node)
        if sub is None:
            out.append(("<UNRESOLVED-DF:%s>" % node, path))
            return
        walk_density(sub, out, visited, path + " -> df:" + node)
        return
    if isinstance(node, (int, float, bool)) or node is None:
        return
    if isinstance(node, list):
        for index, element in enumerate(node):
            walk_density(element, out, visited, path + "[%d]" % index)
        return
    if isinstance(node, dict):
        node_type = node.get("type")
        for key, value in node.items():
            if key == "type" or key in ENUM_KEYS:
                continue
            if key == "noise" and isinstance(value, str):
                out.append((value, path + ".noise(type=%s)" % node_type))
                continue
            if (key == "argument" and isinstance(value, str)
                    and node_type in NOISE_ARGUMENT_TYPES):
                out.append((value, path + ".argument(type=%s)" % node_type))
                continue
            walk_density(value, out, visited, path + "." + key)


def walk_surface_rule(node, out, path):
    if isinstance(node, dict):
        node_type = node.get("type")
        for key, value in node.items():
            if key == "type":
                continue
            if key == "noise" and isinstance(value, str):
                out.append((value, path + ".noise(type=%s)" % node_type))
                continue
            walk_surface_rule(value, out, path + "." + key)
    elif isinstance(node, list):
        for index, element in enumerate(node):
            walk_surface_rule(element, out, path + "[%d]" % index)


def report(dim):
    path = os.path.join(WG, "noise_settings", dim + ".json")
    with open(path) as handle:
        settings = json.load(handle)
    legacy = settings.get("legacy_random_source", False)
    print("=" * 74)
    print("%s.json   legacy_random_source = %s" % (dim, legacy))

    router = settings.get("noise_router", {})
    router_total = 0
    router_names = OrderedDict()
    print("  ROUTER (%d entries)" % len(router))
    for entry in sorted(router):
        found = []
        walk_density(router[entry], found, set(), entry)
        router_total += len(found)
        uniq = OrderedDict()
        for name, where in found:
            uniq.setdefault(name, where)
            router_names.setdefault(name, entry)
        if found:
            print("    %-26s %d ref(s): %s" % (entry, len(found), ", ".join(uniq)))
            for name, where in uniq.items():
                print("        %-28s via %s" % (name, where))
        else:
            print("    %-26s 0 named" % entry)

    found = []
    if settings.get("surface_rule") is not None:
        walk_surface_rule(settings["surface_rule"], found, "surface_rule")
    grouped = OrderedDict()
    for name, where in found:
        grouped.setdefault(name, []).append(where)
    print("  SURFACE_RULE: %d named ref(s), %d distinct" % (len(found), len(grouped)))
    for name, wheres in grouped.items():
        print("    %-34s x%d" % (name, len(wheres)))
        for where in wheres:
            print("        %s" % where)
    print("  TOTAL named refs: router=%d surface_rule=%d" % (router_total, len(found)))
    return router_total, len(found)


def main():
    dims = sys.argv[2:] if len(sys.argv) > 2 else [
        "end", "nether", "caves", "floating_islands", "overworld"]
    for dim in dims:
        report(dim)
    return 0


if __name__ == "__main__":
    sys.exit(main())
