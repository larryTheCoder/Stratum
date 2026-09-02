// GENERATED FILE — DO NOT EDIT BY HAND.
//
// Derived from the mcdoc schema (SpyglassMC/vanilla-mcdoc, MIT) for the
// pinned Minecraft version, as SPEC section 11 requires: the schema is
// authoritative about which density functions exist and what fields they
// take, and hand-copying it is how a field name drifts.
//
// Regenerate with: tools/mcdoc-sync


// Minecraft version: 1.21.11
// vanilla-mcdoc commit: c373ab678a7bf70f26c06fec3e7b470886dfb2f7

// clang-format off

constexpr std::array<SettingsField, 11> kSettingsFields = {{
    {.name = "default_block", .kind = SettingsKind::BlockState, .optional = false},
    {.name = "default_fluid", .kind = SettingsKind::BlockState, .optional = false},
    {.name = "sea_level", .kind = SettingsKind::Int, .optional = false},
    {.name = "disable_mob_generation", .kind = SettingsKind::Boolean, .optional = false},
    {.name = "aquifers_enabled", .kind = SettingsKind::Boolean, .optional = false},
    {.name = "ore_veins_enabled", .kind = SettingsKind::Boolean, .optional = false},
    {.name = "legacy_random_source", .kind = SettingsKind::Boolean, .optional = false},
    {.name = "noise", .kind = SettingsKind::Geometry, .optional = false},
    {.name = "noise_router", .kind = SettingsKind::Router, .optional = false},
    {.name = "spawn_target", .kind = SettingsKind::SpawnTarget, .optional = false},
    {.name = "surface_rule", .kind = SettingsKind::MaterialRule, .optional = false},
}};

constexpr std::array<GeometryField, 4> kGeometryFields = {{
    {.name = "min_y", .minimum = -2048, .maximum = 2047},
    {.name = "height", .minimum = 0, .maximum = 4096},
    {.name = "size_horizontal", .minimum = 1, .maximum = 4},
    {.name = "size_vertical", .minimum = 1, .maximum = 4},
}};

constexpr std::array<std::string_view, 15> kRouterFields = {{
    "barrier",
    "fluid_level_floodedness",
    "fluid_level_spread",
    "lava",
    "vein_toggle",
    "vein_ridged",
    "vein_gap",
    "temperature",
    "vegetation",
    "continents",
    "erosion",
    "depth",
    "ridges",
    "preliminary_surface_level",
    "final_density",
}};

// clang-format on
