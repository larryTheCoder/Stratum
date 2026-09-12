// GENERATED FILE — DO NOT EDIT BY HAND.
//
// Derived from the mcdoc schema (SpyglassMC/vanilla-mcdoc, MIT) for the
// pinned Minecraft version, as SPEC section 11 requires: the schema is
// authoritative about which surface rules and conditions exist and what fields they
// take, and hand-copying it is how a field name drifts.
//
// Regenerate with: tools/mcdoc-sync

// Minecraft version: 1.21.11
// vanilla-mcdoc commit: c373ab678a7bf70f26c06fec3e7b470886dfb2f7
//
// The types with no fields below are the five absent from mcdoc — see
// tools/mcdoc/surface.py. They are in the tables so that every type the
// engine knows has a row, and they carry no fields because they have none.

// clang-format off

constexpr std::array<SchemaField, 1> kRuleFieldsBlock = {{
    {.name = "result_state", .kind = FieldKind::BlockState, .optional = false, .allowsReference = false, .values = {}},
}};
constexpr std::array<SchemaField, 2> kRuleFieldsCondition = {{
    {.name = "if_true", .kind = FieldKind::Condition, .optional = false, .allowsReference = false, .values = {}},
    {.name = "then_run", .kind = FieldKind::Rule, .optional = false, .allowsReference = false, .values = {}},
}};
constexpr std::array<SchemaField, 1> kRuleFieldsSequence = {{
    {.name = "sequence", .kind = FieldKind::List, .optional = false, .allowsReference = false, .elementKind = FieldKind::Rule, .values = {}},
}};

constexpr std::array<TypeInfo<RuleType>, 4> kRuleTable = {{
    {.type = RuleType::Bandlands, .name = "minecraft:bandlands", .fields = {}},
    {.type = RuleType::Block, .name = "minecraft:block", .fields = kRuleFieldsBlock},
    {.type = RuleType::Condition, .name = "minecraft:condition", .fields = kRuleFieldsCondition},
    {.type = RuleType::Sequence, .name = "minecraft:sequence", .fields = kRuleFieldsSequence},
}};

constexpr std::array<SchemaField, 1> kConditionFieldsBiome = {{
    {.name = "biome_is", .kind = FieldKind::List, .optional = false, .allowsReference = false, .elementKind = FieldKind::Id, .values = {}},
}};
constexpr std::array<SchemaField, 3> kConditionFieldsNoiseThreshold = {{
    {.name = "noise", .kind = FieldKind::Id, .optional = false, .allowsReference = false, .values = {}},
    {.name = "min_threshold", .kind = FieldKind::Number, .optional = false, .allowsReference = false, .values = {}},
    {.name = "max_threshold", .kind = FieldKind::Number, .optional = false, .allowsReference = false, .values = {}},
}};
constexpr std::array<SchemaField, 1> kConditionFieldsNot = {{
    {.name = "invert", .kind = FieldKind::Condition, .optional = false, .allowsReference = false, .values = {}},
}};
constexpr std::array<std::string_view, 2> kConditionValuesStoneDepthSurfaceType = {"floor", "ceiling"};
constexpr std::array<SchemaField, 4> kConditionFieldsStoneDepth = {{
    {.name = "offset", .kind = FieldKind::Int, .optional = false, .allowsReference = false, .values = {}},
    {.name = "surface_type", .kind = FieldKind::Selector, .optional = false, .allowsReference = false, .values = kConditionValuesStoneDepthSurfaceType},
    {.name = "add_surface_depth", .kind = FieldKind::Boolean, .optional = false, .allowsReference = false, .values = {}},
    {.name = "secondary_depth_range", .kind = FieldKind::Int, .optional = false, .allowsReference = false, .values = {}},
}};
constexpr std::array<std::string_view, 3> kConditionValuesVerticalGradientTrueAtAndBelow = {"absolute", "above_bottom", "below_top"};
constexpr std::array<std::string_view, 3> kConditionValuesVerticalGradientFalseAtAndAbove = {"absolute", "above_bottom", "below_top"};
constexpr std::array<SchemaField, 3> kConditionFieldsVerticalGradient = {{
    {.name = "random_name", .kind = FieldKind::String, .optional = false, .allowsReference = false, .values = {}},
    {.name = "true_at_and_below", .kind = FieldKind::Anchor, .optional = false, .allowsReference = false, .values = kConditionValuesVerticalGradientTrueAtAndBelow},
    {.name = "false_at_and_above", .kind = FieldKind::Anchor, .optional = false, .allowsReference = false, .values = kConditionValuesVerticalGradientFalseAtAndAbove},
}};
constexpr std::array<SchemaField, 3> kConditionFieldsWater = {{
    {.name = "offset", .kind = FieldKind::Int, .optional = false, .allowsReference = false, .values = {}},
    {.name = "surface_depth_multiplier", .kind = FieldKind::Int, .optional = false, .allowsReference = false, .values = {}},
    {.name = "add_stone_depth", .kind = FieldKind::Boolean, .optional = false, .allowsReference = false, .values = {}},
}};
constexpr std::array<std::string_view, 3> kConditionValuesYAboveAnchor = {"absolute", "above_bottom", "below_top"};
constexpr std::array<SchemaField, 3> kConditionFieldsYAbove = {{
    {.name = "anchor", .kind = FieldKind::Anchor, .optional = false, .allowsReference = false, .values = kConditionValuesYAboveAnchor},
    {.name = "surface_depth_multiplier", .kind = FieldKind::Int, .optional = false, .allowsReference = false, .values = {}},
    {.name = "add_stone_depth", .kind = FieldKind::Boolean, .optional = false, .allowsReference = false, .values = {}},
}};

constexpr std::array<TypeInfo<ConditionType>, 11> kConditionTable = {{
    {.type = ConditionType::AbovePreliminarySurface, .name = "minecraft:above_preliminary_surface", .fields = {}},
    {.type = ConditionType::Biome, .name = "minecraft:biome", .fields = kConditionFieldsBiome},
    {.type = ConditionType::Hole, .name = "minecraft:hole", .fields = {}},
    {.type = ConditionType::NoiseThreshold, .name = "minecraft:noise_threshold", .fields = kConditionFieldsNoiseThreshold},
    {.type = ConditionType::Not, .name = "minecraft:not", .fields = kConditionFieldsNot},
    {.type = ConditionType::Steep, .name = "minecraft:steep", .fields = {}},
    {.type = ConditionType::StoneDepth, .name = "minecraft:stone_depth", .fields = kConditionFieldsStoneDepth},
    {.type = ConditionType::Temperature, .name = "minecraft:temperature", .fields = {}},
    {.type = ConditionType::VerticalGradient, .name = "minecraft:vertical_gradient", .fields = kConditionFieldsVerticalGradient},
    {.type = ConditionType::Water, .name = "minecraft:water", .fields = kConditionFieldsWater},
    {.type = ConditionType::YAbove, .name = "minecraft:y_above", .fields = kConditionFieldsYAbove},
}};

// clang-format on
