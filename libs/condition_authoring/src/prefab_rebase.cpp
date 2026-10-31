#include <carto/condition/prefab_rebase.hpp>

#include <algorithm>
#include <tuple>
#include <utility>

namespace carto::condition {
namespace {

core::Diagnostic invalid(std::string message) {
    return {core::ErrorCode::invalid_argument, std::move(message)};
}

core::Diagnostic validation(std::string message) {
    return {core::ErrorCode::validation_failed, std::move(message)};
}

const SourceAsset* find_asset(const AuthoringDocument& authoring, Id id) {
    const auto found = std::lower_bound(authoring.assets.begin(), authoring.assets.end(), id,
        [](const auto& asset, Id target) { return asset.id < target; });
    return found == authoring.assets.end() || found->id != id ? nullptr : &*found;
}

const SourceEvidence* find_source(const AuthoringDocument& authoring, Id id) {
    const auto found = std::lower_bound(authoring.sources.begin(), authoring.sources.end(), id,
        [](const auto& source, Id target) { return source.id < target; });
    return found == authoring.sources.end() || found->id != id ? nullptr : &*found;
}

const PrefabInstanceMember* find_member(
    const PrefabInstanceRecord& instance,
    Id asset_id) {
    const auto found = std::lower_bound(instance.members.begin(), instance.members.end(), asset_id,
        [](const auto& member, Id target) { return member.asset_id < target; });
    return found == instance.members.end() || found->asset_id != asset_id ? nullptr : &*found;
}

} // namespace

core::Result<void> PrefabRebaseChange::validate() const {
    if (static_cast<unsigned>(kind) >
            static_cast<unsigned>(PrefabRebaseChangeKind::source_evidence_stale) ||
        asset_id == 0U || previous_source_digest.is_zero() != previous_source_revision.empty() ||
        current_source_digest.is_zero() != current_source_revision.empty()) {
        return core::Result<void>::failure(validation(
            "prefab rebase change identity or source evidence is inconsistent"));
    }
    switch (kind) {
    case PrefabRebaseChangeKind::member_added:
        if (instance_object_id != 0U || previous_source_object_id != 0U ||
            !previous_source_digest.is_zero() || current_source_digest.is_zero() ||
            current_source_object_id == 0U) {
            return core::Result<void>::failure(validation(
                "prefab rebase addition does not bind one current source member"));
        }
        break;
    case PrefabRebaseChangeKind::member_removed:
        if (instance_object_id == 0U || previous_source_object_id == 0U ||
            previous_source_digest.is_zero() || current_source_object_id != 0U ||
            !current_source_digest.is_zero()) {
            return core::Result<void>::failure(validation(
                "prefab rebase removal does not bind one existing instance member"));
        }
        break;
    case PrefabRebaseChangeKind::source_binding_changed:
        if (instance_object_id == 0U || previous_source_object_id == 0U ||
            current_source_object_id == 0U || previous_source_digest.is_zero() ||
            current_source_digest.is_zero() ||
            (previous_source_object_id == current_source_object_id &&
             previous_source_revision == current_source_revision &&
             previous_source_digest == current_source_digest)) {
            return core::Result<void>::failure(validation(
                "prefab rebase source binding change lacks an actual binding difference"));
        }
        break;
    case PrefabRebaseChangeKind::source_evidence_stale:
        if (current_source_object_id == 0U || current_source_digest.is_zero()) {
            return core::Result<void>::failure(validation(
                "prefab rebase stale evidence does not identify the current source asset"));
        }
        break;
    }
    return core::Result<void>::success();
}

bool PrefabRebaseReview::has_changes() const noexcept {
    return !changes.empty() || instance_prefab_revision != current_prefab_revision;
}

core::Result<void> PrefabRebaseReview::validate(
    const AuthoringDocument& authoring) const {
    if (instance_id == 0U || prefab_id == 0U ||
        captured_authoring_revision != authoring.project_revision ||
        captured_authoring_digest.is_zero() || !project_mesh_review_required ||
        instance_prefab_revision.exhausted() || current_prefab_revision.exhausted()) {
        return core::Result<void>::failure(core::Diagnostic(core::ErrorCode::stale_data,
            "prefab rebase review identity, authoring receipt, or project gate is invalid"));
    }
    for (const auto& change : changes) {
        if (auto valid = change.validate(); !valid) {
            return core::Result<void>::failure(valid.error());
        }
    }
    if (!std::is_sorted(changes.begin(), changes.end(), [](const auto& left, const auto& right) {
            return std::tie(left.asset_id, left.kind) < std::tie(right.asset_id, right.kind);
        }) || std::adjacent_find(changes.begin(), changes.end(), [](const auto& left, const auto& right) {
            return left.asset_id == right.asset_id && left.kind == right.kind;
        }) != changes.end()) {
        return core::Result<void>::failure(validation(
            "prefab rebase changes are not uniquely ordered"));
    }
    const auto current = between(authoring, instance_id);
    if (!current || current.value() != *this) {
        return core::Result<void>::failure(core::Diagnostic(core::ErrorCode::stale_data,
            "prefab rebase review no longer matches authoring state"));
    }
    return core::Result<void>::success();
}

namespace {

core::Result<PrefabRebaseReview> build_review(
    const AuthoringDocument& authoring,
    Id requested_instance_id,
    const assets::Sha256Digest& authoring_digest) {
    if (requested_instance_id == 0U) {
        return core::Result<PrefabRebaseReview>::failure(invalid(
            "prefab rebase review requires a non-zero instance identity"));
    }
    const auto instance_position = std::lower_bound(
        authoring.prefab_instances.begin(), authoring.prefab_instances.end(),
        requested_instance_id, [](const auto& item, Id target) {
            return item.id < target;
        });
    if (instance_position == authoring.prefab_instances.end() ||
        instance_position->id != requested_instance_id) {
        return core::Result<PrefabRebaseReview>::failure(core::Diagnostic(
            core::ErrorCode::not_found, "prefab rebase instance does not exist"));
    }
    const auto prefab_position = std::lower_bound(
        authoring.prefabs.begin(), authoring.prefabs.end(), instance_position->prefab_id,
        [](const auto& item, Id target) { return item.id < target; });
    if (prefab_position == authoring.prefabs.end() ||
        prefab_position->id != instance_position->prefab_id) {
        return core::Result<PrefabRebaseReview>::failure(core::Diagnostic(
            core::ErrorCode::not_found, "prefab rebase definition does not exist"));
    }
    PrefabRebaseReview review;
    review.instance_id = instance_position->id;
    review.prefab_id = prefab_position->id;
    review.captured_authoring_revision = authoring.project_revision;
    review.captured_authoring_digest = authoring_digest;
    review.instance_prefab_revision = instance_position->prefab_source_revision;
    review.current_prefab_revision = prefab_position->source_revision;

    for (const Id asset_id : prefab_position->asset_ids) {
        const auto* asset = find_asset(authoring, asset_id);
        if (asset == nullptr) {
            return core::Result<PrefabRebaseReview>::failure(validation(
                "prefab rebase definition references a missing source asset"));
        }
        const auto* member = find_member(*instance_position, asset_id);
        if (member == nullptr) {
            PrefabRebaseChange added;
            added.kind = PrefabRebaseChangeKind::member_added;
            added.asset_id = asset_id;
            added.current_source_object_id = asset->authored_object_id;
            added.current_source_revision = asset->source_revision;
            added.current_source_digest = asset->source_digest;
            review.changes.push_back(std::move(added));
        } else if (member->source_object_id != asset->authored_object_id ||
            member->source_revision != asset->source_revision ||
            member->source_digest != asset->source_digest) {
            PrefabRebaseChange changed;
            changed.kind = PrefabRebaseChangeKind::source_binding_changed;
            changed.asset_id = asset_id;
            changed.instance_object_id = member->instance_object_id;
            changed.previous_source_object_id = member->source_object_id;
            changed.current_source_object_id = asset->authored_object_id;
            changed.previous_source_revision = member->source_revision;
            changed.current_source_revision = asset->source_revision;
            changed.previous_source_digest = member->source_digest;
            changed.current_source_digest = asset->source_digest;
            review.changes.push_back(std::move(changed));
        }

        const auto* evidence = find_source(authoring, asset->source_id);
        if (evidence == nullptr || !evidence->current ||
            evidence->revision != asset->source_revision ||
            evidence->digest != asset->source_digest) {
            PrefabRebaseChange stale;
            stale.kind = PrefabRebaseChangeKind::source_evidence_stale;
            stale.asset_id = asset_id;
            stale.instance_object_id = member == nullptr ? 0U : member->instance_object_id;
            stale.previous_source_object_id = member == nullptr ? 0U : member->source_object_id;
            stale.current_source_object_id = asset->authored_object_id;
            stale.previous_source_revision = member == nullptr
                ? std::string{} : member->source_revision;
            stale.current_source_revision = asset->source_revision;
            stale.previous_source_digest = member == nullptr
                ? assets::Sha256Digest{} : member->source_digest;
            stale.current_source_digest = asset->source_digest;
            review.changes.push_back(std::move(stale));
        }
    }
    for (const auto& member : instance_position->members) {
        if (std::binary_search(prefab_position->asset_ids.begin(),
                prefab_position->asset_ids.end(), member.asset_id)) {
            continue;
        }
        PrefabRebaseChange removed;
        removed.kind = PrefabRebaseChangeKind::member_removed;
        removed.asset_id = member.asset_id;
        removed.instance_object_id = member.instance_object_id;
        removed.previous_source_object_id = member.source_object_id;
        removed.previous_source_revision = member.source_revision;
        removed.previous_source_digest = member.source_digest;
        review.changes.push_back(std::move(removed));
    }
    std::sort(review.changes.begin(), review.changes.end(), [](const auto& left, const auto& right) {
        return std::tie(left.asset_id, left.kind) < std::tie(right.asset_id, right.kind);
    });
    if (review.instance_id == 0U || review.prefab_id == 0U ||
        review.captured_authoring_digest.is_zero() ||
        std::any_of(review.changes.begin(), review.changes.end(), [](const auto& change) {
            return !change.validate();
        })) {
        return core::Result<PrefabRebaseReview>::failure(validation(
            "prefab rebase review could not be constructed from valid authoring evidence"));
    }
    return core::Result<PrefabRebaseReview>::success(std::move(review));
}

} // namespace

core::Result<PrefabRebaseReview> PrefabRebaseReview::between(
    const AuthoringDocument& authoring,
    Id requested_instance_id) {
    if (auto valid = authoring.validate(); !valid) {
        return core::Result<PrefabRebaseReview>::failure(
            valid.error().with_context("prefab rebase authoring source"));
    }
    const auto authoring_digest = authoring.canonical_digest();
    if (!authoring_digest) {
        return core::Result<PrefabRebaseReview>::failure(authoring_digest.error());
    }
    return build_review(authoring, requested_instance_id, authoring_digest.value());
}

core::Result<std::vector<PrefabRebaseReview>> PrefabRebaseReview::all(
    const AuthoringDocument& authoring) {
    if (auto valid = authoring.validate(); !valid) {
        return core::Result<std::vector<PrefabRebaseReview>>::failure(
            valid.error().with_context("prefab rebase authoring source"));
    }
    const auto authoring_digest = authoring.canonical_digest();
    if (!authoring_digest) {
        return core::Result<std::vector<PrefabRebaseReview>>::failure(authoring_digest.error());
    }
    std::vector<PrefabRebaseReview> reviews;
    reviews.reserve(authoring.prefab_instances.size());
    for (const auto& instance : authoring.prefab_instances) {
        auto review = build_review(authoring, instance.id, authoring_digest.value());
        if (!review) {
            return core::Result<std::vector<PrefabRebaseReview>>::failure(review.error());
        }
        reviews.push_back(std::move(review.value()));
    }
    return core::Result<std::vector<PrefabRebaseReview>>::success(std::move(reviews));
}

} // namespace carto::condition
