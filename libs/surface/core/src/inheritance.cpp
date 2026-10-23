#include <carto/surface/inheritance.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <iomanip>
#include <iterator>
#include <map>
#include <set>
#include <span>
#include <sstream>
#include <string>
#include <system_error>
#include <utility>

namespace carto::surface {

namespace {

core::Diagnostic invalid(std::string message) {
    return core::Diagnostic(core::ErrorCode::invalid_argument, std::move(message));
}

bool valid_field(RepresentationPolicyField field) noexcept {
    switch (field) {
    case RepresentationPolicyField::view:
    case RepresentationPolicyField::material:
    case RepresentationPolicyField::constraint:
    case RepresentationPolicyField::fracture:
        return true;
    }
    return false;
}

std::string_view field_name(RepresentationPolicyField field) noexcept {
    switch (field) {
    case RepresentationPolicyField::view: return "view";
    case RepresentationPolicyField::material: return "material";
    case RepresentationPolicyField::constraint: return "constraint";
    case RepresentationPolicyField::fracture: return "fracture";
    }
    return {};
}

core::Result<std::vector<RepresentationPolicyField>> parse_fields(std::string_view value) {
    std::vector<RepresentationPolicyField> fields;
    if (value.empty()) return core::Result<std::vector<RepresentationPolicyField>>::success({});
    while (!value.empty()) {
        const std::size_t separator = value.find(',');
        const std::string_view token = value.substr(0U, separator);
        RepresentationPolicyField field{};
        if (token == "view") field = RepresentationPolicyField::view;
        else if (token == "material") field = RepresentationPolicyField::material;
        else if (token == "constraint") field = RepresentationPolicyField::constraint;
        else if (token == "fracture") field = RepresentationPolicyField::fracture;
        else {
            return core::Result<std::vector<RepresentationPolicyField>>::failure(
                invalid("inheritance record contains an unknown policy field"));
        }
        fields.push_back(field);
        if (separator == std::string_view::npos) break;
        value.remove_prefix(separator + 1U);
        if (value.empty()) {
            return core::Result<std::vector<RepresentationPolicyField>>::failure(
                invalid("inheritance record contains an empty policy field"));
        }
    }
    return core::Result<std::vector<RepresentationPolicyField>>::success(std::move(fields));
}

core::Result<std::uint64_t> parse_id(std::string_view value) {
    std::uint64_t id = 0U;
    const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), id);
    if (value.empty() || error != std::errc{} || end != value.data() + value.size() || id == 0U ||
        (value.size() > 1U && value.front() == '0')) {
        return core::Result<std::uint64_t>::failure(
            invalid("inheritance record contains an invalid identity"));
    }
    return core::Result<std::uint64_t>::success(id);
}

std::string digest_text(std::string_view value) {
    return assets::sha256(std::span<const std::uint8_t>{
        reinterpret_cast<const std::uint8_t*>(value.data()), value.size()}).hex();
}

bool satisfies(const SurfaceErrorEnvelope& error, const SurfaceQualityPolicy& policy) noexcept {
    return error.rms_radiance_error <= policy.max_rms_radiance_error &&
        error.max_radiance_error <= policy.max_radiance_error &&
        error.diffuse_error <= policy.max_diffuse_error &&
        error.specular_error <= policy.max_specular_error &&
        error.visibility_error <= policy.max_visibility_error &&
        error.max_silhouette_error <= policy.max_silhouette_error;
}

void append_error(std::ostringstream& output, const SurfaceErrorEnvelope& error) {
    output << std::setprecision(17)
           << error.max_position_error << ',' << error.max_normal_error << ','
           << error.max_silhouette_error << ',' << error.rms_radiance_error << ','
           << error.max_radiance_error << ',' << error.diffuse_error << ','
           << error.specular_error << ',' << error.visibility_error << ','
           << error.energy_error;
}

} // namespace

core::Result<void> RepresentationInheritanceRecord::validate() const {
    if (!representation || !parent || representation == parent) {
        return core::Result<void>::failure(
            invalid("inheritance record identities are invalid"));
    }
    const auto valid_set = [](const std::vector<RepresentationPolicyField>& fields) {
        return std::all_of(fields.begin(), fields.end(), valid_field) &&
            std::is_sorted(fields.begin(), fields.end()) &&
            std::adjacent_find(fields.begin(), fields.end()) == fields.end();
    };
    if (!valid_set(inherited_fields) || !valid_set(override_fields)) {
        return core::Result<void>::failure(
            invalid("inheritance record fields are unknown, duplicated, or unordered"));
    }
    std::vector<RepresentationPolicyField> overlap;
    std::set_intersection(inherited_fields.begin(), inherited_fields.end(),
        override_fields.begin(), override_fields.end(), std::back_inserter(overlap));
    if (!overlap.empty()) {
        return core::Result<void>::failure(
            invalid("a policy field cannot be inherited and overridden"));
    }
    return core::Result<void>::success();
}

