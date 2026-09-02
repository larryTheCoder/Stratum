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

constexpr std::array<SchemaField, 1> kFieldsAbs = {{
    {.name = "argument", .kind = FieldKind::Function, .optional = false, .allowsReference = true, .selectorValues = {}},
}};
constexpr std::array<SchemaField, 2> kFieldsAdd = {{
    {.name = "argument1", .kind = FieldKind::Function, .optional = false, .allowsReference = true, .selectorValues = {}},
    {.name = "argument2", .kind = FieldKind::Function, .optional = false, .allowsReference = true, .selectorValues = {}},
}};
constexpr std::array<SchemaField, 1> kFieldsBlendDensity = {{
    {.name = "argument", .kind = FieldKind::Function, .optional = false, .allowsReference = true, .selectorValues = {}},
}};
constexpr std::array<SchemaField, 1> kFieldsCache2d = {{
    {.name = "argument", .kind = FieldKind::Function, .optional = false, .allowsReference = true, .selectorValues = {}},
}};
constexpr std::array<SchemaField, 1> kFieldsCacheAllInCell = {{
    {.name = "argument", .kind = FieldKind::Function, .optional = false, .allowsReference = true, .selectorValues = {}},
}};
constexpr std::array<SchemaField, 1> kFieldsCacheOnce = {{
    {.name = "argument", .kind = FieldKind::Function, .optional = false, .allowsReference = true, .selectorValues = {}},
}};
constexpr std::array<SchemaField, 3> kFieldsClamp = {{
    {.name = "input", .kind = FieldKind::Function, .optional = false, .allowsReference = false, .selectorValues = {}},
    {.name = "min", .kind = FieldKind::Number, .optional = false, .allowsReference = false, .selectorValues = {}},
    {.name = "max", .kind = FieldKind::Number, .optional = false, .allowsReference = false, .selectorValues = {}},
}};
constexpr std::array<SchemaField, 1> kFieldsConstant = {{
    {.name = "argument", .kind = FieldKind::Number, .optional = false, .allowsReference = false, .selectorValues = {}},
}};
constexpr std::array<SchemaField, 1> kFieldsCube = {{
    {.name = "argument", .kind = FieldKind::Function, .optional = false, .allowsReference = true, .selectorValues = {}},
}};
constexpr std::array<SchemaField, 4> kFieldsFindTopSurface = {{
    {.name = "density", .kind = FieldKind::Function, .optional = false, .allowsReference = true, .selectorValues = {}},
    {.name = "upper_bound", .kind = FieldKind::Function, .optional = false, .allowsReference = true, .selectorValues = {}},
    {.name = "lower_bound", .kind = FieldKind::Number, .optional = false, .allowsReference = false, .selectorValues = {}},
    {.name = "cell_height", .kind = FieldKind::Number, .optional = false, .allowsReference = false, .selectorValues = {}},
}};
constexpr std::array<SchemaField, 1> kFieldsFlatCache = {{
    {.name = "argument", .kind = FieldKind::Function, .optional = false, .allowsReference = true, .selectorValues = {}},
}};
constexpr std::array<SchemaField, 1> kFieldsHalfNegative = {{
    {.name = "argument", .kind = FieldKind::Function, .optional = false, .allowsReference = true, .selectorValues = {}},
}};
constexpr std::array<SchemaField, 1> kFieldsInterpolated = {{
    {.name = "argument", .kind = FieldKind::Function, .optional = false, .allowsReference = true, .selectorValues = {}},
}};
constexpr std::array<SchemaField, 1> kFieldsInvert = {{
    {.name = "argument", .kind = FieldKind::Function, .optional = false, .allowsReference = true, .selectorValues = {}},
}};
constexpr std::array<SchemaField, 2> kFieldsMax = {{
    {.name = "argument1", .kind = FieldKind::Function, .optional = false, .allowsReference = true, .selectorValues = {}},
    {.name = "argument2", .kind = FieldKind::Function, .optional = false, .allowsReference = true, .selectorValues = {}},
}};
constexpr std::array<SchemaField, 2> kFieldsMin = {{
    {.name = "argument1", .kind = FieldKind::Function, .optional = false, .allowsReference = true, .selectorValues = {}},
    {.name = "argument2", .kind = FieldKind::Function, .optional = false, .allowsReference = true, .selectorValues = {}},
}};
constexpr std::array<SchemaField, 2> kFieldsMul = {{
    {.name = "argument1", .kind = FieldKind::Function, .optional = false, .allowsReference = true, .selectorValues = {}},
    {.name = "argument2", .kind = FieldKind::Function, .optional = false, .allowsReference = true, .selectorValues = {}},
}};
constexpr std::array<SchemaField, 3> kFieldsNoise = {{
    {.name = "noise", .kind = FieldKind::NoiseRef, .optional = false, .allowsReference = false, .selectorValues = {}},
    {.name = "xz_scale", .kind = FieldKind::Number, .optional = false, .allowsReference = false, .selectorValues = {}},
    {.name = "y_scale", .kind = FieldKind::Number, .optional = false, .allowsReference = false, .selectorValues = {}},
}};
constexpr std::array<SchemaField, 5> kFieldsOldBlendedNoise = {{
    {.name = "xz_scale", .kind = FieldKind::Number, .optional = false, .allowsReference = false, .selectorValues = {}},
    {.name = "y_scale", .kind = FieldKind::Number, .optional = false, .allowsReference = false, .selectorValues = {}},
    {.name = "xz_factor", .kind = FieldKind::Number, .optional = false, .allowsReference = false, .selectorValues = {}},
    {.name = "y_factor", .kind = FieldKind::Number, .optional = false, .allowsReference = false, .selectorValues = {}},
    {.name = "smear_scale_multiplier", .kind = FieldKind::Number, .optional = false, .allowsReference = false, .selectorValues = {}},
}};
constexpr std::array<SchemaField, 1> kFieldsQuarterNegative = {{
    {.name = "argument", .kind = FieldKind::Function, .optional = false, .allowsReference = true, .selectorValues = {}},
}};
constexpr std::array<SchemaField, 5> kFieldsRangeChoice = {{
    {.name = "input", .kind = FieldKind::Function, .optional = false, .allowsReference = true, .selectorValues = {}},
    {.name = "min_inclusive", .kind = FieldKind::Number, .optional = false, .allowsReference = false, .selectorValues = {}},
    {.name = "max_exclusive", .kind = FieldKind::Number, .optional = false, .allowsReference = false, .selectorValues = {}},
    {.name = "when_in_range", .kind = FieldKind::Function, .optional = false, .allowsReference = true, .selectorValues = {}},
    {.name = "when_out_of_range", .kind = FieldKind::Function, .optional = false, .allowsReference = true, .selectorValues = {}},
}};
constexpr std::array<SchemaField, 1> kFieldsShift = {{
    {.name = "argument", .kind = FieldKind::NoiseRef, .optional = false, .allowsReference = false, .selectorValues = {}},
}};
constexpr std::array<SchemaField, 1> kFieldsShiftA = {{
    {.name = "argument", .kind = FieldKind::NoiseRef, .optional = false, .allowsReference = false, .selectorValues = {}},
}};
constexpr std::array<SchemaField, 1> kFieldsShiftB = {{
    {.name = "argument", .kind = FieldKind::NoiseRef, .optional = false, .allowsReference = false, .selectorValues = {}},
}};
constexpr std::array<SchemaField, 6> kFieldsShiftedNoise = {{
    {.name = "noise", .kind = FieldKind::NoiseRef, .optional = false, .allowsReference = false, .selectorValues = {}},
    {.name = "xz_scale", .kind = FieldKind::Number, .optional = false, .allowsReference = false, .selectorValues = {}},
    {.name = "y_scale", .kind = FieldKind::Number, .optional = false, .allowsReference = false, .selectorValues = {}},
    {.name = "shift_x", .kind = FieldKind::Function, .optional = false, .allowsReference = true, .selectorValues = {}},
    {.name = "shift_y", .kind = FieldKind::Function, .optional = false, .allowsReference = true, .selectorValues = {}},
    {.name = "shift_z", .kind = FieldKind::Function, .optional = false, .allowsReference = true, .selectorValues = {}},
}};
constexpr std::array<SchemaField, 1> kFieldsSlide = {{
    {.name = "argument", .kind = FieldKind::Function, .optional = false, .allowsReference = true, .selectorValues = {}},
}};
constexpr std::array<SchemaField, 1> kFieldsSpline = {{
    {.name = "spline", .kind = FieldKind::Spline, .optional = false, .allowsReference = false, .selectorValues = {}},
}};
constexpr std::array<SchemaField, 1> kFieldsSquare = {{
    {.name = "argument", .kind = FieldKind::Function, .optional = false, .allowsReference = true, .selectorValues = {}},
}};
constexpr std::array<SchemaField, 1> kFieldsSqueeze = {{
    {.name = "argument", .kind = FieldKind::Function, .optional = false, .allowsReference = true, .selectorValues = {}},
}};
constexpr std::array<std::string_view, 2> kSelectorWeirdScaledSamplerRarityValueMapper = {"type_1", "type_2"};
constexpr std::array<SchemaField, 3> kFieldsWeirdScaledSampler = {{
    {.name = "rarity_value_mapper", .kind = FieldKind::Selector, .optional = false, .allowsReference = false, .selectorValues = kSelectorWeirdScaledSamplerRarityValueMapper},
    {.name = "noise", .kind = FieldKind::NoiseRef, .optional = false, .allowsReference = false, .selectorValues = {}},
    {.name = "input", .kind = FieldKind::Function, .optional = false, .allowsReference = true, .selectorValues = {}},
}};
constexpr std::array<SchemaField, 4> kFieldsYClampedGradient = {{
    {.name = "from_y", .kind = FieldKind::Number, .optional = false, .allowsReference = false, .selectorValues = {}},
    {.name = "to_y", .kind = FieldKind::Number, .optional = false, .allowsReference = false, .selectorValues = {}},
    {.name = "from_value", .kind = FieldKind::Number, .optional = false, .allowsReference = false, .selectorValues = {}},
    {.name = "to_value", .kind = FieldKind::Number, .optional = false, .allowsReference = false, .selectorValues = {}},
}};

