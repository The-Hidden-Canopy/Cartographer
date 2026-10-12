#include <carto/semantic/migration.hpp>

#include <algorithm>
#include <charconv>
#include <iomanip>
#include <limits>
#include <set>
#include <sstream>
#include <system_error>
#include <tuple>
#include <utility>

namespace carto::semantic {
namespace {

constexpr std::string_view kRuleMagic = "CARTOGRAPHER_MIGRATION_RULE";
constexpr std::string_view kPlanMagic = "CARTOGRAPHER_MIGRATION_PLAN";

core::Diagnostic invalid(std::string message) {
    return core::Diagnostic(core::ErrorCode::invalid_argument, std::move(message));
}

core::Diagnostic validation(std::string message) {
    return core::Diagnostic(core::ErrorCode::validation_failed, std::move(message));
}

core::Diagnostic parse_error(std::string message) {
    return core::Diagnostic(core::ErrorCode::validation_failed, std::move(message));
}

bool safe_text(std::string_view value, std::size_t maximum, bool allow_empty = false) {
    if ((!allow_empty && value.empty()) || value.size() > maximum) return false;
    return std::all_of(value.begin(), value.end(), [](unsigned char byte) {
        return byte >= 0x20U && byte != 0x7fU && byte != '\t' &&
            byte != '\r' && byte != '\n';
    });
}

bool read_uint(std::istream& input, std::uint64_t& value) {
    std::string token;
    if (!(input >> token)) return false;
    const auto parsed = std::from_chars(token.data(), token.data() + token.size(), value);
    return parsed.ec == std::errc{} && parsed.ptr == token.data() + token.size();
}

bool require_token(std::istream& input, std::string_view expected) {
    std::string token;
    return static_cast<bool>(input >> token) && token == expected;
}

bool read_count(std::istream& input, std::uint64_t maximum, std::size_t& value) {
    std::uint64_t encoded = 0U;
    if (!read_uint(input, encoded) || encoded > maximum ||
        encoded > std::numeric_limits<std::size_t>::max()) {
        return false;
    }
    value = static_cast<std::size_t>(encoded);
    return true;
}

} // namespace

core::Result<void> MigrationRule::validate() const {
    if (auto result = schema.validate(); !result) {
        return core::Result<void>::failure(result.error().with_context(
            "migration rule"));
    }
    if (from_version.value == 0U || to_version.value == 0U) {
        return core::Result<void>::failure(validation(
            "migration rule versions must be non-zero"));
    }
    if (from_version == to_version) {
        return core::Result<void>::failure(validation(
            "migration rule must move between two different versions"));
    }
    if (renames.size() > kMaxRenamesPerRule ||
        preserved_game_owned_fields.size() > kMaxPreservedFieldsPerRule) {
        return core::Result<void>::failure(validation(
            "migration rule exceeds its bounded capacity"));
    }
    for (const auto& rename : renames) {
        if (!safe_text(rename.from, SchemaField::kMaxFieldNameBytes) ||
            !safe_text(rename.to, SchemaField::kMaxFieldNameBytes) ||
            rename.from == rename.to) {
            return core::Result<void>::failure(invalid(
                "migration rule rename endpoint is invalid"));
        }
    }
    if (!std::is_sorted(renames.begin(), renames.end(),
            [](const FieldRename& left, const FieldRename& right) {
                return left.from < right.from;
            }) ||
        std::adjacent_find(renames.begin(), renames.end(),
            [](const FieldRename& left, const FieldRename& right) {
                return left.from == right.from;
            }) != renames.end()) {
        return core::Result<void>::failure(validation(
            "migration rule renames must be sorted and unique by source field"));
    }
    std::set<std::string> rename_targets;
    for (const auto& rename : renames) {
        if (!rename_targets.insert(rename.to).second) {
            return core::Result<void>::failure(validation(
                "migration rule renames two fields onto one target"));
        }
    }
    for (const auto& field : preserved_game_owned_fields) {
        if (!safe_text(field, SchemaField::kMaxFieldNameBytes)) {
            return core::Result<void>::failure(invalid(
                "migration rule preserved field name is invalid"));
        }
    }
    if (!std::is_sorted(preserved_game_owned_fields.begin(),
            preserved_game_owned_fields.end()) ||
        std::adjacent_find(preserved_game_owned_fields.begin(),
            preserved_game_owned_fields.end()) != preserved_game_owned_fields.end()) {
        return core::Result<void>::failure(validation(
            "migration rule preserved fields must be sorted and unique"));
    }
    return core::Result<void>::success();
}

std::string MigrationRule::serialize() const {
    std::ostringstream output;
    output << kRuleMagic << ' ' << kSchemaVersion << '\n';
    output << "SCHEMA " << std::quoted(schema.name_space) << ' '
           << std::quoted(schema.name) << '\n';
    output << "FROM " << from_version.value << '\n';
    output << "TO " << to_version.value << '\n';
    output << "RENAMES " << renames.size() << '\n';
    for (const auto& rename : renames) {
        output << "RENAME " << std::quoted(rename.from) << ' '
               << std::quoted(rename.to) << '\n';
    }
    output << "PRESERVED " << preserved_game_owned_fields.size() << '\n';
    for (const auto& field : preserved_game_owned_fields) {
        output << "PRESERVED_FIELD " << std::quoted(field) << '\n';
    }
    output << "END\n";
    return output.str();
}

core::Result<MigrationRule> MigrationRule::deserialize(std::string_view text) {
    if (text.empty() || text.size() > kMaxSerializedBytes) {
        return core::Result<MigrationRule>::failure(parse_error(
            "migration rule text is empty or exceeds its byte bound"));
    }
    std::istringstream input{std::string(text)};
    std::string magic;
    std::uint64_t version = 0U;
    if (!(input >> magic) || magic != kRuleMagic || !read_uint(input, version) ||
        version != kSchemaVersion) {
        return core::Result<MigrationRule>::failure(parse_error(
            "migration rule framing or schema version is invalid"));
    }
    MigrationRule rule;
    std::uint64_t from = 0U;
    std::uint64_t to = 0U;
    std::size_t rename_count = 0U;
    std::size_t preserved_count = 0U;
    if (!require_token(input, "SCHEMA") ||
        !(input >> std::quoted(rule.schema.name_space) >>
          std::quoted(rule.schema.name)) ||
        !require_token(input, "FROM") || !read_uint(input, from) ||
        !require_token(input, "TO") || !read_uint(input, to) ||
        !require_token(input, "RENAMES") ||
        !read_count(input, MigrationRule::kMaxRenamesPerRule, rename_count)) {
        return core::Result<MigrationRule>::failure(parse_error(
            "migration rule header is malformed"));
    }
    rule.from_version = SchemaVersion{static_cast<std::uint32_t>(from)};
    rule.to_version = SchemaVersion{static_cast<std::uint32_t>(to)};
    rule.renames.reserve(rename_count);
    for (std::size_t index = 0U; index < rename_count; ++index) {
        FieldRename rename;
        if (!require_token(input, "RENAME") ||
            !(input >> std::quoted(rename.from) >> std::quoted(rename.to))) {
            return core::Result<MigrationRule>::failure(parse_error(
                "migration rule rename is malformed"));
        }
        rule.renames.push_back(std::move(rename));
    }
    if (!require_token(input, "PRESERVED") ||
        !read_count(input, MigrationRule::kMaxPreservedFieldsPerRule, preserved_count)) {
        return core::Result<MigrationRule>::failure(parse_error(
            "migration rule preserved-field header is malformed"));
    }
    rule.preserved_game_owned_fields.reserve(preserved_count);
    for (std::size_t index = 0U; index < preserved_count; ++index) {
        std::string field;
        if (!require_token(input, "PRESERVED_FIELD") ||
            !(input >> std::quoted(field))) {
            return core::Result<MigrationRule>::failure(parse_error(
                "migration rule preserved field is malformed"));
        }
        rule.preserved_game_owned_fields.push_back(std::move(field));
    }
    if (!require_token(input, "END")) {
        return core::Result<MigrationRule>::failure(parse_error(
            "migration rule is missing its END record"));
    }
    std::string trailing;
    if (input >> trailing) {
        return core::Result<MigrationRule>::failure(parse_error(
            "migration rule contains trailing unbound data"));
    }
    if (auto result = rule.validate(); !result) {
        return core::Result<MigrationRule>::failure(result.error());
    }
    return core::Result<MigrationRule>::success(std::move(rule));
}

core::Result<MigrationPlan> MigrationPlan::compose(
    const SchemaRegistry& registry,
    const SchemaIdentity& schema,
    SchemaVersion from,
    SchemaVersion to,
    std::span<const MigrationRule> available_rules) {
    MigrationPlan plan;
    plan.schema_ = schema;
    plan.from_version_ = from;
    plan.to_version_ = to;
    if (auto result = schema.validate(); !result) {
        return core::Result<MigrationPlan>::failure(result.error());
    }
    if (from.value == 0U || to.value == 0U) {
        return core::Result<MigrationPlan>::failure(validation(
            "migration plan versions must be non-zero"));
    }
    if (from > to) {
        return core::Result<MigrationPlan>::failure(validation(
            "backwards migration across schema versions is unsupported"));
    }
    if (!registry.contains(schema, from)) {
        return core::Result<MigrationPlan>::failure(core::Diagnostic(
            core::ErrorCode::not_found,
            "migration plan start version is not registered"));
    }
    if (!registry.contains(schema, to)) {
        return core::Result<MigrationPlan>::failure(core::Diagnostic(
            core::ErrorCode::not_found,
            "migration plan target version is not registered"));
    }
    if (from == to) {
        return core::Result<MigrationPlan>::success(std::move(plan));
    }
    std::set<std::uint32_t> visited;
    SchemaVersion current = from;
    while (current != to) {
        if (!visited.insert(current.value).second) {
            return core::Result<MigrationPlan>::failure(core::Diagnostic(
                core::ErrorCode::cycle_detected,
                "migration rule chain revisits a schema version"));
        }
        if (plan.rules_.size() >= kMaxRulesPerPlan) {
            return core::Result<MigrationPlan>::failure(validation(
                "migration plan exceeds its bounded rule capacity"));
        }
        const MigrationRule* selected = nullptr;
        for (const auto& rule : available_rules) {
            if (!(rule.schema == schema) || rule.from_version != current) continue;
            if (rule.to_version > to) continue;
            if (selected == nullptr || rule.to_version < selected->to_version) {
                selected = &rule;
            }
        }
        if (selected == nullptr) {
            return core::Result<MigrationPlan>::failure(core::Diagnostic(
                core::ErrorCode::not_found,
                "no migration rule covers this schema version jump"));
        }
        if (auto result = selected->validate(); !result) {
            return core::Result<MigrationPlan>::failure(result.error().with_context(
                "migration plan rule"));
        }
        plan.rules_.push_back(*selected);
        current = selected->to_version;
    }
    if (auto result = plan.validate_against(registry); !result) {
        return core::Result<MigrationPlan>::failure(result.error());
    }
    return core::Result<MigrationPlan>::success(std::move(plan));
}

core::Result<void> MigrationPlan::validate_against(const SchemaRegistry& registry) const {
    if (auto result = schema_.validate(); !result) return result;
    if (from_version_.value == 0U || to_version_.value == 0U || from_version_ > to_version_) {
        return core::Result<void>::failure(validation(
            "migration plan version range is invalid"));
    }
    if (rules_.size() > kMaxRulesPerPlan) {
        return core::Result<void>::failure(validation(
            "migration plan exceeds its bounded rule capacity"));
    }
    if (!registry.contains(schema_, from_version_) ||
        !registry.contains(schema_, to_version_)) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::not_found,
            "migration plan references an unregistered schema version"));
    }
    SchemaVersion cursor = from_version_;
    for (const auto& rule : rules_) {
        if (!(rule.schema == schema_)) {
            return core::Result<void>::failure(validation(
                "migration plan mixes schemas across its rule chain"));
        }
        if (auto result = rule.validate(); !result) return result;
        if (rule.from_version != cursor) {
            return core::Result<void>::failure(validation(
                "migration plan rules do not form a contiguous version chain"));
        }
        const auto from_result = registry.find(schema_, rule.from_version);
        if (!from_result) {
            return core::Result<void>::failure(from_result.error().with_context(
                "migration plan validation"));
        }
        const SchemaRevision* from_revision = from_result.value();
        for (const auto& field : from_revision->fields) {
            if (field.ownership != FieldOwnership::game_owned) continue;
            const auto position = std::lower_bound(
                rule.preserved_game_owned_fields.begin(),
                rule.preserved_game_owned_fields.end(), field.name);
            if (position == rule.preserved_game_owned_fields.end() ||
                *position != field.name) {
                return core::Result<void>::failure(validation(
                    "migration rule does not preserve a game-owned field"));
            }
        }
        for (const auto& rename : rule.renames) {
            const auto position = std::lower_bound(
                from_revision->fields.begin(), from_revision->fields.end(),
                rename.from,
                [](const SchemaField& field, const std::string& name) {
                    return field.name < name;
                });
            if (position == from_revision->fields.end() ||
                position->name != rename.from) {
                return core::Result<void>::failure(validation(
                    "migration rule renames a field missing from its source revision"));
            }
            const auto target_position = std::lower_bound(
                from_revision->fields.begin(), from_revision->fields.end(),
                rename.to,
                [](const SchemaField& field, const std::string& name) {
                    return field.name < name;
                });
            if (target_position != from_revision->fields.end() &&
                target_position->name == rename.to) {
                return core::Result<void>::failure(validation(
                    "migration rule rename target already exists in its source revision"));
            }
        }
        cursor = rule.to_version;
    }
    if (cursor != to_version_) {
        return core::Result<void>::failure(validation(
            "migration plan does not reach its declared target version"));
    }
    return core::Result<void>::success();
}

