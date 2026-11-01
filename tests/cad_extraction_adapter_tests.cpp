#include <carto/engineering/cad_extraction_adapter.hpp>

#include <algorithm>
#include <functional>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>

#define CHECK(condition)                                                                           \
    do {                                                                                           \
        if (!(condition))                                                                          \
            throw std::runtime_error(#condition);                                                  \
    } while (false)
using namespace carto;
using namespace carto::engineering;

assets::Sha256Digest hash(std::string_view text) {
    return assets::sha256(std::span<const std::uint8_t>(
        reinterpret_cast<const std::uint8_t *>(text.data()), text.size()));
}

// Synthetic provider output exercises adapter wiring only. This is not a STEP
// reader and the source string is not presented as real geometry extraction.
struct FixtureBackend final : CadExtractionBackend {
    mutable unsigned calls = 0U;
    mutable std::string provider = "fixture.kernel";
    mutable std::string observed_settings;
    bool throw_on_extract = false;
    bool change_provider = false;
    bool change_provider_during_version = false;
    std::string *caller_buffer = nullptr;
    std::function<void(CadKernelExtraction &)> mutate;
    std::string_view identity() const noexcept override { return provider; }
    std::string_view version() const noexcept override {
        if (change_provider_during_version)
            provider = "changed.provider";
        return "fixture-v1";
    }
    core::Result<CadKernelExtraction>
    extract_step(std::string_view bytes, const CadExtractionContext &context,
                 const CadExtractionLimits &limits) const override {
        ++calls;
        if (throw_on_extract)
            throw std::runtime_error("synthetic backend error");
        CHECK(limits.max_settings_bytes == 65536U && limits.max_faces == 4096U &&
              limits.max_display_triangles == 65536U &&
              limits.max_correspondence_text_bytes == 4U * 1024U * 1024U);
        if (caller_buffer)
            (*caller_buffer)[0] = 'X';
        if (change_provider)
            provider = "changed.provider";
        observed_settings = context.import_settings_bytes;
        CadKernelExtraction out;
        out.source_digest = hash(bytes);
        out.settings_digest = hash(context.import_settings_bytes);
        out.units = "millimeters";
        out.coordinate_units = "millimeters";
        out.source_units_known = true;
        out.unchanged_source_identity_stable = true;
        out.unsupported_geometry = false;
        out.unresolved_external_references = false;
        out.parts.push_back(
            {10U, "part-A", "Synthetic part", "A", "", std::nullopt, context.source_id});
        // Child precedes its higher-ID parent to exercise visited-parent handling.
        out.occurrences.push_back(
            {1U, "occ-child", 10U, 2U, core::Transform::identity(), 1U, context.source_id});
        out.occurrences.push_back({2U, "occ-root", 10U, std::nullopt, core::Transform::identity(),
                                   1U, context.source_id});
        out.topology.push_back({20U, context.source_id, 10U, std::nullopt, TopologyKind::face,
                                "face-A", context.revision, 100U, 0U});
        out.correspondence.source_namespace = context.source_namespace;
        out.correspondence.source_revision = context.revision;
        out.correspondence.source_digest = out.source_digest;
        TopologyIdentityKey key{context.source_namespace, "part-A", TopologyKind::face, "face-A"};
        const auto id = persistent_topology_id(key);
        CHECK(id);
        out.correspondence.bindings.push_back(
            {key, id.value(), 100U, core::Revision{1U}, hash("mesh-evidence"), 0U});
        out.correspondence.bindings.push_back(
            {key, id.value(), 100U, core::Revision{1U}, hash("mesh-evidence"), 1U});
        if (mutate)
            mutate(out);
        return core::Result<CadKernelExtraction>::success(std::move(out));
    }
};

int main() {
    try {
        const std::string source =
            "ISO-10303-21; HEADER; FILE_SCHEMA(('AP242')); ENDSEC; DATA; ENDSEC; END-ISO-10303-21;";
        CadExtractionContext context{
            "fixture-project", "fixture-source", 7U,
            "fixture.step",    "rev-A",          hash("settings"),
            "settings",        "run-1",          CadImportMode::first_import};
        FixtureBackend backend;
        auto candidate = extract_step_with_backend(backend, context, source);
        CHECK(candidate);
        CHECK(backend.calls == 1U && !candidate.value().display_mesh_content_verified);
        CHECK(backend.observed_settings == "settings");
        const auto &document = candidate.value().authoring;
        CHECK(document.sources.size() == 1U && document.sources[0].digest == hash(source));
        CHECK(document.sources[0].provider_identity == "fixture.kernel" &&
              document.sources[0].units == "millimeters");
        CHECK(document.sources[0].lossy && document.sources[0].diagnostics.size() >= 3U);
        CHECK(std::any_of(document.sources[0].diagnostics.begin(),
                          document.sources[0].diagnostics.end(), [](const std::string &item) {
                              return item.find("Declared STEP schema profile: AP242") !=
                                     std::string::npos;
                          }));
        auto complete_fixture = make_dv012_fixture();
        CHECK(complete_fixture);
        auto package_document = document;
        package_document.workcell = complete_fixture.value().workcell;
        const auto package =
            InspectionPackage::publish(package_document, package_document.project_revision,
                                       package_document.project_namespace);
        CHECK(package);
        const auto consumer_admission = admit_for_consumer(package.value());
        CHECK(consumer_admission);
        CHECK(consumer_admission.value().status == PackageAdmissionStatus::refuse);
        CHECK(std::any_of(consumer_admission.value().reasons.begin(),
                          consumer_admission.value().reasons.end(), [](const std::string &item) {
                              return item.find("declares lossy import") != std::string::npos;
                          }));
        CHECK(document.parts.size() == 1U && document.occurrences.size() == 2U &&
              document.topology.size() == 1U);
        CHECK(document.occurrences[0].parent_id == 2U);
        CHECK(document.topology[0].source_revision == "rev-A");
        const auto replay = AuthoringDocument::deserialize(document.serialize());
        CHECK(replay);
        CHECK(replay.value().serialize() == document.serialize());
        auto selected = candidate.value().correspondence.resolve_display_triangle(
            "rev-A", 100U, core::Revision{1U}, hash("mesh-evidence"), 1U);
        CHECK(selected);
        CHECK(!candidate.value().correspondence.resolve_display_triangle(
            "wrong-revision", 100U, core::Revision{1U}, hash("mesh-evidence"), 1U));
        auto reimport = context;
        reimport.mode = CadImportMode::reimport;
        const auto same_map =
            extract_step_with_backend(backend, reimport, source, &candidate.value().correspondence);
        CHECK(!same_map && same_map.error().code == core::ErrorCode::unsupported);
        const auto changed_source = extract_step_with_backend(backend, reimport, source + " ",
                                                              &candidate.value().correspondence);
        CHECK(!changed_source && changed_source.error().code == core::ErrorCode::stale_data);
        CHECK(!extract_step_with_backend(backend, reimport, source));
        CHECK(!extract_step_with_backend(backend, context, source,
                                         &candidate.value().correspondence));
        auto invalid_context = context;
        invalid_context.source_namespace = "bad\nnamespace";
        const auto calls_before = backend.calls;
        CHECK(!extract_step_with_backend(backend, invalid_context, source));
        CHECK(backend.calls == calls_before);
        auto mismatched_settings = context;
        mismatched_settings.import_settings_bytes = "different settings";
        const auto mismatched = extract_step_with_backend(backend, mismatched_settings, source);
        CHECK(!mismatched &&
              mismatched.error().message.find(
                  "CAD settings bytes do not match the requested digest") != std::string::npos);
        CHECK(backend.calls == calls_before);
        auto binary_settings = context;
        binary_settings.import_settings_bytes = std::string("s\0x", 3U);
        binary_settings.import_settings_digest = hash(binary_settings.import_settings_bytes);
        FixtureBackend binary_backend;
        CHECK(extract_step_with_backend(binary_backend, binary_settings, source));
        CHECK(binary_backend.observed_settings == binary_settings.import_settings_bytes);
        auto maximum_settings = context;
        maximum_settings.import_settings_bytes = std::string(65536U, 's');
        maximum_settings.import_settings_digest = hash(maximum_settings.import_settings_bytes);
        FixtureBackend bounded_backend;
        CHECK(extract_step_with_backend(bounded_backend, maximum_settings, source));
        CHECK(bounded_backend.observed_settings == maximum_settings.import_settings_bytes);
        maximum_settings.import_settings_bytes.push_back('s');
        maximum_settings.import_settings_digest = hash(maximum_settings.import_settings_bytes);
        const auto above_limit =
            extract_step_with_backend(bounded_backend, maximum_settings, source);
        CHECK(!above_limit && above_limit.error().code == core::ErrorCode::validation_failed);
        CHECK(bounded_backend.calls == 1U);
        CHECK(!extract_step_with_backend(backend, context, "not a STEP file"));
        CHECK(backend.calls == calls_before);
        FixtureBackend throwing;
        throwing.throw_on_extract = true;
        CHECK(!extract_step_with_backend(throwing, context, source));
        FixtureBackend changing;
        changing.change_provider = true;
        CHECK(!extract_step_with_backend(changing, context, source));
        FixtureBackend changing_during_version;
        changing_during_version.change_provider_during_version = true;
        CHECK(!extract_step_with_backend(changing_during_version, context, source));
        CHECK(changing_during_version.calls == 0U);
        std::string mutable_source = source;
        FixtureBackend aliased;
        aliased.caller_buffer = &mutable_source;
        auto safe = extract_step_with_backend(aliased, context, mutable_source);
        CHECK(safe);
        CHECK(mutable_source != source && safe.value().authoring.sources[0].digest == hash(source));

        auto refused = [&](std::function<void(CadKernelExtraction &)> mutation) {
            FixtureBackend invalid;
            invalid.mutate = std::move(mutation);
            CHECK(!extract_step_with_backend(invalid, context, source));
        };
        refused([](auto &value) { value.source_digest = hash("different source"); });
        refused([](auto &value) { value.settings_digest = hash("different settings"); });
        refused([](auto &value) { value.units.clear(); });
        refused([](auto &value) { value.coordinate_units = "inches"; });
        refused([](auto &value) { value.unchanged_source_identity_stable = false; });
        refused([](auto &value) { value.unsupported_geometry = true; });
        refused([](auto &value) { value.unresolved_external_references = true; });
        refused([](auto &value) { value.parts.push_back(value.parts.front()); });
        refused([](auto &value) { value.occurrences[1].parent_id = 1U; });
        refused([](auto &value) { value.occurrences[0].parent_id = 999U; });
        refused([](auto &value) { value.occurrences[0].transform.scale.x = -1.0; });
        refused([](auto &value) { value.occurrences[0].transform.rotation.w = 2.0; });
        refused([](auto &value) { value.topology[0].source_revision = "wrong-revision"; });
        refused([](auto &value) { value.topology[0].occurrence_id = 1U; });
        refused([](auto &value) {
            value.correspondence.bindings[1].display_mesh_digest = hash("other-mesh");
        });
        refused([](auto &value) {
            value.correspondence.bindings[1].display_mesh_revision = core::Revision{2U};
        });
        refused([](auto &value) { value.correspondence.bindings[1].triangle = 0U; });
        refused([](auto &value) { value.correspondence.bindings[0].triangle = 2U; });
        refused([](auto &value) { value.parts[0].name.clear(); });
        FixtureBackend oversized;
        oversized.mutate = [](auto &value) {
            auto large = value.correspondence.bindings.front();
            large.topology.part_external_id = std::string(4096U, 'p');
            large.topology.topology_external_id = std::string(4096U, 'f');
            for (std::size_t i = 0; i < 540U; ++i)
                value.correspondence.bindings.push_back(large);
        };
        const auto oversized_result = extract_step_with_backend(oversized, context, source);
        CHECK(!oversized_result &&
              oversized_result.error().message.find(
                  "CAD correspondence text exceeds adapter bounds") != std::string::npos);
        FixtureBackend drift;
        drift.mutate = [](auto &value) { value.occurrences[0].transform.translation.x = 1.0; };
        const auto drift_result =
            extract_step_with_backend(drift, reimport, source, &candidate.value().correspondence);
        CHECK(!drift_result && drift_result.error().code == core::ErrorCode::unsupported);
        FixtureBackend added;
        added.mutate = [](auto &value) {
            auto face = value.topology[0];
            face.id = 21U;
            face.external_id = "face-B";
            face.display_triangle = 2U;
            value.topology.push_back(face);
            auto binding = value.correspondence.bindings[0];
            binding.topology.topology_external_id = "face-B";
            const auto id = persistent_topology_id(binding.topology);
            CHECK(id);
            binding.persistent_id = id.value();
            binding.triangle = 2U;
            value.correspondence.bindings.push_back(binding);
        };
        const auto added_identity =
            extract_step_with_backend(added, reimport, source, &candidate.value().correspondence);
        CHECK(!added_identity && added_identity.error().code == core::ErrorCode::stale_data);
        std::cout << "CAD extraction adapter fixture tests passed (synthetic backend only)\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