constexpr std::array<TypeInfo, 34> kTypeTable = {{
    {.type = NodeType::Abs, .name = "minecraft:abs", .fields = kFieldsAbs},
    {.type = NodeType::Add, .name = "minecraft:add", .fields = kFieldsAdd},
    {.type = NodeType::BlendAlpha, .name = "minecraft:blend_alpha", .fields = {}},
    {.type = NodeType::BlendDensity, .name = "minecraft:blend_density", .fields = kFieldsBlendDensity},
    {.type = NodeType::BlendOffset, .name = "minecraft:blend_offset", .fields = {}},
    {.type = NodeType::Cache2d, .name = "minecraft:cache_2d", .fields = kFieldsCache2d},
    {.type = NodeType::CacheAllInCell, .name = "minecraft:cache_all_in_cell", .fields = kFieldsCacheAllInCell},
    {.type = NodeType::CacheOnce, .name = "minecraft:cache_once", .fields = kFieldsCacheOnce},
    {.type = NodeType::Clamp, .name = "minecraft:clamp", .fields = kFieldsClamp},
    {.type = NodeType::Constant, .name = "minecraft:constant", .fields = kFieldsConstant},
    {.type = NodeType::Cube, .name = "minecraft:cube", .fields = kFieldsCube},
    {.type = NodeType::EndIslands, .name = "minecraft:end_islands", .fields = {}},
    {.type = NodeType::FindTopSurface, .name = "minecraft:find_top_surface", .fields = kFieldsFindTopSurface},
    {.type = NodeType::FlatCache, .name = "minecraft:flat_cache", .fields = kFieldsFlatCache},
    {.type = NodeType::HalfNegative, .name = "minecraft:half_negative", .fields = kFieldsHalfNegative},
    {.type = NodeType::Interpolated, .name = "minecraft:interpolated", .fields = kFieldsInterpolated},
    {.type = NodeType::Invert, .name = "minecraft:invert", .fields = kFieldsInvert},
    {.type = NodeType::Max, .name = "minecraft:max", .fields = kFieldsMax},
    {.type = NodeType::Min, .name = "minecraft:min", .fields = kFieldsMin},
    {.type = NodeType::Mul, .name = "minecraft:mul", .fields = kFieldsMul},
    {.type = NodeType::Noise, .name = "minecraft:noise", .fields = kFieldsNoise},
    {.type = NodeType::OldBlendedNoise, .name = "minecraft:old_blended_noise", .fields = kFieldsOldBlendedNoise},
    {.type = NodeType::QuarterNegative, .name = "minecraft:quarter_negative", .fields = kFieldsQuarterNegative},
    {.type = NodeType::RangeChoice, .name = "minecraft:range_choice", .fields = kFieldsRangeChoice},
    {.type = NodeType::Shift, .name = "minecraft:shift", .fields = kFieldsShift},
    {.type = NodeType::ShiftA, .name = "minecraft:shift_a", .fields = kFieldsShiftA},
    {.type = NodeType::ShiftB, .name = "minecraft:shift_b", .fields = kFieldsShiftB},
    {.type = NodeType::ShiftedNoise, .name = "minecraft:shifted_noise", .fields = kFieldsShiftedNoise},
    {.type = NodeType::Slide, .name = "minecraft:slide", .fields = kFieldsSlide},
    {.type = NodeType::Spline, .name = "minecraft:spline", .fields = kFieldsSpline},
    {.type = NodeType::Square, .name = "minecraft:square", .fields = kFieldsSquare},
    {.type = NodeType::Squeeze, .name = "minecraft:squeeze", .fields = kFieldsSqueeze},
    {.type = NodeType::WeirdScaledSampler, .name = "minecraft:weird_scaled_sampler", .fields = kFieldsWeirdScaledSampler},
    {.type = NodeType::YClampedGradient, .name = "minecraft:y_clamped_gradient", .fields = kFieldsYClampedGradient},
}};

// clang-format on
