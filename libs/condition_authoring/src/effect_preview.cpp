#include <carto/condition/effect_preview.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <limits>
#include <sstream>
#include <span>
#include <utility>

namespace carto::condition {
namespace {

core::Diagnostic invalid(std::string message) {
    return core::Diagnostic(core::ErrorCode::validation_failed, std::move(message));
}

std::uint64_t mix64(std::uint64_t value) noexcept {
    value += 0x9e3779b97f4a7c15ULL;
    value = (value ^ (value >> 30U)) * 0xbf58476d1ce4e5b9ULL;
    value = (value ^ (value >> 27U)) * 0x94d049bb133111ebULL;
    return value ^ (value >> 31U);
}

double unit_sample(std::uint64_t seed, std::uint64_t stream) noexcept {
    constexpr double kUnitScale = 1.0 / 9007199254740992.0;
    return static_cast<double>(mix64(seed ^ mix64(stream)) >> 11U) * kUnitScale;
}

core::Vec3d sample_volume(const EffectSource& source, std::uint64_t seed) noexcept {
    if (source.shape == EffectEmissionShape::box) {
        return {
            (unit_sample(seed, 1U) * 2.0 - 1.0) * source.half_extents.x,
            (unit_sample(seed, 2U) * 2.0 - 1.0) * source.half_extents.y,
            (unit_sample(seed, 3U) * 2.0 - 1.0) * source.half_extents.z,
        };
    }
    if (source.shape == EffectEmissionShape::sphere) {
        const double z = unit_sample(seed, 1U) * 2.0 - 1.0;
        const double angle = unit_sample(seed, 2U) * 6.28318530717958647692;
        const double planar = std::sqrt(std::max(0.0, 1.0 - z * z));
        const double radius = source.radius * std::cbrt(unit_sample(seed, 3U));
        return {radius * planar * std::cos(angle), radius * z,
            radius * planar * std::sin(angle)};
    }
    return {};
}

struct SpawnEvent {
    double time = 0.0;
    std::uint32_t ordinal = 0U;
    std::uint64_t random_stream = 0U;
};

core::Result<EffectPreviewResult> fail(core::Diagnostic diagnostic) {
    return core::Result<EffectPreviewResult>::failure(std::move(diagnostic));
}

} // namespace

core::Result<EffectPreviewResult> preview_effect(
    const AuthoringDocument& authoring,
    const std::map<std::string, spatial_field::ScalarField2D>& fields,
    Id effect_source_id,
    std::string_view operating_state,
    double elapsed_seconds) {
    if (auto valid = authoring.validate(); !valid) {
        return fail(valid.error().with_context("effect preview authoring"));
    }
    if (effect_source_id == 0U || !std::isfinite(elapsed_seconds) ||
        elapsed_seconds < 0.0 || elapsed_seconds > 600.0 ||
        operating_state.empty() || operating_state.size() > 128U) {
        return fail(invalid("effect preview identity, state, or time is invalid"));
    }

    const auto source_it = std::find_if(authoring.effect_sources.begin(),
        authoring.effect_sources.end(), [effect_source_id](const EffectSource& item) {
            return item.id == effect_source_id;
        });
    if (source_it == authoring.effect_sources.end()) {
        return fail(core::Diagnostic(core::ErrorCode::not_found,
            "effect preview source is not present in the CONDITION document"));
    }
    const bool normal_state = operating_state == source_it->normal_operating_state;
    const bool disabled_state = operating_state == source_it->disabled_operating_state;
    if (!normal_state && !disabled_state) {
        return fail(invalid("effect preview requires an exact authored operating state"));
    }

    EffectPreviewResult result;
    std::vector<std::pair<const FieldInfluenceBinding*, const spatial_field::ScalarField2D*>>
        resolved_fields;
    for (const auto& binding : authoring.field_influences) {
        if (binding.effect_source_id != effect_source_id) continue;
        const auto found = fields.find(binding.field_identity);
        if (found == fields.end()) {
            return fail(core::Diagnostic(core::ErrorCode::not_found,
                "effect preview field binding is not present in this project: " +
                binding.field_identity));
        }
        if (found->second.identity() != binding.field_identity ||
            found->second.source_revision() != binding.expected_field_revision) {
            return fail(core::Diagnostic(core::ErrorCode::stale_data,
                "effect preview field identity or source revision is stale: " +
                binding.field_identity));
        }
        const auto field_digest = found->second.canonical_digest();
        if (!field_digest || field_digest.value() != binding.expected_field_digest) {
            return fail(core::Diagnostic(core::ErrorCode::stale_data,
                "effect preview field digest is stale: " + binding.field_identity));
        }
        resolved_fields.emplace_back(&binding, &found->second);
        result.field_receipts.push_back({found->second.identity(),
            found->second.source_revision(), field_digest.value()});
    }

    result.effect_source_id = source_it->id;
    result.asset_id = source_it->asset_id;
    result.operating_state = std::string(operating_state);
    result.authoring_revision = authoring.project_revision;
    const auto authoring_digest = authoring.canonical_digest();
    if (!authoring_digest) return fail(authoring_digest.error());
    result.authoring_digest = authoring_digest.value();

    if (normal_state && source_it->enabled) {
        for (const auto& emitter : authoring.particle_emitters) {
            if (emitter.effect_source_id != effect_source_id || !emitter.enabled) continue;
            const double quantized_time = std::floor(
                elapsed_seconds / emitter.fixed_timestep_seconds) *
                emitter.fixed_timestep_seconds;
            const double active_end = emitter.start_seconds + emitter.duration_seconds;
            const double last_event_time = std::min(quantized_time, active_end);
            std::vector<SpawnEvent> events;
            events.reserve(static_cast<std::size_t>(emitter.total_particle_bound) * 2U);

            if (emitter.emission_rate_per_second > 0.0 &&
                emitter.duration_seconds > 0.0 && last_event_time >= emitter.start_seconds) {
                for (std::uint32_t ordinal = 0U;
                     ordinal < emitter.total_particle_bound; ++ordinal) {
                    const double event_time = emitter.start_seconds +
                        static_cast<double>(ordinal) / emitter.emission_rate_per_second;
                    if (event_time > last_event_time) break;
                    events.push_back({event_time, ordinal,
                        static_cast<std::uint64_t>(ordinal) * 2U});
                }
            }
            if (emitter.burst_count > 0U && emitter.duration_seconds > 0.0 &&
                emitter.burst_time_seconds >= emitter.start_seconds &&
                emitter.burst_time_seconds <= last_event_time) {
                for (std::uint32_t ordinal = 0U;
                     ordinal < emitter.burst_count && ordinal < emitter.total_particle_bound;
                     ++ordinal) {
                    events.push_back({emitter.burst_time_seconds, ordinal,
                        (static_cast<std::uint64_t>(ordinal) * 2U) + 1U});
                }
            }
            std::stable_sort(events.begin(), events.end(), [](const auto& left, const auto& right) {
                if (left.time != right.time) return left.time < right.time;
                return left.random_stream < right.random_stream;
            });
            if (events.size() > emitter.total_particle_bound) {
                events.resize(emitter.total_particle_bound);
            }

            std::uint32_t preview_ordinal = 0U;
            for (const auto& event : events) {
                const double age = quantized_time - event.time;
                if (age < 0.0 || age >= emitter.particle_lifetime_seconds) continue;
                const std::uint64_t seed = mix64(emitter.deterministic_seed ^
                    mix64(static_cast<std::uint64_t>(emitter.id)) ^
                    mix64(event.random_stream));
                const auto local_offset = sample_volume(*source_it, seed);
                const auto scaled_offset = core::componentwise_multiply(
                    local_offset, source_it->local_transform.scale);
                const auto origin = source_it->local_transform.translation +
                    source_it->local_transform.rotation.rotate(scaled_offset);
                auto velocity = emitter.initial_velocity + core::Vec3d{
                    (unit_sample(seed, 10U) * 2.0 - 1.0) * emitter.velocity_spread.x,
                    (unit_sample(seed, 11U) * 2.0 - 1.0) * emitter.velocity_spread.y,
                    (unit_sample(seed, 12U) * 2.0 - 1.0) * emitter.velocity_spread.z};
                velocity = source_it->local_transform.rotation.rotate(velocity);
                auto acceleration = source_it->local_transform.rotation.rotate(
                    emitter.acceleration);
                for (const auto& [binding, field] : resolved_fields) {
                    const auto sample = field->sample_bilinear({origin.x, origin.z});
                    if (!sample) {
                        return fail(sample.error().with_context(
                            "effect preview field sample " + binding->field_identity));
                    }
                    core::Vec3d force = binding->direction *
                        (sample.value() * binding->strength);
                    if (binding->kind == EffectInfluenceKind::avoidance) {
                        force = binding->direction *
                            (-std::abs(sample.value()) * binding->strength);
                    } else if (binding->kind == EffectInfluenceKind::turbulence) {
                        const double amplitude = std::abs(sample.value()) * binding->strength;
                        const std::uint64_t turbulence_seed = mix64(seed ^ binding->id);
                        force = {
                            (unit_sample(turbulence_seed, 21U) * 2.0 - 1.0) * amplitude,
                            (unit_sample(turbulence_seed, 22U) * 2.0 - 1.0) * amplitude,
                            (unit_sample(turbulence_seed, 23U) * 2.0 - 1.0) * amplitude,
                        };
                    }
                    acceleration = acceleration + source_it->local_transform.rotation.rotate(force);
                }
                const auto position = origin + velocity * age + acceleration * (0.5 * age * age);
                const auto current_velocity = velocity + acceleration * age;
                if (!position.finite() || !current_velocity.finite()) {
                    return fail(invalid("effect preview produced a non-finite particle sample"));
                }
                result.particles.push_back({emitter.id, preview_ordinal++, position,
                    current_velocity, age, emitter.particle_lifetime_seconds});
            }
        }
    }

    std::ostringstream canonical;
    canonical << "CONDITION_EFFECT_PREVIEW 1\n" << result.effect_source_id << ' '
        << result.asset_id << ' ' << std::quoted(result.operating_state) << ' '
        << result.authoring_revision.value() << ' ' << result.authoring_digest.hex() << '\n'
        << result.field_receipts.size() << '\n';
    for (const auto& receipt : result.field_receipts) {
        canonical << std::quoted(receipt.identity) << ' ' << receipt.revision.value() << ' '
            << receipt.digest.hex() << '\n';
    }
    canonical << std::setprecision(17) << result.particles.size() << '\n';
    for (const auto& particle : result.particles) {
        canonical << particle.emitter_id << ' ' << particle.ordinal << ' '
            << particle.position.x << ' ' << particle.position.y << ' '
            << particle.position.z << ' ' << particle.velocity.x << ' '
            << particle.velocity.y << ' ' << particle.velocity.z << ' '
            << particle.age_seconds << ' ' << particle.lifetime_seconds << '\n';
    }
    const auto& bytes = canonical.str();
    result.preview_digest = assets::sha256(std::span<const std::uint8_t>{
        reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size()});
    return core::Result<EffectPreviewResult>::success(std::move(result));
}

} // namespace carto::condition
