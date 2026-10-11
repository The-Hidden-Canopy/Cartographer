#pragma once

#include <carto/core/result.hpp>
#include <carto/semantic/schema.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace carto::semantic {

struct FieldRename {
    std::string from;
    std::string to;

    [[nodiscard]] bool operator==(const FieldRename&) const noexcept = default;
};

// One validated migration hop between two registered schema revisions of the
// same schema. A rule must explicitly preserve every game-owned field of the
// from-revision: game-owned state is never dropped, renamed, or re-owned by
// a migration.
struct MigrationRule {
    static constexpr std::uint32_t kSchemaVersion = 1U;
    static constexpr std::size_t kMaxSerializedBytes = 4U * 1024U * 1024U;
    static constexpr std::size_t kMaxRenamesPerRule = 4096U;
    static constexpr std::size_t kMaxPreservedFieldsPerRule = 4096U;

    SchemaIdentity schema;
    SchemaVersion from_version;
    SchemaVersion to_version;
    // Sorted by `from`, unique in both directions.
    std::vector<FieldRename> renames;
    // Sorted, unique; must cover every game-owned field of the from-revision.
    std::vector<std::string> preserved_game_owned_fields;

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] std::string serialize() const;
    [[nodiscard]] static core::Result<MigrationRule> deserialize(std::string_view text);
};

// An ordered chain of migration rules from one registered version to another.
// Plans are composed only from caller-supplied rules and registered schema
// revisions; a version jump with no covering rule is unsupported and fails
// closed. Plan validation re-checks every hop and the game-owned preservation
// obligation against the registry.
class MigrationPlan final {
public:
    static constexpr std::uint32_t kSchemaVersion = 1U;
    static constexpr std::size_t kMaxRulesPerPlan = 4096U;

    [[nodiscard]] static core::Result<MigrationPlan> compose(
        const SchemaRegistry& registry,
        const SchemaIdentity& schema,
        SchemaVersion from,
        SchemaVersion to,
        std::span<const MigrationRule> available_rules);

    [[nodiscard]] const SchemaIdentity& schema() const noexcept { return schema_; }
    [[nodiscard]] SchemaVersion from_version() const noexcept { return from_version_; }
    [[nodiscard]] SchemaVersion to_version() const noexcept { return to_version_; }
    [[nodiscard]] const std::vector<MigrationRule>& rules() const noexcept { return rules_; }

    [[nodiscard]] core::Result<void> validate_against(const SchemaRegistry& registry) const;
    [[nodiscard]] std::string serialize() const;
    [[nodiscard]] static core::Result<MigrationPlan> deserialize(std::string_view text);

private:
    SchemaIdentity schema_;
    SchemaVersion from_version_;
    SchemaVersion to_version_;
    std::vector<MigrationRule> rules_;
};

} // namespace carto::semantic