std::string MigrationPlan::serialize() const {
    std::ostringstream output;
    output << kPlanMagic << ' ' << kSchemaVersion << '\n';
    output << "SCHEMA " << std::quoted(schema_.name_space) << ' '
           << std::quoted(schema_.name) << '\n';
    output << "FROM " << from_version_.value << '\n';
    output << "TO " << to_version_.value << '\n';
    output << "RULES " << rules_.size() << '\n';
    for (const auto& rule : rules_) {
        output << "RULE " << std::quoted(rule.serialize()) << '\n';
    }
    output << "END\n";
    return output.str();
}

core::Result<MigrationPlan> MigrationPlan::deserialize(std::string_view text) {
    if (text.empty() || text.size() > SemanticSidecar::kMaxSerializedBytes) {
        return core::Result<MigrationPlan>::failure(parse_error(
            "migration plan text is empty or exceeds its byte bound"));
    }
    std::istringstream input{std::string(text)};
    std::string magic;
    std::uint64_t version = 0U;
    if (!(input >> magic) || magic != kPlanMagic || !read_uint(input, version) ||
        version != kSchemaVersion) {
        return core::Result<MigrationPlan>::failure(parse_error(
            "migration plan framing or schema version is invalid"));
    }
    MigrationPlan plan;
    std::uint64_t from = 0U;
    std::uint64_t to = 0U;
    std::size_t rule_count = 0U;
    if (!require_token(input, "SCHEMA") ||
        !(input >> std::quoted(plan.schema_.name_space) >>
          std::quoted(plan.schema_.name)) ||
        !require_token(input, "FROM") || !read_uint(input, from) ||
        !require_token(input, "TO") || !read_uint(input, to) ||
        !require_token(input, "RULES") ||
        !read_count(input, MigrationPlan::kMaxRulesPerPlan, rule_count)) {
        return core::Result<MigrationPlan>::failure(parse_error(
            "migration plan header is malformed"));
    }
    plan.from_version_ = SchemaVersion{static_cast<std::uint32_t>(from)};
    plan.to_version_ = SchemaVersion{static_cast<std::uint32_t>(to)};
    plan.rules_.reserve(rule_count);
    for (std::size_t index = 0U; index < rule_count; ++index) {
        std::string encoded_rule;
        if (!require_token(input, "RULE") || !(input >> std::quoted(encoded_rule))) {
            return core::Result<MigrationPlan>::failure(parse_error(
                "migration plan rule is malformed"));
        }
        auto rule = MigrationRule::deserialize(encoded_rule);
        if (!rule) {
            return core::Result<MigrationPlan>::failure(
                rule.error().with_context("migration plan rule"));
        }
        plan.rules_.push_back(std::move(rule.value()));
    }
    if (!require_token(input, "END")) {
        return core::Result<MigrationPlan>::failure(parse_error(
            "migration plan is missing its END record"));
    }
    std::string trailing;
    if (input >> trailing) {
        return core::Result<MigrationPlan>::failure(parse_error(
            "migration plan contains trailing unbound data"));
    }
    for (const auto& rule : plan.rules_) {
        if (auto result = rule.validate(); !result) {
            return core::Result<MigrationPlan>::failure(result.error());
        }
    }
    return core::Result<MigrationPlan>::success(std::move(plan));
}

} // namespace carto::semantic
