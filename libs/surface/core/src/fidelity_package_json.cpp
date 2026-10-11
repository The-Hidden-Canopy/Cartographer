#include <carto/assets/blob_store.hpp>
#include <carto/surface/fidelity_package_json.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <iomanip>
#include <limits>
#include <span>
#include <sstream>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace carto::surface {
namespace {

constexpr std::string_view kKind = "vanta_cartographer_fidelity_package";

core::Diagnostic invalid(std::string message) {
    return core::Diagnostic(core::ErrorCode::validation_failed, std::move(message));
}

const char* representation_kind_name(SurfaceRepresentationKind kind) noexcept {
    switch (kind) {
    case SurfaceRepresentationKind::authored: return "authored";
    case SurfaceRepresentationKind::dense_geometry: return "dense_geometry";
    case SurfaceRepresentationKind::simplified_geometry: return "simplified_geometry";
    case SurfaceRepresentationKind::displacement_surface: return "displacement_surface";
    case SurfaceRepresentationKind::normal_distribution: return "normal_distribution";
    case SurfaceRepresentationKind::closure_proxy: return "closure_proxy";
    case SurfaceRepresentationKind::collision_proxy: return "collision_proxy";
    case SurfaceRepresentationKind::runtime_cluster: return "runtime_cluster";
    }
    return "unknown";
}

const char* migration_target_name(MigrationTarget target) noexcept {
    switch (target) {
    case MigrationTarget::displacement: return "displacement";
    case MigrationTarget::normal_field: return "normal_field";
    case MigrationTarget::ndf: return "ndf";
    case MigrationTarget::roughness: return "roughness";
    case MigrationTarget::anisotropy: return "anisotropy";
    case MigrationTarget::cavity: return "cavity";
    case MigrationTarget::directional_visibility: return "directional_visibility";
    case MigrationTarget::silhouette_descriptor: return "silhouette_descriptor";
    }
    return "unknown";
}

template <typename T>
std::vector<const T*> sorted_pointers(const std::vector<T>& values) {
    std::vector<const T*> result;
    result.reserve(values.size());
    for (const T& value : values) result.push_back(&value);
    return result;
}

void append_text(std::ostringstream& output, std::string_view key, std::string_view value) {
    output << key << value.size() << ':' << value << ';';
}

void append_error(std::ostringstream& output, const SurfaceErrorEnvelope& error) {
    output << std::setprecision(17)
           << "position=" << error.max_position_error << ';'
           << "normal=" << error.max_normal_error << ';'
           << "silhouette=" << error.max_silhouette_error << ';'
           << "rms_radiance=" << error.rms_radiance_error << ';'
           << "max_radiance=" << error.max_radiance_error << ';'
           << "diffuse=" << error.diffuse_error << ';'
           << "specular=" << error.specular_error << ';'
           << "visibility=" << error.visibility_error << ';'
           << "energy=" << error.energy_error << ';';
}

std::string vanta_canonical_without_digest(const VantaFidelityPackage& package) {
    std::ostringstream output;
    output << "vanta-cartographer-fidelity-package-v1;format=" << package.schema_version
           << ";revision=" << package.source_revision.value() << '\n';

    auto domains = sorted_pointers(package.domains);
    std::sort(domains.begin(), domains.end(), [](const SurfaceDomain* left, const SurfaceDomain* right) {
        return left->id.value < right->id.value;
    });
    for (const SurfaceDomain* domain : domains) {
        output << "domain=" << domain->id.value << ':' << domain->source_revision.value() << ';';
        append_text(output, "semantic=", domain->semantic_class);
        append_text(output, "material=", domain->material_class);
        append_text(output, "source=", domain->source_content_digest);
        append_text(output, "digest=", domain->digest);
        output << '\n';
    }

    auto representations = sorted_pointers(package.representations);
    std::sort(representations.begin(), representations.end(),
              [](const SurfaceRepresentationNode* left, const SurfaceRepresentationNode* right) {
                  return left->id.value < right->id.value;
              });
    for (const SurfaceRepresentationNode* representation : representations) {
        output << "representation=" << representation->id.value << ':' << representation->domain.value << ':'
               << (representation->parent.has_value() ? std::to_string(representation->parent->value) : "root")
               << ':' << static_cast<unsigned>(representation->kind) << ";children=";
        for (const RepresentationId child : representation->children) output << child.value << ',';
        output << ';';
        append_error(output, representation->error);
        append_text(output, "content=", representation->content_digest);
        output << '\n';
    }

    auto runtime = sorted_pointers(package.runtime_records);
    std::sort(runtime.begin(), runtime.end(),
              [](const SurfaceRuntimeRecord* left, const SurfaceRuntimeRecord* right) {
                  return std::tie(left->domain.value, left->representation.value, left->continuity_id) <
                         std::tie(right->domain.value, right->representation.value, right->continuity_id);
              });
    for (const SurfaceRuntimeRecord* record : runtime) {
        output << std::setprecision(17) << "runtime=" << record->domain.value << ':'
               << record->representation.value << ':' << record->geometric_error << ':'
               << record->radiometric_error << ':' << record->continuity_id << ':'
               << record->material_group << ':' << record->semantic_group << '\n';
    }

    auto closures = sorted_pointers(package.closures);
    std::sort(closures.begin(), closures.end(),
              [](const SurfaceClosurePolygon* left, const SurfaceClosurePolygon* right) {
                  return std::tuple{left->domain.value, left->representation.value,
                                    left->source_revision.value()} <
                         std::tuple{right->domain.value, right->representation.value,
                                    right->source_revision.value()};
              });
    for (const SurfaceClosurePolygon* closure : closures) {
        output << "closure=" << closure->domain.value << ':' << closure->representation.value << ':'
               << closure->source_revision.value() << ';';
        append_error(output, closure->error);
        append_text(output, "content=", closure->digest);
        output << '\n';
    }

    auto migrations = sorted_pointers(package.migrations);
    std::sort(migrations.begin(), migrations.end(),
              [](const RepresentationMigration* left, const RepresentationMigration* right) {
                  return std::tie(left->domain.value, left->source_representation.value,
                                  left->target_representation.value) <
                         std::tie(right->domain.value, right->source_representation.value,
                                  right->target_representation.value);
              });
    for (const RepresentationMigration* migration : migrations) {
        output << "migration=" << migration->domain.value << ':'
               << migration->source_representation.value << ':'
               << migration->target_representation.value << ':'
               << migration->source_revision.value() << ':' << std::setprecision(17)
               << migration->residual_radiometric_error << ';';
        for (const MigrationTarget target : migration->targets) {
            output << static_cast<unsigned>(target) << ',';
        }
        output << ';';
        append_text(output, "algorithm=", migration->algorithm_version);
        append_text(output, "digest=", migration->digest);
        output << '\n';
    }
    return output.str();
}

std::string sha256_hex(std::string_view value) {
    return assets::sha256(std::span<const std::uint8_t>{
        reinterpret_cast<const std::uint8_t*>(value.data()), value.size()}).hex();
}

void append_json_string(std::string& output, std::string_view value) {
    static constexpr char hex[] = "0123456789abcdef";
    output.push_back('"');
    for (const char raw_byte : value) {
        const unsigned char byte = static_cast<unsigned char>(raw_byte);
        switch (byte) {
        case '"': output += "\\\""; break;
        case '\\': output += "\\\\"; break;
        case '\b': output += "\\b"; break;
        case '\f': output += "\\f"; break;
        case '\n': output += "\\n"; break;
        case '\r': output += "\\r"; break;
        case '\t': output += "\\t"; break;
        default:
            if (byte < 0x20U) {
                output += "\\u00";
                output.push_back(hex[byte >> 4U]);
                output.push_back(hex[byte & 0x0fU]);
            } else {
                output.push_back(static_cast<char>(byte));
            }
            break;
        }
    }
    output.push_back('"');
}

void append_key(std::string& output, std::string_view key, bool& first) {
    if (!first) output.push_back(',');
    first = false;
    append_json_string(output, key);
    output.push_back(':');
}

void append_string_member(std::string& output, std::string_view key, std::string_view value, bool& first) {
    append_key(output, key, first);
    append_json_string(output, value);
}

void append_uint_member(std::string& output, std::string_view key, std::uint64_t value, bool& first) {
    append_key(output, key, first);
    output += std::to_string(value);
}

void append_number_member(std::string& output, std::string_view key, double value, bool& first) {
    append_key(output, key, first);
    std::ostringstream number;
    number << std::setprecision(17) << value;
    output += number.str();
}

void append_error_json(std::string& output, const SurfaceErrorEnvelope& error, bool& first) {
    append_key(output, "error", first);
    output.push_back('{');
    bool nested_first = true;
    append_number_member(output, "max_position_error", error.max_position_error, nested_first);
    append_number_member(output, "max_normal_error", error.max_normal_error, nested_first);
    append_number_member(output, "max_silhouette_error", error.max_silhouette_error, nested_first);
    append_number_member(output, "rms_radiance_error", error.rms_radiance_error, nested_first);
    append_number_member(output, "max_radiance_error", error.max_radiance_error, nested_first);
    append_number_member(output, "diffuse_error", error.diffuse_error, nested_first);
    append_number_member(output, "specular_error", error.specular_error, nested_first);
    append_number_member(output, "visibility_error", error.visibility_error, nested_first);
    append_number_member(output, "energy_error", error.energy_error, nested_first);
    output.push_back('}');
}

void append_domain_json(std::string& output, const SurfaceDomain& domain) {
    output.push_back('{');
    bool first = true;
    append_uint_member(output, "id", domain.id.value, first);
    append_uint_member(output, "source_revision", domain.source_revision.value(), first);
    append_string_member(output, "semantic_class", domain.semantic_class, first);
    append_string_member(output, "material_class", domain.material_class, first);
    append_string_member(output, "source_content_digest", domain.source_content_digest, first);
    append_string_member(output, "digest", domain.digest, first);
    output.push_back('}');
}

void append_representation_json(std::string& output, const SurfaceRepresentationNode& representation) {
    output.push_back('{');
    bool first = true;
    append_uint_member(output, "id", representation.id.value, first);
    append_uint_member(output, "domain", representation.domain.value, first);
    if (representation.parent.has_value()) append_uint_member(output, "parent", representation.parent->value, first);
    append_key(output, "children", first);
    output.push_back('[');
    for (std::size_t index = 0U; index < representation.children.size(); ++index) {
        if (index != 0U) output.push_back(',');
        output += std::to_string(representation.children[index].value);
    }
    output.push_back(']');
    append_string_member(output, "kind", representation_kind_name(representation.kind), first);
    append_error_json(output, representation.error, first);
    append_string_member(output, "content_digest", representation.content_digest, first);
    output.push_back('}');
}

void append_runtime_json(std::string& output, const SurfaceRuntimeRecord& record) {
    output.push_back('{');
    bool first = true;
    append_uint_member(output, "domain", record.domain.value, first);
    append_uint_member(output, "representation", record.representation.value, first);
    append_number_member(output, "geometric_error", record.geometric_error, first);
    append_number_member(output, "radiometric_error", record.radiometric_error, first);
    append_uint_member(output, "continuity_id", record.continuity_id, first);
    append_uint_member(output, "material_group", record.material_group, first);
    append_uint_member(output, "semantic_group", record.semantic_group, first);
    output.push_back('}');
}

void append_closure_json(std::string& output, const SurfaceClosurePolygon& closure) {
    output.push_back('{');
    bool first = true;
    append_uint_member(output, "domain", closure.domain.value, first);
    append_uint_member(output, "representation", closure.representation.value, first);
    append_uint_member(output, "source_revision", closure.source_revision.value(), first);
    append_error_json(output, closure.error, first);
    append_string_member(output, "content_digest", closure.digest, first);
    output.push_back('}');
}

void append_migration_json(std::string& output, const RepresentationMigration& migration) {
    output.push_back('{');
    bool first = true;
    append_uint_member(output, "domain", migration.domain.value, first);
    append_uint_member(output, "source_representation", migration.source_representation.value, first);
    append_uint_member(output, "target_representation", migration.target_representation.value, first);
    append_key(output, "targets", first);
    output.push_back('[');
    for (std::size_t index = 0U; index < migration.targets.size(); ++index) {
        if (index != 0U) output.push_back(',');
        append_json_string(output, migration_target_name(migration.targets[index]));
    }
    output.push_back(']');
    append_number_member(output, "residual_radiometric_error", migration.residual_radiometric_error, first);
    append_uint_member(output, "source_revision", migration.source_revision.value(), first);
    append_string_member(output, "algorithm_version", migration.algorithm_version, first);
    append_string_member(output, "digest", migration.digest, first);
    output.push_back('}');
}

std::string to_json(const VantaFidelityPackage& package, std::string_view package_digest) {
    std::string output;
    output.reserve(4096U);
    output.push_back('{');
    bool first = true;
    append_string_member(output, "kind", kKind, first);
    append_uint_member(output, "format_version", package.schema_version, first);
    append_uint_member(output, "source_revision", package.source_revision.value(), first);

    auto domains = sorted_pointers(package.domains);
    std::sort(domains.begin(), domains.end(), [](const SurfaceDomain* left, const SurfaceDomain* right) {
        return left->id.value < right->id.value;
    });
    append_key(output, "domains", first);
    output.push_back('[');
    for (std::size_t index = 0U; index < domains.size(); ++index) {
        if (index != 0U) output.push_back(',');
        append_domain_json(output, *domains[index]);
    }
    output.push_back(']');

    auto representations = sorted_pointers(package.representations);
    std::sort(representations.begin(), representations.end(),
              [](const SurfaceRepresentationNode* left, const SurfaceRepresentationNode* right) {
                  return left->id.value < right->id.value;
              });
    append_key(output, "representations", first);
    output.push_back('[');
    for (std::size_t index = 0U; index < representations.size(); ++index) {
        if (index != 0U) output.push_back(',');
        append_representation_json(output, *representations[index]);
    }
    output.push_back(']');

    auto runtime = sorted_pointers(package.runtime_records);
    std::sort(runtime.begin(), runtime.end(),
              [](const SurfaceRuntimeRecord* left, const SurfaceRuntimeRecord* right) {
                  return std::tie(left->domain.value, left->representation.value, left->continuity_id) <
                         std::tie(right->domain.value, right->representation.value, right->continuity_id);
              });
    append_key(output, "runtime_records", first);
    output.push_back('[');
    for (std::size_t index = 0U; index < runtime.size(); ++index) {
        if (index != 0U) output.push_back(',');
        append_runtime_json(output, *runtime[index]);
    }
    output.push_back(']');

    auto closures = sorted_pointers(package.closures);
    std::sort(closures.begin(), closures.end(),
              [](const SurfaceClosurePolygon* left, const SurfaceClosurePolygon* right) {
                  return std::tuple{left->domain.value, left->representation.value,
                                    left->source_revision.value()} <
                         std::tuple{right->domain.value, right->representation.value,
                                    right->source_revision.value()};
              });
    append_key(output, "closures", first);
    output.push_back('[');
    for (std::size_t index = 0U; index < closures.size(); ++index) {
        if (index != 0U) output.push_back(',');
        append_closure_json(output, *closures[index]);
    }
    output.push_back(']');

    auto migrations = sorted_pointers(package.migrations);
    std::sort(migrations.begin(), migrations.end(),
              [](const RepresentationMigration* left, const RepresentationMigration* right) {
                  return std::tie(left->domain.value, left->source_representation.value,
                                  left->target_representation.value) <
                         std::tie(right->domain.value, right->source_representation.value,
                                  right->target_representation.value);
              });
    append_key(output, "migrations", first);
    output.push_back('[');
    for (std::size_t index = 0U; index < migrations.size(); ++index) {
        if (index != 0U) output.push_back(',');
        append_migration_json(output, *migrations[index]);
    }
    output.push_back(']');
    append_string_member(output, "package_digest", package_digest, first);
    output.push_back('}');
    return output;
}

} // namespace

core::Result<std::string> serialize_vanta_fidelity_package_json(
    const VantaFidelityPackage& package,
    const geometry::CompiledMesh& source) {
    const core::Result<void> valid = package.validate(source);
    if (!valid) {
        return core::Result<std::string>::failure(
            invalid("cannot serialize invalid VANTA fidelity package: " + valid.error().message));
    }
    const std::string canonical = vanta_canonical_without_digest(package);
    const std::string package_digest = sha256_hex(canonical);
    std::string output = to_json(package, package_digest);
    if (output.size() > kMaxVantaFidelityPackageJsonBytes) {
        return core::Result<std::string>::failure(
            invalid("VANTA fidelity package JSON exceeds its safety bound"));
    }
    return core::Result<std::string>::success(std::move(output));
}

} // namespace carto::surface
