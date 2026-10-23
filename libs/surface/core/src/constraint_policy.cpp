#include <carto/surface/inheritance.hpp>

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <limits>
#include <map>
#include <span>
#include <sstream>
#include <string>
#include <utility>

namespace carto::surface {

namespace {

core::Diagnostic invalid(std::string message) {
    return core::Diagnostic(core::ErrorCode::invalid_argument, std::move(message));
}

core::Diagnostic unsupported(std::string message) {
    return core::Diagnostic(core::ErrorCode::unsupported, std::move(message));
}

bool valid_metric(SurfaceConstraintMetric metric) noexcept {
    switch (metric) {
    case SurfaceConstraintMetric::max_position_error:
    case SurfaceConstraintMetric::max_normal_error:
    case SurfaceConstraintMetric::max_silhouette_error:
    case SurfaceConstraintMetric::rms_radiance_error:
    case SurfaceConstraintMetric::max_radiance_error:
    case SurfaceConstraintMetric::diffuse_error:
    case SurfaceConstraintMetric::specular_error:
    case SurfaceConstraintMetric::visibility_error:
    case SurfaceConstraintMetric::energy_error:
        return true;
    }
    return false;
}

double metric_value(const SurfaceErrorEnvelope& error, SurfaceConstraintMetric metric) {
    switch (metric) {
    case SurfaceConstraintMetric::max_position_error: return error.max_position_error;
    case SurfaceConstraintMetric::max_normal_error: return error.max_normal_error;
    case SurfaceConstraintMetric::max_silhouette_error: return error.max_silhouette_error;
    case SurfaceConstraintMetric::rms_radiance_error: return error.rms_radiance_error;
    case SurfaceConstraintMetric::max_radiance_error: return error.max_radiance_error;
    case SurfaceConstraintMetric::diffuse_error: return error.diffuse_error;
    case SurfaceConstraintMetric::specular_error: return error.specular_error;
    case SurfaceConstraintMetric::visibility_error: return error.visibility_error;
    case SurfaceConstraintMetric::energy_error: return error.energy_error;
    }
    return std::numeric_limits<double>::infinity();
}

std::string_view metric_name(SurfaceConstraintMetric metric) noexcept {
    switch (metric) {
    case SurfaceConstraintMetric::max_position_error: return "max_position_error";
    case SurfaceConstraintMetric::max_normal_error: return "max_normal_error";
    case SurfaceConstraintMetric::max_silhouette_error: return "max_silhouette_error";
    case SurfaceConstraintMetric::rms_radiance_error: return "rms_radiance_error";
    case SurfaceConstraintMetric::max_radiance_error: return "max_radiance_error";
    case SurfaceConstraintMetric::diffuse_error: return "diffuse_error";
    case SurfaceConstraintMetric::specular_error: return "specular_error";
    case SurfaceConstraintMetric::visibility_error: return "visibility_error";
    case SurfaceConstraintMetric::energy_error: return "energy_error";
    }
    return {};
}

std::string digest_text(std::string_view value) {
    return assets::sha256(std::span<const std::uint8_t>{
        reinterpret_cast<const std::uint8_t*>(value.data()), value.size()}).hex();
}

bool valid_constraints(const std::vector<SurfaceRepresentationConstraint>& constraints) {
    if (constraints.empty() || constraints.size() > 65'536U ||
        !std::is_sorted(constraints.begin(), constraints.end(),
            [](const auto& left, const auto& right) { return left.id < right.id; })) {
        return false;
    }
    for (std::size_t index = 0U; index < constraints.size(); ++index) {
        if (!constraints[index].validate() ||
            (index > 0U && constraints[index - 1U].id == constraints[index].id)) {
            return false;
        }
    }
    return true;
}

std::string constraints_canonical(
    const std::vector<SurfaceRepresentationConstraint>& constraints) {
    std::ostringstream output;
    output << "surface-representation-constraints-v1\n" << std::setprecision(17);
    for (const auto& constraint : constraints) {
        output << constraint.id << ':' << metric_name(constraint.metric) << ':'
               << constraint.maximum << '\n';
    }
    return output.str();
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

core::Result<void> SurfaceRepresentationConstraint::validate() const {
    if (id == 0U || !valid_metric(metric) || !std::isfinite(maximum) || maximum < 0.0) {
        return core::Result<void>::failure(
            invalid("surface representation constraint is invalid"));
    }
    return core::Result<void>::success();
}

core::Result<RepresentationConstraintPolicyOutcome> evaluate_inherited_constraint_policy(
    const SurfaceRepresentationTree& tree,
    const std::vector<RepresentationInheritanceRecord>& inheritance,
    const std::vector<RepresentationConstraintBinding>& bindings,
    RepresentationId representation) {
    const SurfaceRepresentationNode* target = nullptr;
    std::map<RepresentationId, const SurfaceRepresentationNode*> nodes;
    for (const auto& node : tree.nodes()) {
        nodes.emplace(node.id, &node);
        if (node.id == representation) target = &node;
    }
    if (target == nullptr) {
        return core::Result<RepresentationConstraintPolicyOutcome>::failure(
            core::Diagnostic(core::ErrorCode::not_found,
                "constraint policy target is absent from the representation tree"));
    }
    if (auto valid = tree.validate(target->domain); !valid) {
        return core::Result<RepresentationConstraintPolicyOutcome>::failure(valid.error());
    }
    std::map<RepresentationId, const RepresentationInheritanceRecord*> records;
    for (const auto& record : inheritance) {
        if (auto valid = record.validate(); !valid) {
            return core::Result<RepresentationConstraintPolicyOutcome>::failure(valid.error());
        }
        const auto node = nodes.find(record.representation);
        if (node == nodes.end() || !node->second->parent.has_value() ||
            *node->second->parent != record.parent ||
            !records.emplace(record.representation, &record).second) {
            return core::Result<RepresentationConstraintPolicyOutcome>::failure(
                invalid("constraint inheritance does not match the representation tree"));
        }
    }
    std::map<RepresentationId, const RepresentationConstraintBinding*> policy_bindings;
    for (const auto& binding : bindings) {
        if (!binding.representation || !nodes.contains(binding.representation) ||
            !valid_constraints(binding.constraints) ||
            !policy_bindings.emplace(binding.representation, &binding).second) {
            return core::Result<RepresentationConstraintPolicyOutcome>::failure(
                invalid("constraint binding has an invalid policy or duplicate identity"));
        }
    }

    std::vector<const SurfaceRepresentationNode*> path;
    const SurfaceRepresentationNode* current = target;
    while (current != nullptr) {
        path.push_back(current);
        if (!current->parent.has_value()) break;
        const auto parent = nodes.find(*current->parent);
        if (parent == nodes.end()) {
            return core::Result<RepresentationConstraintPolicyOutcome>::failure(
                invalid("constraint policy ancestry is incomplete"));
        }
        current = parent->second;
    }
    std::reverse(path.begin(), path.end());

    const std::vector<SurfaceRepresentationConstraint>* effective = nullptr;
    std::string inheritance_material = "surface-constraint-inheritance-v1\n";
    inheritance_material.append("root=");
    inheritance_material.append(std::to_string(path.front()->id.value));
    inheritance_material.push_back('\n');
    for (std::size_t index = 0U; index < path.size(); ++index) {
        const auto* node = path[index];
        const auto binding_it = policy_bindings.find(node->id);
        const RepresentationConstraintBinding* binding =
            binding_it == policy_bindings.end() ? nullptr : binding_it->second;
        if (index == 0U) {
            if (binding == nullptr || records.contains(node->id)) {
                return core::Result<RepresentationConstraintPolicyOutcome>::failure(
                    unsupported("constraint policy root has no explicit policy"));
            }
            effective = &binding->constraints;
            inheritance_material.append("root-policy=");
            inheritance_material.append(digest_text(constraints_canonical(*effective)));
            inheritance_material.push_back('\n');
            continue;
        }
        const auto record_it = records.find(node->id);
        if (record_it == records.end()) {
            return core::Result<RepresentationConstraintPolicyOutcome>::failure(
                unsupported("representation has no constraint inheritance declaration"));
        }
        const auto& record = *record_it->second;
        const bool inherits = std::binary_search(record.inherited_fields.begin(),
            record.inherited_fields.end(), RepresentationPolicyField::constraint);
        const bool overrides = std::binary_search(record.override_fields.begin(),
            record.override_fields.end(), RepresentationPolicyField::constraint);
        const auto encoded = record.serialize();
        if (!encoded) {
            return core::Result<RepresentationConstraintPolicyOutcome>::failure(encoded.error());
        }
        inheritance_material.append(encoded.value());
        inheritance_material.push_back('\n');
        if (inherits) {
            if (binding != nullptr && binding->constraints != *effective) {
                return core::Result<RepresentationConstraintPolicyOutcome>::failure(
                    invalid("inherited constraints differ from the parent policy"));
            }
        } else if (overrides) {
            if (binding == nullptr) {
                return core::Result<RepresentationConstraintPolicyOutcome>::failure(
                    unsupported("constraint override has no explicit policy value"));
            }
            effective = &binding->constraints;
        } else {
            return core::Result<RepresentationConstraintPolicyOutcome>::failure(
                unsupported("representation does not inherit or override constraints"));
        }
    }
    if (effective == nullptr) {
        return core::Result<RepresentationConstraintPolicyOutcome>::failure(
            invalid("effective representation constraints are missing"));
    }

    std::vector<std::uint64_t> violated;
    for (const auto& constraint : *effective) {
        if (metric_value(target->error, constraint.metric) > constraint.maximum) {
            violated.push_back(constraint.id);
        }
    }
    const std::string constraints_digest = digest_text(constraints_canonical(*effective));
    const std::string inheritance_digest = digest_text(inheritance_material);
    std::ostringstream canonical;
    canonical << "surface-constraint-policy-outcome-v1\n"
              << "representation=" << target->id.value << '\n'
              << "representation_digest=" << target->content_digest << '\n'
              << "constraints_digest=" << constraints_digest << '\n'
              << "inheritance_digest=" << inheritance_digest << '\n'
              << "accepted=" << (violated.empty() ? '1' : '0') << '\n'
              << "violations=";
    for (const auto id : violated) canonical << id << ',';
    canonical << "\nerror=";
    append_error(canonical, target->error);
    RepresentationConstraintPolicyOutcome outcome{
        target->id, target->content_digest, constraints_digest, inheritance_digest,
        digest_text(canonical.str()), violated, violated.empty()};
    return core::Result<RepresentationConstraintPolicyOutcome>::success(std::move(outcome));
}

} // namespace carto::surface