core::Result<std::string> RepresentationInheritanceRecord::serialize() const {
    if (auto result = validate(); !result) {
        return core::Result<std::string>::failure(result.error());
    }
    const auto append_fields = [](std::string& output,
                                  const std::vector<RepresentationPolicyField>& fields) {
        bool first = true;
        for (const auto field : fields) {
            if (!first) output.push_back(',');
            output.append(field_name(field));
            first = false;
        }
    };
    std::string output = "surface-representation-inheritance-v1\nrepresentation=";
    output.append(std::to_string(representation.value));
    output.append("\nparent=");
    output.append(std::to_string(parent.value));
    output.append("\ninherited=");
    append_fields(output, inherited_fields);
    output.append("\noverrides=");
    append_fields(output, override_fields);
    return core::Result<std::string>::success(std::move(output));
}

core::Result<RepresentationInheritanceRecord> RepresentationInheritanceRecord::parse(
    std::string_view bytes) {
    const std::string original(bytes);
    constexpr std::string_view prefix = "surface-representation-inheritance-v1\n";
    if (!bytes.starts_with(prefix)) {
        return core::Result<RepresentationInheritanceRecord>::failure(
            invalid("inheritance record version or header is unsupported"));
    }
    bytes.remove_prefix(prefix.size());
    std::array<std::string_view, 4U> lines{};
    for (std::size_t i = 0U; i < lines.size(); ++i) {
        const std::size_t newline = bytes.find('\n');
        if (i + 1U == lines.size()) {
            if (newline != std::string_view::npos) {
                return core::Result<RepresentationInheritanceRecord>::failure(
                    invalid("inheritance record has trailing fields"));
            }
            lines[i] = bytes;
        } else {
            if (newline == std::string_view::npos) {
                return core::Result<RepresentationInheritanceRecord>::failure(
                    invalid("inheritance record is truncated"));
            }
            lines[i] = bytes.substr(0U, newline);
            bytes.remove_prefix(newline + 1U);
        }
    }
    constexpr std::array<std::string_view, 4U> keys{
        "representation=", "parent=", "inherited=", "overrides="};
    std::array<std::string_view, 4U> values{};
    for (std::size_t i = 0U; i < lines.size(); ++i) {
        if (!lines[i].starts_with(keys[i])) {
            return core::Result<RepresentationInheritanceRecord>::failure(
                invalid("inheritance record has an unknown or out-of-order field"));
        }
        values[i] = lines[i].substr(keys[i].size());
    }
    const auto representation = parse_id(values[0]);
    const auto parent = parse_id(values[1]);
    const auto inherited = parse_fields(values[2]);
    const auto overrides = parse_fields(values[3]);
    if (!representation || !parent || !inherited || !overrides) {
        if (!representation) return core::Result<RepresentationInheritanceRecord>::failure(representation.error());
        if (!parent) return core::Result<RepresentationInheritanceRecord>::failure(parent.error());
        if (!inherited) return core::Result<RepresentationInheritanceRecord>::failure(inherited.error());
        return core::Result<RepresentationInheritanceRecord>::failure(overrides.error());
    }
    RepresentationInheritanceRecord result{
        RepresentationId{representation.value()}, RepresentationId{parent.value()},
        inherited.value(), overrides.value()};
    if (auto valid = result.validate(); !valid) {
        return core::Result<RepresentationInheritanceRecord>::failure(valid.error());
    }
    const auto canonical = result.serialize();
    if (!canonical || canonical.value() != original) {
        return core::Result<RepresentationInheritanceRecord>::failure(
            invalid("inheritance record is not in canonical form"));
    }
    return core::Result<RepresentationInheritanceRecord>::success(std::move(result));
}

