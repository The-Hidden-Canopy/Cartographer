#include <carto/engineering/cad_extraction_adapter.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <tuple>
#include <utility>

namespace carto::engineering {
namespace {
constexpr std::size_t kMaxRecords = 4096U;
constexpr std::size_t kMaxTriangles = 65536U;
constexpr std::size_t kMaxCorrespondenceTextBytes = 4U * 1024U * 1024U;
constexpr std::size_t kMaxSettingsBytes = 65536U;
core::Diagnostic invalid(std::string message) {
    return {core::ErrorCode::validation_failed, std::move(message)};
}
core::Diagnostic unsupported(std::string message) {
    return {core::ErrorCode::unsupported, std::move(message)};
}
bool safe_text(std::string_view value, std::size_t maximum = 4096U) {
    return !value.empty() && value.size() <= maximum &&
           std::all_of(value.begin(), value.end(),
                       [](unsigned char c) { return c >= 0x20U && c != 0x7fU; });
}
bool bounded_correspondence_text(const TopologyCorrespondenceMap &map) {
    std::size_t bytes = 0U;
    const auto add = [&bytes](std::string_view value) {
        if (value.size() > kMaxCorrespondenceTextBytes - bytes)
            return false;
        bytes += value.size();
        return true;
    };
    if (!add(map.source_namespace) || !add(map.source_revision))
        return false;
    for (const auto &binding : map.bindings) {
        if (!add(binding.topology.source_namespace) || !add(binding.topology.part_external_id) ||
            !add(binding.topology.topology_external_id))
            return false;
    }
    return true;
}
bool rigid(const core::Transform &transform) {
    if (!transform.finite())
        return false;
    const auto &s = transform.scale;
    const auto &q = transform.rotation;
    const double norm = std::hypot(std::hypot(q.x, q.y), std::hypot(q.z, q.w));
    constexpr double tolerance = 1e-9;
    return std::isfinite(norm) && std::abs(norm - 1.0) <= tolerance &&
           std::abs(s.x - 1.0) <= tolerance && std::abs(s.y - 1.0) <= tolerance &&
           std::abs(s.z - 1.0) <= tolerance;
}
using FaceKey = std::pair<Id, std::string>;
using DisplayKey = std::pair<std::uint64_t, std::uint32_t>;
} // namespace

core::Result<CadExtractionCandidate>
extract_step_with_backend(const CadExtractionBackend &backend, const CadExtractionContext &context,
                          std::string_view source_bytes,
                          const TopologyCorrespondenceMap *previous) {
    using Result = core::Result<CadExtractionCandidate>;
    auto fail = [](std::string message) { return Result::failure(invalid(std::move(message))); };
    const auto identity_view = backend.identity();
    if (!safe_text(identity_view))
        return fail("CAD backend identity is invalid");
    const std::string provider_identity(identity_view);
    const auto version_view = backend.version();
    if (!safe_text(version_view))
        return fail("CAD backend version is invalid");
    const std::string provider_version(version_view);
    if (backend.identity() != provider_identity)
        return fail("CAD backend identity changed while reading its version");
    if (source_bytes.empty() || source_bytes.size() > kMaxEngineeringEvidenceBytes ||
        context.source_id == 0U || !safe_text(context.project_namespace) ||
        !safe_text(context.source_namespace, 256U) || !safe_text(context.locator) ||
        !safe_text(context.revision, 256U) || !safe_text(context.import_run_id) ||
        context.import_settings_digest.is_zero() || context.import_settings_bytes.empty() ||
        context.import_settings_bytes.size() > kMaxSettingsBytes ||
        (context.mode != CadImportMode::first_import && context.mode != CadImportMode::reimport) ||
        (context.mode == CadImportMode::first_import && previous != nullptr) ||
        (context.mode == CadImportMode::reimport && previous == nullptr))
        return fail("CAD extraction context, source or backend identity is invalid");
    const auto settings_bytes = std::span<const std::uint8_t>(
        reinterpret_cast<const std::uint8_t *>(context.import_settings_bytes.data()),
        context.import_settings_bytes.size());
    if (assets::sha256(settings_bytes) != context.import_settings_digest)
        return fail("CAD settings bytes do not match the requested digest");
    const CadExtractionContext request = context;
    const std::string source_snapshot(source_bytes);
    const std::string_view snapshot_view(source_snapshot);
    std::optional<TopologyCorrespondenceMap> previous_snapshot;
    if (previous) {
        if (previous->bindings.size() > kMaxTriangles || !bounded_correspondence_text(*previous))
            return fail("CAD previous correspondence exceeds adapter bounds");
        if (auto valid = previous->validate(); !valid)
            return Result::failure(valid.error().with_context("previous CAD correspondence"));
        previous_snapshot = *previous;
    }
    StepAp242Provider header_probe;
    auto probe = header_probe.probe(snapshot_view);
    if (!probe)
        return Result::failure(probe.error());
    std::string profile = probe.value().schema_profile;
    std::transform(profile.begin(), profile.end(), profile.begin(),
                   [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    if (!probe.value().recognized || !profile.starts_with("AP242"))
        return Result::failure(
            unsupported("CAD backend adapter accepts a bounded AP242 header only"));
    const auto source_digest = assets::sha256(std::span<const std::uint8_t>(
        reinterpret_cast<const std::uint8_t *>(snapshot_view.data()), snapshot_view.size()));
    const CadExtractionLimits limits;
    auto extracted = [&]() -> core::Result<CadKernelExtraction> {
        try {
            return backend.extract_step(snapshot_view, request, limits);
        } catch (...) {
            return core::Result<CadKernelExtraction>::failure(
                invalid("CAD extraction backend threw an exception"));
        }
    }();
    if (!extracted)
        return Result::failure(extracted.error().with_context("CAD extraction backend"));
    if (assets::sha256(std::span<const std::uint8_t>(
            reinterpret_cast<const std::uint8_t *>(snapshot_view.data()), snapshot_view.size())) !=
            source_digest ||
        backend.identity() != provider_identity || backend.version() != provider_version)
        return fail("CAD backend mutated source snapshot or provider identity during extraction");
    auto value = std::move(extracted.value());
    if (value.source_digest != source_digest ||
        value.settings_digest != request.import_settings_digest)
        return fail("CAD backend output does not match the requested source and settings digests");
    if (!value.source_units_known ||
        (value.units != "millimeters" && value.units != "meters" && value.units != "inches") ||
        value.coordinate_units != value.units || !value.unchanged_source_identity_stable ||
        value.unsupported_geometry || value.unresolved_external_references)
        return Result::failure(
            unsupported("CAD backend did not establish units, stable identities, supported "
                        "geometry and resolved references"));
    if (value.parts.empty() || value.parts.size() > kMaxRecords || value.occurrences.empty() ||
        value.occurrences.size() > kMaxRecords || value.topology.empty() ||
        value.topology.size() > kMaxRecords || value.correspondence.bindings.empty() ||
        value.correspondence.bindings.size() > kMaxTriangles || value.diagnostics.size() > 253U)
        return fail("CAD backend output is empty or exceeds adapter bounds");
    if (!bounded_correspondence_text(value.correspondence))
        return fail("CAD correspondence text exceeds adapter bounds");
    if (value.correspondence.source_namespace != request.source_namespace ||
        value.correspondence.source_revision != request.revision ||
        value.correspondence.source_digest != source_digest)
        return fail("CAD correspondence source identity does not match the request");
    if (auto valid = value.correspondence.validate(); !valid)
        return Result::failure(valid.error().with_context("CAD topology correspondence"));

    std::map<Id, const EngineeringPart *> parts;
    std::set<std::string> part_external;
    for (const auto &part : value.parts) {
        if (auto valid = part.validate(); !valid)
            return Result::failure(valid.error());
        if (part.source_id != request.source_id || !parts.emplace(part.id, &part).second ||
            !part_external.insert(part.external_id).second)
            return fail("CAD part source or external identity is duplicated or mismatched");
    }
    std::map<Id, const EngineeringOccurrence *> occurrences;
    std::set<std::string> occurrence_external;
    for (const auto &occurrence : value.occurrences) {
        if (auto valid = occurrence.validate(); !valid)
            return Result::failure(valid.error());
        if (occurrence.source_id != request.source_id || !rigid(occurrence.transform) ||
            !parts.contains(occurrence.part_id) ||
            !occurrences.emplace(occurrence.id, &occurrence).second ||
            !occurrence_external.insert(occurrence.external_id).second)
            return fail("CAD occurrence source, part or external identity is invalid");
    }
    std::map<Id, unsigned> color;
    for (const auto &[id, ignored] : occurrences) {
        static_cast<void>(ignored);
        std::vector<Id> path;
        Id cursor = id;
        while (true) {
            if (color[cursor] == 1U)
                return fail("CAD occurrence hierarchy contains a cycle");
            if (color[cursor] == 2U)
                break;
            color[cursor] = 1U;
            path.push_back(cursor);
            const auto current = occurrences.find(cursor);
            if (current == occurrences.end())
                return fail("CAD occurrence parent is unresolved");
            if (!current->second->parent_id)
                break;
            cursor = *current->second->parent_id;
            if (!occurrences.contains(cursor))
                return fail("CAD occurrence parent is unresolved");
        }
        for (const auto path_id : path)
            color[path_id] = 2U;
    }
    std::map<FaceKey, const TopologyBinding *> faces;
    for (const auto &face : value.topology) {
        if (auto valid = face.validate(); !valid)
            return Result::failure(valid.error());
        if (face.source_id != request.source_id || face.source_revision != request.revision ||
            face.kind != TopologyKind::face || face.occurrence_id.has_value() ||
            !parts.contains(face.part_id) ||
            !faces.emplace(FaceKey{face.part_id, face.external_id}, &face).second)
            return fail("CAD face is foreign, duplicated, occurrence-specific or unsupported");
    }
    std::map<std::string, Id> part_by_external;
    for (const auto &[id, part] : parts)
        part_by_external.emplace(part->external_id, id);
    std::set<DisplayKey> used_display_triangles;
    std::map<std::uint64_t, std::pair<core::Revision, assets::Sha256Digest>> mesh_snapshots;
    std::set<FaceKey> represented;
    for (const auto &binding : value.correspondence.bindings) {
        const auto part = part_by_external.find(binding.topology.part_external_id);
        if (part == part_by_external.end() || binding.topology.kind != TopologyKind::face ||
            binding.display_mesh_revision.value() == 0U ||
            !used_display_triangles.emplace(binding.display_mesh_asset, binding.triangle).second)
            return fail("CAD display triangle is foreign, ambiguous or has no revision");
        const auto [mesh, inserted] = mesh_snapshots.emplace(
            binding.display_mesh_asset,
            std::make_pair(binding.display_mesh_revision, binding.display_mesh_digest));
        if (!inserted && (mesh->second.first != binding.display_mesh_revision ||
                          mesh->second.second != binding.display_mesh_digest))
            return fail("CAD display mesh has inconsistent revision or digest evidence");
        const FaceKey key{part->second, binding.topology.topology_external_id};
        if (!faces.contains(key))
            return fail("CAD display triangle references an unknown face");
        const auto *face = faces.at(key);
        if (face->display_mesh_asset == binding.display_mesh_asset &&
            face->display_triangle == binding.triangle)
            represented.insert(key);
    }
    if (represented.size() != faces.size())
        return fail("CAD face has no matching representative display binding");
    if (previous_snapshot) {
        const auto comparison = compare_topology_reimport(*previous_snapshot, value.correspondence);
        if (!comparison)
            return Result::failure(comparison.error().with_context("CAD reimport comparison"));
        if (comparison.value().publication_blocked || !comparison.value().added_identities.empty())
            return Result::failure(core::Diagnostic(
                core::ErrorCode::stale_data, "CAD reimport changed or ambiguous; preserve prior "
                                             "authoring and reconcile before publication"));
        return Result::failure(
            unsupported("CAD reimport requires full prior authoring identity reconciliation; "
                        "matching face correspondence alone is insufficient"));
    }
    value.diagnostics.push_back("Face-only extraction omits PMI and other topology; this candidate "
                                "is lossy and consumer admission must refuse it.");
    value.diagnostics.push_back("Display mesh bytes and triangle ownership are provider assertions "
                                "and are not verified by this adapter.");
    value.diagnostics.push_back("Declared STEP schema profile: " + probe.value().schema_profile);
    ExtractedAssemblyInput input;
    input.project_namespace = request.project_namespace;
    input.source_id = request.source_id;
    input.source_bytes = snapshot_view;
    input.locator = request.locator;
    input.revision = request.revision;
    input.provider_identity = provider_identity;
    input.provider_version = provider_version;
    input.import_settings_digest = request.import_settings_digest;
    input.import_run_id = request.import_run_id;
    input.units = std::move(value.units);
    input.lossy = true;
    input.diagnostics = std::move(value.diagnostics);
    input.parts = std::move(value.parts);
    input.occurrences = std::move(value.occurrences);
    input.topology = std::move(value.topology);
    auto authoring = admit_extracted_assembly(std::move(input));
    if (!authoring)
        return Result::failure(authoring.error());
    return Result::success(CadExtractionCandidate{std::move(authoring.value()),
                                                  std::move(value.correspondence), false});
}
} // namespace carto::engineering