core::Result<RepresentationViewPolicyOutcome> evaluate_inherited_view_policy(
    const SurfaceRepresentationTree& tree,
    const std::vector<RepresentationInheritanceRecord>& inheritance,
    const std::vector<RepresentationViewPolicyBinding>& bindings,
    RepresentationId representation) {
    const SurfaceRepresentationNode* target = nullptr;
    for (const auto& node : tree.nodes()) {
        if (node.id == representation) target = &node;
    }
    if (target == nullptr) {
        return core::Result<RepresentationViewPolicyOutcome>::failure(
            core::Diagnostic(core::ErrorCode::not_found,
                "view policy target is absent from the representation tree"));
    }
    if (auto valid = tree.validate(target->domain); !valid) {
        return core::Result<RepresentationViewPolicyOutcome>::failure(valid.error());
    }

    std::map<RepresentationId, const SurfaceRepresentationNode*> nodes;
    for (const auto& node : tree.nodes()) nodes.emplace(node.id, &node);
    std::map<RepresentationId, const RepresentationInheritanceRecord*> records;
    for (const auto& record : inheritance) {
        if (auto valid = record.validate(); !valid) {
            return core::Result<RepresentationViewPolicyOutcome>::failure(valid.error());
        }
        const auto node = nodes.find(record.representation);
        if (node == nodes.end() || !node->second->parent.has_value() ||
            *node->second->parent != record.parent ||
            !records.emplace(record.representation, &record).second) {
            return core::Result<RepresentationViewPolicyOutcome>::failure(
                invalid("view policy inheritance does not match the representation tree"));
        }
    }

    std::map<RepresentationId, const RepresentationViewPolicyBinding*> policy_bindings;
    for (const auto& binding : bindings) {
        if (!binding.representation || !nodes.contains(binding.representation) ||
            !policy_bindings.emplace(binding.representation, &binding).second) {
            return core::Result<RepresentationViewPolicyOutcome>::failure(
                invalid("view policy binding has an unknown or duplicate representation"));
        }
        if (binding.override_policy) {
            if (auto valid = binding.override_policy->validate(); !valid) {
                return core::Result<RepresentationViewPolicyOutcome>::failure(valid.error());
            }
        }
    }

    std::vector<const SurfaceRepresentationNode*> path;
    const SurfaceRepresentationNode* current = target;
    while (current != nullptr) {
        path.push_back(current);
        if (!current->parent.has_value()) break;
        const auto parent = nodes.find(*current->parent);
        if (parent == nodes.end()) {
            return core::Result<RepresentationViewPolicyOutcome>::failure(
                invalid("view policy ancestry is incomplete"));
        }
        current = parent->second;
    }
    std::reverse(path.begin(), path.end());

    const SurfaceQualityPolicy* effective = nullptr;
    std::string inheritance_material = "surface-view-inheritance-v1\n";
    inheritance_material.append("root=");
    inheritance_material.append(std::to_string(path.front()->id.value));
    inheritance_material.push_back('\n');
    for (std::size_t index = 0U; index < path.size(); ++index) {
        const auto* node = path[index];
        const auto binding_it = policy_bindings.find(node->id);
        const RepresentationViewPolicyBinding* binding =
            binding_it == policy_bindings.end() ? nullptr : binding_it->second;
        if (index == 0U) {
            if (binding == nullptr || !binding->override_policy) {
                return core::Result<RepresentationViewPolicyOutcome>::failure(
                    core::Diagnostic(core::ErrorCode::unsupported,
                        "view policy root has no explicit policy"));
            }
            if (records.contains(node->id)) {
                return core::Result<RepresentationViewPolicyOutcome>::failure(
                    invalid("representation root cannot have an inheritance record"));
            }
            effective = &*binding->override_policy;
            inheritance_material.append("root-policy=");
            inheritance_material.append(effective->digest().hex());
            inheritance_material.push_back('\n');
            continue;
        }

        const auto record_it = records.find(node->id);
        if (record_it == records.end()) {
            return core::Result<RepresentationViewPolicyOutcome>::failure(
                core::Diagnostic(core::ErrorCode::unsupported,
                    "representation has no inherited policy declaration"));
        }
        const auto& record = *record_it->second;
        const bool inherits_view = std::binary_search(
            record.inherited_fields.begin(), record.inherited_fields.end(),
            RepresentationPolicyField::view);
        const bool overrides_view = std::binary_search(
            record.override_fields.begin(), record.override_fields.end(),
            RepresentationPolicyField::view);
        const auto serialized_record = record.serialize();
        if (!serialized_record) {
            return core::Result<RepresentationViewPolicyOutcome>::failure(
                serialized_record.error());
        }
        inheritance_material.append(serialized_record.value());
        inheritance_material.push_back('\n');

        if (inherits_view) {
            if (binding != nullptr && binding->override_policy) {
                return core::Result<RepresentationViewPolicyOutcome>::failure(
                    invalid("inherited view policy cannot also provide an override"));
            }
        } else if (overrides_view) {
            if (binding == nullptr || !binding->override_policy) {
                return core::Result<RepresentationViewPolicyOutcome>::failure(
                    core::Diagnostic(core::ErrorCode::unsupported,
                        "view policy override has no supported policy value"));
            }
            effective = &*binding->override_policy;
        } else {
            return core::Result<RepresentationViewPolicyOutcome>::failure(
                core::Diagnostic(core::ErrorCode::unsupported,
                    "representation does not inherit or override the view policy"));
        }
    }

    if (effective == nullptr) {
        return core::Result<RepresentationViewPolicyOutcome>::failure(
            invalid("effective view policy is missing"));
    }
    const std::string policy_digest = effective->digest().hex();
    const std::string inheritance_digest = digest_text(inheritance_material);
    const bool accepted = satisfies(target->error, *effective);
    std::ostringstream canonical;
    canonical << "surface-view-policy-outcome-v1\n"
              << "representation=" << target->id.value << '\n'
              << "representation_digest=" << target->content_digest << '\n'
              << "policy_digest=" << policy_digest << '\n'
              << "inheritance_digest=" << inheritance_digest << '\n'
              << "accepted=" << (accepted ? '1' : '0') << '\n'
              << "error=";
    append_error(canonical, target->error);
    RepresentationViewPolicyOutcome outcome{
        target->id, target->content_digest, policy_digest, inheritance_digest,
        digest_text(canonical.str()), accepted};
    return core::Result<RepresentationViewPolicyOutcome>::success(std::move(outcome));
}

} // namespace carto::surface
