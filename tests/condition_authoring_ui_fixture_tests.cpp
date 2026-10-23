#include <carto/application/application.hpp>
#include <carto/assets/blob_store.hpp>
#include <carto/biological/authoring.hpp>
#include <carto/condition/authoring.hpp>
#include <carto/condition/wave_import.hpp>
#include <carto/geometry/sign.hpp>
#include <carto/production/sculpt_stack.hpp>
#include <carto/project/presentation_catalog.hpp>
#include <carto/project/project.hpp>
#include <carto/render/material.hpp>
#include <carto/spatial_field/scalar_field.hpp>
#include <carto/ui/ui.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace {

void require(bool condition, std::string_view message) {
    if (!condition) throw std::runtime_error(std::string(message));
}

void append_u16(std::vector<std::uint8_t>& bytes, std::uint16_t value) {
    bytes.push_back(static_cast<std::uint8_t>(value & 0xffU));
    bytes.push_back(static_cast<std::uint8_t>((value >> 8U) & 0xffU));
}

void append_u32(std::vector<std::uint8_t>& bytes, std::uint32_t value) {
    for (std::uint32_t shift = 0U; shift < 32U; shift += 8U) {
        bytes.push_back(static_cast<std::uint8_t>((value >> shift) & 0xffU));
    }
}

void append_tag(std::vector<std::uint8_t>& bytes, std::string_view tag) {
    bytes.insert(bytes.end(), tag.begin(), tag.end());
}

std::vector<std::uint8_t> make_synthetic_wave() {
    constexpr std::uint32_t kSampleRate = 48'000U;
    constexpr std::uint32_t kFrames = 48'000U;
    constexpr std::uint16_t kChannels = 1U;
    constexpr std::uint16_t kBitsPerSample = 16U;
    constexpr std::uint16_t kBlockAlign = kChannels * (kBitsPerSample / 8U);
    constexpr std::uint32_t kDataBytes = kFrames * kBlockAlign;

    std::vector<std::uint8_t> samples;
    samples.reserve(kDataBytes);
    for (std::uint32_t frame = 0U; frame < kFrames; ++frame) {
        // A deterministic, low-amplitude 200 Hz square-wave tone. No private or
        // user recording is read, copied, or embedded in the project artifact.
        const std::int16_t sample = ((frame / 120U) & 1U) == 0U ? 2'048 : -2'048;
        const auto bits = static_cast<std::uint16_t>(sample);
        append_u16(samples, bits);
    }

    std::vector<std::uint8_t> wave;
    wave.reserve(44U + samples.size());
    append_tag(wave, "RIFF");
    append_u32(wave, 36U + kDataBytes);
    append_tag(wave, "WAVEfmt ");
    append_u32(wave, 16U);
    append_u16(wave, 1U);
    append_u16(wave, kChannels);
    append_u32(wave, kSampleRate);
    append_u32(wave, kSampleRate * kBlockAlign);
    append_u16(wave, kBlockAlign);
    append_u16(wave, kBitsPerSample);
    append_tag(wave, "data");
    append_u32(wave, kDataBytes);
    wave.insert(wave.end(), samples.begin(), samples.end());
    return wave;
}

void write_bytes(const std::filesystem::path& path,
                 std::span<const std::uint8_t> bytes) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    require(output.good(), "could not create the generated acoustic QA WAVE clip");
    output.write(reinterpret_cast<const char*>(bytes.data()),
        static_cast<std::streamsize>(bytes.size()));
    require(output.good(), "could not write the generated acoustic QA WAVE clip");
}

carto::assets::Sha256Digest fixture_digest(std::string_view value) {
    return carto::assets::sha256(std::span<const std::uint8_t>{
        reinterpret_cast<const std::uint8_t*>(value.data()), value.size()});
}

void run_gas_station_effect_fixture(const std::filesystem::path& fixture_root) {
    using carto::condition::EffectEmissionShape;
    using carto::condition::EffectInfluenceKind;
    using carto::condition::EffectIntentClass;
    using carto::condition::FieldInfluenceBinding;
    using carto::condition::ParticleEmitterSource;
    using carto::spatial_field::FieldSemantic;

    const auto started = std::chrono::steady_clock::now();
    const auto project_path = fixture_root / "condition-gas-station-effects.carto";
    const auto stale_project_path =
        fixture_root / "condition-gas-station-effects-stale-field.carto";
    carto::application::ApplicationSession session;
    carto::ui::UiController ui(session);
    require(static_cast<bool>(ui.dispatch(
                carto::application::NewProjectAction{"CONDITION Gas Station effects"})) &&
            static_cast<bool>(ui.import_condition_fixture()),
        "could not create the source-owned Gas Station effect project");

    struct FieldSpec {
        std::string_view identity;
        FieldSemantic semantic;
        double initial_value;
    };
    const std::array<FieldSpec, 3U> field_specs{{
        {"gas_station.wind_u", FieldSemantic::wind_u, 0.25},
        {"gas_station.drainage", FieldSemantic::drainage_flow, 0.4},
        {"gas_station.wind_v", FieldSemantic::wind_v, -0.15},
    }};
    for (const auto& spec : field_specs) {
        require(static_cast<bool>(ui.dispatch(carto::application::CreateSpatialFieldAction{
                    std::string(spec.identity), spec.semantic, {-20.0, -20.0},
                    2.0, 21U, 21U, -1.0, 1.0, spec.initial_value})),
            "could not create a deterministic Gas Station effect field");
    }

    auto authoring_result = carto::condition::make_condition_fixture();
    require(authoring_result.has_value(),
        "could not create the Gas Station CONDITION authoring source");
    auto authoring = std::move(authoring_result).value();
    authoring.project_namespace = "condition.fixture.gas-station-effects";
    auto& source = authoring.sources.front();
    source.label = "Gas Station deterministic effect source";
    source.logical_locator = "fixture://condition/gas-station-effects";
    source.revision = "gas-station-effects-1";
    source.digest = fixture_digest("gas-station-effects-source-v1");
    source.import_settings_digest = fixture_digest("gas-station-effects-import-v1");
    for (auto& asset : authoring.assets) {
        asset.source_revision = source.revision;
        asset.source_digest = source.digest;
    }
    authoring.assets[0U].stable_name = "gas_station_compressor";
    authoring.assets[1U].stable_name = "gas_station_runoff_edge";
    authoring.assets[1U].asset_class = carto::condition::AssetClass::terrain;
    authoring.assets[2U].stable_name = "gas_station_service_vehicle";
    authoring.assets[2U].asset_class = carto::condition::AssetClass::vehicle;

    authoring.effect_sources = {
        {1U, 1U, "gas-station.compressor.steam-air", EffectEmissionShape::point,
            carto::core::Transform::identity(), {}, 0.0,
            EffectIntentClass::physical_state,
            "compressor.running", "compressor.off", true},
        {2U, 2U, "gas-station.runoff.splash", EffectEmissionShape::box,
            carto::core::Transform::identity(), {1.5, 0.15, 0.4}, 0.0,
            EffectIntentClass::physical_state,
            "rain.active", "rain.stopped", true},
        {3U, 3U, "gas-station.vehicle.smoke", EffectEmissionShape::sphere,
            carto::core::Transform::identity(), {}, 0.25,
            EffectIntentClass::physical_state,
            "engine.running", "engine.stopped", true},
    };
    authoring.particle_emitters = {
        ParticleEmitterSource{1U, 1U, 5.0, 2.0, 0.0, 4.0, 0U, 0.0, {}, {},
            {0.0, 0.7, 0.0}, 1.0 / 60.0, 64U, 501U, true},
        ParticleEmitterSource{2U, 2U, 12.0, 0.8, 0.0, 4.0, 8U, 0.1,
            {0.0, 0.6, 0.0}, {0.2, 0.1, 0.2}, {}, 1.0 / 60.0, 96U, 502U, true},
        ParticleEmitterSource{3U, 3U, 3.0, 2.5, 0.0, 4.0, 0U, 0.0,
            {0.0, 0.2, 0.0}, {0.1, 0.1, 0.1}, {0.0, 0.25, 0.0},
            1.0 / 60.0, 64U, 503U, true},
    };

    const auto current = ui.snapshot();
    const auto field_view = [&current](std::string_view identity)
        -> const carto::application::SpatialFieldView& {
        const auto field = std::find_if(current.spatial_fields.begin(),
            current.spatial_fields.end(), [identity](const auto& candidate) {
                return candidate.identity == identity;
            });
        if (field == current.spatial_fields.end()) {
            throw std::runtime_error("Gas Station field is missing from the application snapshot");
        }
        return *field;
    };
    const std::array<std::tuple<carto::condition::Id, EffectInfluenceKind,
        std::string_view, carto::core::Vec3d, double>, 3U> influence_specs{{
        {1U, EffectInfluenceKind::wind, "gas_station.wind_u", {1.0, 0.0, 0.0}, 0.35},
        {2U, EffectInfluenceKind::drainage, "gas_station.drainage", {0.0, -1.0, 0.0}, 0.2},
        {3U, EffectInfluenceKind::turbulence, "gas_station.wind_v", {0.0, 0.0, 1.0}, 0.1},
    }};
    authoring.field_influences.clear();
    for (std::size_t index = 0U; index < influence_specs.size(); ++index) {
        const auto& [effect_id, kind, identity, direction, strength] = influence_specs[index];
        const auto& field = field_view(identity);
        authoring.field_influences.push_back(FieldInfluenceBinding{
            static_cast<carto::condition::Id>(index + 1U), effect_id, kind,
            std::string(identity), field.source_revision, field.digest, direction, strength});
    }
    authoring.project_revision = current.condition.authored_revision.next();
    require(static_cast<bool>(authoring.validate()),
        "Gas Station effect sources do not validate against their source fields");
    require(static_cast<bool>(ui.dispatch(
                carto::application::ImportConditionAuthoringAction{authoring.serialize()})),
        "could not commit the Gas Station effect source document");

    const auto before_preview = ui.snapshot();
    const auto compressor = ui.preview_condition_effect(1U, "compressor.running", 2.0);
    const auto runoff = ui.preview_condition_effect(2U, "rain.active", 1.5);
    const auto smoke = ui.preview_condition_effect(3U, "engine.running", 2.0);
    require(compressor && runoff && smoke &&
            !compressor.value().particles.empty() && !runoff.value().particles.empty() &&
            !smoke.value().particles.empty() &&
            compressor.value().field_receipts.size() == 1U &&
            runoff.value().field_receipts.size() == 1U &&
            smoke.value().field_receipts.size() == 1U,
        "Gas Station effect preview did not resolve all exact field receipts");
    const auto compressor_repeat = ui.preview_condition_effect(
        1U, "compressor.running", 2.0);
    const auto runoff_repeat = ui.preview_condition_effect(2U, "rain.active", 1.5);
    const auto smoke_repeat = ui.preview_condition_effect(3U, "engine.running", 2.0);
    require(compressor_repeat && runoff_repeat && smoke_repeat &&
            compressor_repeat.value().preview_digest == compressor.value().preview_digest &&
            runoff_repeat.value().preview_digest == runoff.value().preview_digest &&
            smoke_repeat.value().preview_digest == smoke.value().preview_digest,
        "Gas Station effect previews are not deterministic for fixed source and field revisions");
    for (const auto& [effect_id, disabled_state] : std::array{
             std::pair<carto::condition::Id, std::string_view>{1U, "compressor.off"},
             std::pair<carto::condition::Id, std::string_view>{2U, "rain.stopped"},
             std::pair<carto::condition::Id, std::string_view>{3U, "engine.stopped"}}) {
        const auto disabled = ui.preview_condition_effect(
            effect_id, disabled_state, 2.0);
        require(disabled && disabled.value().particles.empty(),
            "disabled Gas Station effect state unexpectedly emitted preview particles");
    }
    const auto after_preview = ui.snapshot();
    require(after_preview.project_revision == before_preview.project_revision &&
            after_preview.condition.authored_revision == before_preview.condition.authored_revision &&
            after_preview.operations.size() == before_preview.operations.size(),
        "Gas Station effect preview mutated project or authoring history");

    require(static_cast<bool>(ui.dispatch(
                carto::application::SaveProjectAction{project_path})),
        "could not save the current Gas Station effect project");
    auto saved = carto::project::ProjectDocument::load(project_path);
    require(saved && saved.value().validate() && saved.value().condition_authoring().has_value(),
        "Gas Station effect project failed portable .carto validation");
    const auto saved_authoring = carto::condition::AuthoringDocument::deserialize(
        saved.value().condition_authoring()->bytes);
    require(saved_authoring && saved_authoring.value().serialize() == authoring.serialize() &&
            saved_authoring.value().effect_sources.size() == 3U &&
            saved_authoring.value().particle_emitters.size() == 3U &&
            saved_authoring.value().field_influences.size() == 3U,
        "Gas Station .carto lost exact source, emitter, or field-binding records");
    const std::string portable_bytes = saved.value().serialize();
    for (const auto forbidden : {"VANTA", "STRATA", "TrueForm", "REMENENCE",
             "MENAGERIE", "OWH", "HiddenCanopy", "runtime_handle", "worker_endpoint",
             "DONOR_REPOSITORY", "DONOR_COMMIT"}) {
        require(portable_bytes.find(forbidden) == std::string::npos,
            "Gas Station .carto serialized donor runtime identity or workstation coupling");
    }

    carto::application::ApplicationSession reopened_session;
    carto::ui::UiController reopened_ui(reopened_session);
    require(static_cast<bool>(reopened_ui.dispatch(
                carto::application::OpenProjectAction{project_path})),
        "native application could not reopen the Gas Station effect fixture");
    const auto same_field_receipts = [](const auto& left, const auto& right) {
        if (left.size() != right.size()) return false;
        for (std::size_t index = 0U; index < left.size(); ++index) {
            if (left[index].identity != right[index].identity ||
                left[index].revision != right[index].revision ||
                left[index].digest != right[index].digest) {
                return false;
            }
        }
        return true;
    };
    for (const auto& expected : {compressor.value(), runoff.value(), smoke.value()}) {
        const auto reopened = reopened_ui.preview_condition_effect(
            expected.effect_source_id, expected.operating_state,
            expected.effect_source_id == 2U ? 1.5 : 2.0);
        require(reopened && reopened.value().preview_digest == expected.preview_digest &&
                same_field_receipts(reopened.value().field_receipts,
                    expected.field_receipts),
            "Gas Station effect preview changed after .carto save/reopen");
    }

    auto wind = field_view("gas_station.wind_u");
    carto::spatial_field::FieldBrush brush;
    brush.expected_revision = wind.source_revision;
    brush.center_uv = {0.0, 0.0};
    brush.radius = 2.0;
    brush.value = 0.9;
    brush.strength = 1.0;
    brush.mode = carto::spatial_field::BrushMode::replace;
    require(static_cast<bool>(ui.dispatch(carto::application::PaintSpatialFieldAction{
                "gas_station.wind_u", brush})),
        "could not revise the Gas Station wind field for stale-source rejection");
    require(!ui.preview_condition_effect(1U, "compressor.running", 2.0),
        "Gas Station preview accepted an effect binding to a stale field revision");
    require(static_cast<bool>(ui.dispatch(
                carto::application::SaveProjectAction{stale_project_path})),
        "could not persist the stale-field Gas Station artifact");
    auto stale_saved = carto::project::ProjectDocument::load(stale_project_path);
    require(stale_saved && stale_saved.value().validate(),
        "stale-field Gas Station artifact failed structural project validation");
    carto::application::ApplicationSession stale_session;
    carto::ui::UiController stale_ui(stale_session);
    require(static_cast<bool>(stale_ui.dispatch(
                carto::application::OpenProjectAction{stale_project_path})) &&
            !stale_ui.preview_condition_effect(1U, "compressor.running", 2.0),
        "stale Gas Station field binding became valid after save/reopen");

    const auto stale_snapshot = ui.snapshot();
    require(static_cast<bool>(ui.dispatch(carto::application::UndoAction{})),
        "Gas Station field edit could not be undone through the project journal");
    require(!ui.preview_condition_effect(1U, "compressor.running", 2.0),
        "undo silently rewound the source-field revision and revived a stale effect binding");
    require(static_cast<bool>(ui.dispatch(carto::application::RedoAction{})) &&
            ui.snapshot().project_revision > stale_snapshot.project_revision &&
            !ui.preview_condition_effect(1U, "compressor.running", 2.0),
        "redo did not preserve monotonic revisions and fail-closed effect preview");

    const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started).count();
    std::cout << "gas_station_effect_fixture=" << project_path.string()
              << " stale_fixture=" << stale_project_path.string()
              << " sources=3 emitters=3 field_receipts=3 deterministic=true"
              << " read_only_preview=true stale_field_rejected=true"
              << " undo_redo=true private_runtime=false synthetic_source=true"
              << " fixture_elapsed_ms=" << elapsed_ms << '\n';
}

carto::biological::BiologicalAuthoringDocument biological_condition_source(
    std::uint64_t asset_id, std::string_view family, carto::biological::LayerKind covering) {
    using namespace carto::biological;
    BiologicalAuthoringDocument source;
    const std::string label{family};
    source.source = AuthoringSourceIdentity{
        "fixtures/cartographer-condition-biological", asset_id,
        carto::core::Revision{1U}, fixture_digest(label + ":source-v1")};
    source.organism_family = "generic." + label;
    require(static_cast<bool>(source.insert_structure(StructureRecord{
                .id = StructureId{1U},
                .kind = StructureKind::organism,
                .parent = std::nullopt,
                .semantic_type = "organism.proxy",
                .coordinate_frame = "organism.local",
                .kinematic_joint = std::nullopt,
                .kinematic_graph_digest = std::nullopt,
                .geometry_digest = fixture_digest(label + ":body-source"),
                .proxy_surface = EllipsoidProxyProfile{
                    .center = {0.0, 0.0, 0.0},
                    .radii = family == "human" ? carto::core::Vec3d{0.38, 0.9, 0.28}
                        : family == "avian" ? carto::core::Vec3d{0.3, 0.45, 0.55}
                        : carto::core::Vec3d{0.55, 0.4, 0.9},
                    .longitude_segments = 12U,
                    .latitude_segments = 6U,
                },
            })), "biological fixture body source is rejected");
    require(static_cast<bool>(source.insert_field(FieldRecord{
                .id = FieldId{1U},
                .kind = FieldKind::density,
                .value_kind = FieldValueKind::scalar,
                .representation = FieldRepresentation::statistical_shading,
                .domain = "organism.covering-density",
                .coordinate_frame = "organism.local",
                .units = "normalized",
                .sample_count = 32U,
                .stable_seed = asset_id + 1U,
                .coverage_field = std::nullopt,
                .data_digest = fixture_digest(label + ":covering-density"),
                .minimum = 0.1,
                .maximum = 0.9,
            })), "biological fixture covering field is rejected");
    require(static_cast<bool>(source.insert_layer(LayerRecord{
                .id = LayerId{1U},
                .kind = LayerKind::anatomy,
                .owner = StructureId{1U},
                .parent = std::nullopt,
                .order = 10U,
                .semantic_type = "anatomy.proxy",
                .fields = {FieldId{1U}},
            })) && static_cast<bool>(source.insert_layer(LayerRecord{
                .id = LayerId{2U},
                .kind = covering,
                .owner = StructureId{1U},
                .parent = LayerId{1U},
                .order = 20U,
                .semantic_type = "covering." + label,
                .fields = {FieldId{1U}},
            })), "biological fixture covering layer is rejected");
    require(static_cast<bool>(source.validate()), "biological fixture source is invalid");
    return source;
}

void run_biological_condition_fixture(const std::filesystem::path& fixture_root) {
    const auto fixture_started = std::chrono::steady_clock::now();
    const auto project_path = fixture_root / "condition-wildlife-human.carto";
    carto::application::ApplicationSession session;
    carto::ui::UiController ui(session);
    require(static_cast<bool>(ui.dispatch(carto::application::NewProjectAction{
                "CONDITION wildlife and human authoring fixture"})),
        "could not create the biological CONDITION fixture project");

    const std::array sources{
        biological_condition_source(9101U, "canid", carto::biological::LayerKind::strand),
        biological_condition_source(9102U, "human", carto::biological::LayerKind::strand),
        biological_condition_source(9103U, "avian", carto::biological::LayerKind::feather),
    };
    for (const auto& authored_source : sources) {
        auto before = session.snapshot();
        const carto::project::BiologicalAuthoringKey key{
            authored_source.source.source_namespace, authored_source.source.asset_id};
        require(static_cast<bool>(ui.dispatch(carto::application::SetBiologicalAuthoringAction{
                    before.project_revision, authored_source})),
            "native application rejected a biological source fixture");
        before = session.snapshot();
        const auto stored = session.biological_authoring_source(key, before.project_revision);
        require(stored && stored.value().source.revision == authored_source.source.revision,
            "biological source revision changed during native admission");
        const auto preview = session.biological_proxy_preview(
            key, before.project_revision, carto::biological::StructureId{1U});
        require(preview && preview.value().validate_against(stored.value()),
            "biological source did not produce an exact source-bound proxy preview");
        const auto record = std::find_if(before.biological_sources.begin(),
            before.biological_sources.end(), [&key](const auto& candidate) {
                return candidate.key == key;
            });
        require(record != before.biological_sources.end(),
            "native snapshot omitted an admitted biological source");
        const auto objects_before_materialization = before.objects.size();
        require(static_cast<bool>(ui.dispatch(
                    carto::application::CreateBiologicalProxyInstanceAction{
                        before.project_revision, key, authored_source.source.revision,
                        carto::biological::StructureId{1U}, record->payload_digest,
                        preview.value().product_digest})),
            "human-gated biological proxy materialization failed");
        require(session.snapshot().objects.size() == objects_before_materialization + 1U,
            "biological proxy command did not add exactly one scene instance");
        require(static_cast<bool>(ui.dispatch(carto::application::UndoAction{})) &&
                session.snapshot().objects.size() == objects_before_materialization &&
                session.snapshot().biological_sources.size() == before.biological_sources.size(),
            "proxy undo removed source truth or failed to remove its instance atomically");
        require(static_cast<bool>(ui.dispatch(carto::application::RedoAction{})) &&
                session.snapshot().objects.size() == objects_before_materialization + 1U,
            "proxy redo did not restore the materialized instance");
    }

    auto snapshot = session.snapshot();
    require(snapshot.biological_sources.size() == sources.size() &&
            snapshot.objects.size() == sources.size(),
        "biological fixture did not materialize every independently authored source");
    require(static_cast<bool>(ui.dispatch(carto::application::SaveProjectAction{project_path})),
        "could not save biological CONDITION fixture");
    auto project = carto::project::ProjectDocument::load(project_path);
    require(project && project.value().biological_proxy_instance_receipts().size() == sources.size(),
        "biological fixture receipts did not survive .carto save/reopen");
    const std::array<std::string_view, 3U> families{"canid", "human", "avian"};
    const std::array<carto::biological::LayerKind, 3U> covering_kinds{
        carto::biological::LayerKind::strand,
        carto::biological::LayerKind::strand,
        carto::biological::LayerKind::feather,
    };
    for (std::size_t index = 0U; index < sources.size(); ++index) {
        const auto& authored_source = sources[index];
        const carto::project::BiologicalAuthoringKey key{
            authored_source.source.source_namespace, authored_source.source.asset_id};
        const auto stored = project.value().biological_authoring_sources().find(key);
        require(stored != project.value().biological_authoring_sources().end(),
            "biological .carto source lineage is missing");
        const auto decoded = carto::biological::BiologicalAuthoringDocument::deserialize(
            stored->second.bytes);
        require(decoded && decoded.value().layers().size() == 2U &&
                decoded.value().layers().at(carto::biological::LayerId{2U}).kind ==
                    covering_kinds[index] &&
                decoded.value().layers().at(carto::biological::LayerId{2U}).semantic_type == "covering." +
                    std::string(families[index]),
            "biological .carto reopen changed its fur, hair, or feather source layer");
        for (const auto forbidden : {"DONOR_REPOSITORY", "DONOR_COMMIT", "runtime_handle",
                 "provider_handle", "worker_endpoint", "telemetry", "private-donor"}) {
            require(stored->second.bytes.find(forbidden) == std::string::npos,
                "biological source serialization contains private donor identity/runtime material");
        }
    }

    const auto receipt = project.value().biological_proxy_instance_receipts().begin()->second;
    const auto& base_mesh = project.value().meshes().at(receipt.mesh_asset);
    const auto faces = base_mesh.faces_sorted();
    require(!faces.empty() && faces.front().vertices.size() >= 3U,
        "biological proxy fixture did not persist editable mesh topology");
    const carto::production::SculptSurfaceAnchor anchor{
        faces.front().id,
        {faces.front().vertices[0], faces.front().vertices[1], faces.front().vertices[2]},
        {0.25, 0.25, 0.5}};
    const auto sample = carto::production::make_sculpt_surface_sample(base_mesh, anchor);
    require(static_cast<bool>(sample), "biological proxy fixture has no sculptable surface sample");
    require(static_cast<bool>(ui.dispatch(carto::application::SetSelectionModeAction{
                carto::editor::SelectionMode::vertex})) &&
            static_cast<bool>(ui.dispatch(carto::application::SelectVertexAction{
                receipt.object, anchor.vertices[0]})),
        "could not select the source-receipted biological proxy for sculpting");
    snapshot = session.snapshot();
    require(static_cast<bool>(ui.dispatch(carto::application::AppendSculptSurfaceStrokeAction{
                snapshot.project_revision, receipt.object, receipt.mesh_asset,
                carto::production::SculptStrokeMode::add, 0.75, 0.05, {sample.value()}})),
        "native SculptStack could not edit the biological proxy");
    snapshot = session.snapshot();
    require(static_cast<bool>(ui.dispatch(carto::application::SaveProjectAction{std::nullopt})),
        "could not persist biological SculptStack fixture");
    const auto sculpted_project = carto::project::ProjectDocument::load(project_path);
    require(sculpted_project &&
            sculpted_project.value().sculpt_stacks().contains(receipt.mesh_asset) &&
            sculpted_project.value().biological_proxy_instance_receipts().size() == sources.size(),
        "biological source receipts and SculptStack did not persist together");

    carto::application::ApplicationSession reopened_session;
    carto::ui::UiController reopened_ui(reopened_session);
    require(static_cast<bool>(reopened_ui.dispatch(
                carto::application::OpenProjectAction{project_path})),
        "native application could not reopen the biological CONDITION fixture");
    const auto reopened = reopened_session.snapshot();
    require(reopened.biological_sources.size() == sources.size() &&
            reopened.objects.size() == sources.size(),
        "biological sources or proxy instances changed on native application reopen");
    require(sculpted_project.value().sculpt_stacks().contains(receipt.mesh_asset),
        "biological SculptStack was not preserved in the reopened project");
    const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - fixture_started).count();
    std::cout << "biological_fixture_elapsed_ms=" << elapsed_ms << '\n';
}

void run_pine_ridge_price_board_fixture(const std::filesystem::path& fixture_root) {
    const auto fixture_started = std::chrono::steady_clock::now();
    const auto project_path = fixture_root / "condition-pine-ridge-gas-price-board.carto";
    carto::application::ApplicationSession session;
    carto::ui::UiController ui(session);
    require(static_cast<bool>(ui.dispatch(carto::application::NewProjectAction{
                "CONDITION Pine Ridge gas price board"})),
        "could not create the Pine Ridge gas-price board fixture");

    carto::geometry::SignTextSource sign;
    sign.source_identity = "condition.fixture.pineridge.price-board";
    sign.source_revision = carto::core::Revision{1U};
    sign.text = "PINE RIDGE\nREGULAR $4.29\nDIESEL $4.59";
    sign.board_width = 4.8;
    sign.board_height = 1.6;
    sign.border = 0.1;
    sign.glyph_height = 0.28;
    sign.line_spacing = 0.05;
    require(static_cast<bool>(sign.validate()),
        "Pine Ridge price-board source is invalid");
    require(static_cast<bool>(ui.dispatch(carto::application::SetSignTextSourceAction{
                ui.snapshot().project_revision, sign})),
        "could not author the revision-1 Pine Ridge price board");
    require(static_cast<bool>(ui.dispatch(carto::application::CreateSignTextObjectAction{
                ui.snapshot().project_revision, sign.source_identity, sign.source_revision})),
        "could not compile the revision-1 Pine Ridge price board");
    const auto first_object = ui.snapshot().objects.front().object;
    require(first_object.mesh_asset.has_value(),
        "Pine Ridge revision-1 board has no generated panel mesh");

    auto clean_panel = carto::render::StandardMaterial{};
    clean_panel.base_color_factor = {0.055, 0.14, 0.19, 1.0};
    clean_panel.roughness = 0.48;
    auto clean_lettering = carto::render::StandardMaterial{};
    clean_lettering.base_color_factor = {0.92, 0.94, 0.88, 1.0};
    clean_lettering.roughness = 0.34;
    clean_lettering.emissive_factor = {0.35, 0.19, 0.04};
    clean_lettering.emissive_strength = 1.5;
    auto weathered_panel = carto::render::StandardMaterial{};
    weathered_panel.base_color_factor = {0.20, 0.16, 0.10, 1.0};
    weathered_panel.roughness = 0.9;
    auto replacement_lettering = carto::render::StandardMaterial{};
    replacement_lettering.base_color_factor = {0.95, 0.91, 0.78, 1.0};
    replacement_lettering.roughness = 0.62;
    replacement_lettering.emissive_factor = {0.19, 0.12, 0.035};
    replacement_lettering.emissive_strength = 0.8;
    for (const auto& [name, definition] : std::array{
             std::pair{"Pine Ridge enamel", clean_panel},
             std::pair{"Pine Ridge original lettering", clean_lettering},
             std::pair{"Pine Ridge weathered panel", weathered_panel},
             std::pair{"Pine Ridge replacement lettering", replacement_lettering}}) {
        require(static_cast<bool>(ui.dispatch(carto::application::CreateMaterialAction{
                    ui.snapshot().project_revision, name, definition})),
            std::string("could not create Pine Ridge material: ") + name);
    }
    const auto initial_materials = ui.snapshot().materials;
    require(initial_materials.size() == 4U,
        "Pine Ridge fixture did not create four explicit wear/material records");
    const auto assign_region = [&ui](std::uint64_t mesh_asset, std::uint32_t slot,
                                     carto::render::MaterialId material) {
        return ui.dispatch(carto::application::SetMaterialRegionAssignmentAction{
            ui.snapshot().project_revision,
            {mesh_asset, slot}, material, std::nullopt});
    };
    const auto first_panel_region = carto::geometry::kSignTextPanelMaterialSlot;
    const auto first_lettering_region = carto::geometry::kSignTextLetteringMaterialSlot;
    require(static_cast<bool>(assign_region(*first_object.mesh_asset,
                first_panel_region, initial_materials[0U].id)) &&
            static_cast<bool>(assign_region(*first_object.mesh_asset,
                first_lettering_region, initial_materials[1U].id)),
        "could not assign Pine Ridge clean panel and lettering materials");

    const auto revision_one_project = ui.snapshot();
    const auto stale_create = ui.dispatch(carto::application::CreateSignTextObjectAction{
        revision_one_project.project_revision, sign.source_identity,
        carto::core::Revision{2U}});
    require(!stale_create && stale_create.error().code == carto::core::ErrorCode::stale_data &&
            ui.snapshot().objects.size() == 1U &&
            ui.snapshot().project_revision == revision_one_project.project_revision,
        "Pine Ridge fixture accepted a stale sign-source revision or partially mutated the project");

    auto revised_sign = sign;
    revised_sign.source_revision = carto::core::Revision{2U};
    revised_sign.text = "PINE RIDGE\nREGULAR $4.19\nDIESEL $4.49";
    require(static_cast<bool>(ui.dispatch(carto::application::SetSignTextSourceAction{
                ui.snapshot().project_revision, revised_sign})),
        "could not author the revision-2 Pine Ridge price board");
    require(static_cast<bool>(ui.dispatch(carto::application::CreateSignTextObjectAction{
                ui.snapshot().project_revision, revised_sign.source_identity,
                revised_sign.source_revision})),
        "could not compile the alternate Pine Ridge price board");
    const auto two_variants = ui.snapshot();
    require(two_variants.objects.size() == 2U,
        "Pine Ridge alternate values did not create a second revision-bound sign instance");
    const auto revised_object = std::find_if(
        two_variants.objects.begin(), two_variants.objects.end(), [&](const auto& object) {
            return object.object.id != first_object.id;
        });
    require(revised_object != two_variants.objects.end() &&
            revised_object->object.mesh_asset.has_value(),
        "Pine Ridge alternate sign instance has no project-owned mesh");
    const std::uint64_t revised_mesh_asset = *revised_object->object.mesh_asset;
    const auto stale_revision_one = ui.dispatch(
        carto::application::CreateSignTextObjectAction{
            two_variants.project_revision, sign.source_identity, sign.source_revision});
    require(!stale_revision_one &&
            stale_revision_one.error().code == carto::core::ErrorCode::stale_data &&
            ui.snapshot().objects.size() == 2U,
        "Pine Ridge fixture accepted the superseded text revision");
    require(static_cast<bool>(assign_region(revised_mesh_asset, first_panel_region,
                initial_materials[2U].id)) &&
            static_cast<bool>(assign_region(revised_mesh_asset, first_lettering_region,
                initial_materials[3U].id)),
        "could not assign the weathered panel and replacement lettering materials");

    require(static_cast<bool>(ui.dispatch(carto::application::UndoAction{})),
        "could not undo the final Pine Ridge material assignment");
    require(ui.snapshot().material_assignments.size() == 3U,
        "Pine Ridge material undo did not remove exactly one assignment");
    require(static_cast<bool>(ui.dispatch(carto::application::RedoAction{})) &&
            ui.snapshot().material_assignments.size() == 4U,
        "Pine Ridge material redo did not restore the exact assignment set");
    const auto state_before_save = ui.snapshot();

    require(static_cast<bool>(ui.dispatch(carto::application::SaveProjectAction{project_path})),
        "could not persist the Pine Ridge gas-board fixture");
    const auto saved = carto::project::ProjectDocument::load(project_path);
    require(saved && saved.value().validate() &&
            saved.value().sign_text_sources().size() == 1U &&
            saved.value().sign_text_instance_receipts().size() == 2U,
        "saved Pine Ridge fixture failed project validation or lost a sign lineage");
    const auto& saved_source = saved.value().sign_text_sources().at(sign.source_identity);
    require(saved_source.source_revision == revised_sign.source_revision &&
            saved_source.serialize() == revised_sign.serialize(),
        "saved Pine Ridge source does not identify the current wording/value revision");
    const auto& receipts = saved.value().sign_text_instance_receipts();
    const auto receipt_v1 = std::find_if(receipts.begin(), receipts.end(), [&](const auto& item) {
        return item.second.source_revision == sign.source_revision;
    });
    const auto receipt_v2 = std::find_if(receipts.begin(), receipts.end(), [&](const auto& item) {
        return item.second.source_revision == revised_sign.source_revision;
    });
    require(receipt_v1 != receipts.end() && receipt_v2 != receipts.end() &&
            receipt_v1->second.source_snapshot == sign.serialize() &&
            receipt_v1->second.source_digest == fixture_digest(sign.serialize()) &&
            receipt_v2->second.source_snapshot == revised_sign.serialize() &&
            receipt_v2->second.source_digest == fixture_digest(revised_sign.serialize()),
        "Pine Ridge sign instances did not preserve exact source snapshots and digests");
    const auto& saved_assignments = saved.value().material_catalog().assignments();
    require(saved_assignments.size() == 4U &&
            saved_assignments.at({*first_object.mesh_asset, first_panel_region}).material ==
                initial_materials[0U].id &&
            saved_assignments.at({*first_object.mesh_asset, first_lettering_region}).material ==
                initial_materials[1U].id &&
            saved_assignments.at({revised_mesh_asset, first_panel_region}).material ==
                initial_materials[2U].id &&
            saved_assignments.at({revised_mesh_asset, first_lettering_region}).material ==
                initial_materials[3U].id,
        "Pine Ridge clean/worn panel and lettering assignments did not survive save");

    const std::string serialized = saved.value().serialize();
    require(serialized.find("donor_repository") == std::string::npos &&
            serialized.find("donor_commit") == std::string::npos &&
            serialized.find("runtime_endpoint") == std::string::npos &&
            serialized.find("VANTA") == std::string::npos,
        "Pine Ridge public fixture serialized a donor identity or runtime dependency marker");

    carto::application::ApplicationSession reopened_session;
    carto::ui::UiController reopened_ui(reopened_session);
    require(static_cast<bool>(reopened_ui.dispatch(
                carto::application::OpenProjectAction{project_path})),
        "native application could not reopen the Pine Ridge price-board fixture");
    const auto reopened = reopened_ui.snapshot();
    require(reopened.project_revision == state_before_save.project_revision &&
            reopened.objects.size() == 2U && reopened.material_assignments.size() == 4U &&
            reopened.sign_text_sources.size() == 1U &&
            reopened.sign_text_sources.front().source.source_revision ==
                revised_sign.source_revision,
        "Pine Ridge save/reopen lost object variants, material regions, or active source revision");

    const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - fixture_started).count();
    std::cout << "pine_ridge_fixture_project=" << project_path.string()
              << " source_revision=" << revised_sign.source_revision.value()
              << " sign_instances=2 material_regions=4 stale_source_rejected=true"
              << " undo_redo=true save_reopen=true donor_runtime=false"
              << " fixture_elapsed_ms=" << elapsed_ms
              << " performance_benchmark=false\n";
}

void run_safe_place_state_fixture(const std::filesystem::path& fixture_root) {
    const auto fixture_started = std::chrono::steady_clock::now();
    const auto project_path = fixture_root / "condition-safe-place-state-history.carto";
    carto::application::ApplicationSession session;
    carto::ui::UiController ui(session);
    require(static_cast<bool>(ui.dispatch(carto::application::NewProjectAction{
                "CONDITION Safe Place state history"})) &&
            static_cast<bool>(ui.import_condition_fixture()),
        "could not create the Safe Place source fixture");
    const auto baseline = session.condition_authoring();
    require(baseline.has_value() && !baseline.value().safe_places.empty() &&
            !baseline.value().surface_states.empty(),
        "Safe Place source fixture is missing its authored baseline records");

    const auto& safe_place = baseline.value().safe_places.front();
    carto::condition::AuthoredStateDelta privacy_delta;
    privacy_delta.id = 1U;
    privacy_delta.target_record_id = safe_place.id;
    privacy_delta.target_asset_id = safe_place.asset_id;
    privacy_delta.target_kind = carto::condition::StateTargetKind::safe_place_profile;
    privacy_delta.property = carto::condition::StateProperty::privacy;
    privacy_delta.before.numeric_value = safe_place.privacy;
    privacy_delta.after.numeric_value = 0.55;
    carto::condition::AuthoredStateBranch storm_branch;
    storm_branch.id = 1U;
    storm_branch.name = "storm-night-shelter";
    storm_branch.applicability = "after-storm-night";
    storm_branch.deltas.push_back(privacy_delta);
    require(static_cast<bool>(ui.add_condition_state_branch(storm_branch)) &&
            ui.snapshot().condition.state_branch_count == 1U,
        "could not add the revisioned Safe Place privacy alternative");
    const auto baseline_comparison = ui.compare_condition_state_branches(0U, 1U);
    require(baseline_comparison.has_value() &&
            baseline_comparison.value().differences.size() == 1U &&
            baseline_comparison.value().differences.front().right_value.numeric_value == 0.55,
        "Safe Place baseline comparison did not preserve its privacy alternative");

    const auto& surface = baseline.value().surface_states.front();
    carto::condition::AuthoredStateDelta wetness_delta;
    wetness_delta.id = 1U;
    wetness_delta.target_record_id = surface.id;
    wetness_delta.target_asset_id = surface.asset_id;
    wetness_delta.target_kind = carto::condition::StateTargetKind::surface_state;
    wetness_delta.property = carto::condition::StateProperty::wetness;
    wetness_delta.before.numeric_value = surface.wetness;
    wetness_delta.after.numeric_value = 0.9;
    carto::condition::AuthoredStateBranch rain_branch;
    rain_branch.id = 2U;
    rain_branch.parent_branch_id = storm_branch.id;
    rain_branch.name = "rain-on-sign";
    rain_branch.applicability = "after-heavy-rain";
    rain_branch.deltas.push_back(wetness_delta);
    require(static_cast<bool>(ui.add_condition_state_branch(rain_branch)) &&
            ui.snapshot().condition.state_branch_count == 2U,
        "could not add the nested Safe Place surface/wetness alternative");
    const auto nested_comparison = ui.compare_condition_state_branches(1U, 2U);
    require(nested_comparison.has_value() &&
            nested_comparison.value().differences.size() == 1U &&
            nested_comparison.value().differences.front().property ==
                carto::condition::StateProperty::wetness &&
            nested_comparison.value().differences.front().right_value.numeric_value == 0.9,
        "nested Safe Place comparison flattened or changed the wetness signal");

    auto stale_branch = storm_branch;
    stale_branch.id = 3U;
    stale_branch.name = "stale-before-value";
    stale_branch.deltas.front().before.numeric_value += 0.1;
    const auto before_stale = ui.snapshot();
    const auto stale_result = ui.add_condition_state_branch(stale_branch);
    const auto after_stale = ui.snapshot();
    require(!stale_result && stale_result.error().code == carto::core::ErrorCode::stale_data &&
            after_stale.project_revision == before_stale.project_revision &&
            after_stale.condition.authored_revision == before_stale.condition.authored_revision &&
            after_stale.operations.size() == before_stale.operations.size() &&
            after_stale.undo_count == before_stale.undo_count &&
            after_stale.condition.state_branch_count == 2U,
        "Safe Place authoring accepted a branch with a stale before-value or partially mutated state");
    const auto unchanged_source = session.condition_authoring();
    require(unchanged_source.has_value() &&
            unchanged_source.value().safe_places.front().privacy == safe_place.privacy &&
            unchanged_source.value().surface_states.front().wetness == surface.wetness,
        "authored review branches overwrote the Safe Place source baseline");

    const auto final_baseline_comparison = ui.compare_condition_state_branches(0U, 1U);
    require(final_baseline_comparison.has_value(),
        "could not refresh the Safe Place baseline comparison after the nested branch");
    const auto expected_comparison_digest =
        final_baseline_comparison.value().comparison_digest;
    require(static_cast<bool>(ui.dispatch(carto::application::SaveProjectAction{project_path})),
        "could not persist the Safe Place state-history fixture");
    const auto saved = carto::project::ProjectDocument::load(project_path);
    require(saved.has_value() && saved.value().validate() &&
            saved.value().condition_authoring().has_value(),
        "saved Safe Place project did not validate or preserve CONDITION authoring");
    const auto saved_authoring = carto::condition::AuthoringDocument::deserialize(
        saved.value().condition_authoring()->bytes);
    require(saved_authoring.has_value() && saved_authoring.value().state_branches.size() == 2U &&
            saved_authoring.value().safe_places.front().privacy == safe_place.privacy &&
            saved_authoring.value().surface_states.front().wetness == surface.wetness,
        "Safe Place project save changed the source baseline or branch history");

    carto::application::ApplicationSession reopened_session;
    carto::ui::UiController reopened_ui(reopened_session);
    require(static_cast<bool>(reopened_ui.dispatch(
                carto::application::OpenProjectAction{project_path})) &&
            reopened_ui.snapshot().condition.state_branch_count == 2U,
        "native application could not reopen both Safe Place state branches");
    const auto reopened_comparison = reopened_ui.compare_condition_state_branches(0U, 1U);
    require(reopened_comparison.has_value() &&
            reopened_comparison.value().comparison_digest == expected_comparison_digest,
        "Safe Place comparison receipt changed across project save/reopen");

    require(static_cast<bool>(ui.dispatch(carto::application::UndoAction{})) &&
            ui.snapshot().condition.state_branch_count == 1U &&
            static_cast<bool>(ui.dispatch(carto::application::UndoAction{})) &&
            ui.snapshot().condition.state_branch_count == 0U &&
            !ui.compare_condition_state_branches(0U, 1U),
        "Safe Place state branches did not undo in journal order");
    require(static_cast<bool>(ui.dispatch(carto::application::RedoAction{})) &&
            static_cast<bool>(ui.dispatch(carto::application::RedoAction{})) &&
            ui.snapshot().condition.state_branch_count == 2U,
        "Safe Place state branches did not redo in journal order");

    const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - fixture_started).count();
    std::cout << "safe_place_fixture_project=" << project_path.string()
              << " branches=2 nested=true baseline_preserved=true stale_before_rejected=true"
              << " save_reopen=true undo_redo=true runtime_activation=false"
              << " fixture_elapsed_ms=" << elapsed_ms
              << " performance_benchmark=false\n";
}

void run_general_store_fixture(const std::filesystem::path& fixture_root) {
    const auto fixture_started = std::chrono::steady_clock::now();
    const auto project_path = fixture_root / "condition-general-store.carto";
    const auto stale_project_path =
        fixture_root / "condition-general-store-stale-audio.carto";
    carto::application::ApplicationSession session;
    carto::ui::UiController ui(session);
    require(static_cast<bool>(ui.dispatch(carto::application::NewProjectAction{
                "CONDITION General Store exterior and interior"})),
        "could not create the General Store project");

    const auto add_box = [&ui](std::string name, carto::core::Vec3d size,
                               carto::core::Vec3d position) {
        const auto before = ui.snapshot();
        require(static_cast<bool>(ui.dispatch(carto::application::CreateBoxAction{
                    std::move(name), size})),
            "could not create a General Store blockout object");
        const auto after_create = ui.snapshot();
        const auto created = std::find_if(
            after_create.objects.begin(), after_create.objects.end(),
            [&before](const auto& item) {
                return std::none_of(before.objects.begin(), before.objects.end(),
                    [&item](const auto& old_item) {
                        return old_item.object.id == item.object.id;
                    });
            });
        require(created != after_create.objects.end(),
            "General Store box creation did not return a new scene identity");
        const auto object_id = created->object.id;
        carto::core::Transform transform;
        transform.translation = position;
        require(static_cast<bool>(ui.dispatch(
                    carto::application::SetObjectTransformAction{
                        object_id, transform, after_create.project_revision})),
            "could not place a General Store blockout object at its captured revision");
        return object_id;
    };

    const auto exterior_id = add_box(
        "General Store exterior blockout", {12.0, 4.0, 8.0}, {0.0, 2.0, 0.0});
    const auto sign_backing_id = add_box(
        "General Store sign backing", {4.8, 1.25, 0.2}, {0.0, 3.1, 4.1});
    const auto interior_id = add_box(
        "General Store interior blockout", {10.0, 3.2, 6.0}, {0.0, 1.6, 0.0});
    require(exterior_id.value == 1U && sign_backing_id.value == 2U &&
            interior_id.value == 3U,
        "General Store source asset identities no longer match their initial scene owners");

    auto authoring = carto::condition::make_condition_fixture();
    require(static_cast<bool>(authoring),
        "could not create the provider-neutral CONDITION authoring baseline");
    auto& document = authoring.value();
    document.project_namespace = "condition.fixture.general-store";
    auto& root_source = document.sources.front();
    root_source.label = "General Store source-authoring fixture";
    root_source.logical_locator = "fixture://condition/general-store";
    root_source.digest = fixture_digest("general-store-source-revision-1");
    for (auto& asset : document.assets) asset.source_digest = root_source.digest;
    document.assets[0U].stable_name = "general_store_exterior";
    document.assets[0U].tags = {"architecture", "exterior", "safe-place"};
    document.assets[1U].stable_name = "general_store_sign";
    document.assets[1U].tags = {"exterior", "familiarity", "signage"};
    document.assets[2U].stable_name = "general_store_interior";
    document.assets[2U].tags = {"architecture", "interior", "retail"};
    document.safe_places.front().asset_id = document.assets[0U].id;
    document.familiarity.front().asset_id = document.assets[1U].id;
    document.familiarity.front().semantic_id = "GENERAL_STORE.SIGN.BASELINE";
    document.view_conditions.front().asset_id = document.assets[1U].id;
    document.variants[0U].asset_id = document.assets[1U].id;
    document.variants[0U].semantic_id = "GENERAL_STORE.SIGN.VARIANT";
    document.variants[1U].asset_id = document.assets[0U].id;
    document.variants[1U].semantic_id = "GENERAL_STORE.PRESENTATION.DUSK";
    document.temporal_packages[0U].asset_id = document.assets[0U].id;
    document.temporal_packages[1U].asset_id = document.assets[0U].id;
    document.surface_states[0U].asset_id = document.assets[0U].id;
    document.surface_states[1U].asset_id = document.assets[1U].id;
    document.surface_states[1U].provenance_note =
        "fixture-authored General Store sign condition";
    document.interaction_anchors[0U].asset_id = document.assets[0U].id;
    document.interaction_anchors[1U].asset_id = document.assets[2U].id;
    document.interaction_anchors[1U].kind =
        carto::condition::AnchorKind::interaction;
    document.interaction_anchors[1U].interaction_class = "general-store.stocking";
    document.prefabs.front().name = "General Store interior kit";
    document.prefabs.front().sockets = {"store.cooler", "store.counter", "store.entry"};
    document.collections.front().name = "General Store";
    document.region_kits.front().name = "General Store exterior and interior";
    if (const auto validation = document.validate(); !validation) {
        throw std::runtime_error("General Store CONDITION source document is invalid: " +
            validation.error().message);
    }
    require(static_cast<bool>(ui.dispatch(
                carto::application::ImportConditionAuthoringAction{
                    document.serialize()})),
        "could not import the General Store CONDITION authoring source");

    const std::array blockouts{
        std::tuple{"rear wall", carto::core::Vec3d{12.0, 4.0, 0.2},
            carto::core::Vec3d{0.0, 2.0, -4.0}},
        std::tuple{"left wall", carto::core::Vec3d{0.2, 4.0, 8.0},
            carto::core::Vec3d{-6.0, 2.0, 0.0}},
        std::tuple{"right wall", carto::core::Vec3d{0.2, 4.0, 8.0},
            carto::core::Vec3d{6.0, 2.0, 0.0}},
        std::tuple{"front left wall", carto::core::Vec3d{4.0, 4.0, 0.2},
            carto::core::Vec3d{-4.0, 2.0, 4.0}},
        std::tuple{"front right wall", carto::core::Vec3d{4.0, 4.0, 0.2},
            carto::core::Vec3d{4.0, 2.0, 4.0}},
        std::tuple{"door header", carto::core::Vec3d{4.0, 0.5, 0.2},
            carto::core::Vec3d{0.0, 3.75, 4.0}},
        std::tuple{"checkout counter", carto::core::Vec3d{3.0, 0.9, 0.8},
            carto::core::Vec3d{0.0, 0.55, 0.8}},
        std::tuple{"refrigerated cooler", carto::core::Vec3d{1.6, 1.8, 1.2},
            carto::core::Vec3d{3.0, 0.9, -1.5}},
        std::tuple{"merchandise shelving", carto::core::Vec3d{0.6, 2.2, 4.0},
            carto::core::Vec3d{-4.0, 1.1, -0.5}},
    };
    for (const auto& [name, size, position] : blockouts) {
        static_cast<void>(add_box("General Store " + std::string(name), size, position));
    }

    carto::geometry::SignTextSource sign;
    sign.source_identity = "condition.fixture.general-store.front-sign";
    sign.source_revision = carto::core::Revision{1U};
    sign.text = "GENERAL STORE";
    sign.board_width = 4.2;
    sign.board_height = 0.8;
    sign.border = 0.08;
    sign.glyph_height = 0.32;
    require(static_cast<bool>(sign.validate()) &&
            static_cast<bool>(ui.dispatch(carto::application::SetSignTextSourceAction{
                ui.snapshot().project_revision, sign})) &&
            static_cast<bool>(ui.dispatch(carto::application::CreateSignTextObjectAction{
                ui.snapshot().project_revision, sign.source_identity, sign.source_revision})),
        "could not author and compile the revision-1 General Store frontage sign");
    const auto sign_object = ui.snapshot().objects.back().object;
    require(sign_object.mesh_asset.has_value(),
        "General Store frontage sign has no generated project-owned mesh");
    auto sign_transform = sign_object.local_transform;
    sign_transform.translation = {0.0, 3.1, 4.22};
    require(static_cast<bool>(ui.dispatch(carto::application::SetObjectTransformAction{
                sign_object.id, sign_transform, ui.snapshot().project_revision})),
        "could not place the generated General Store sign on its revision-bound backing");

    carto::render::StandardMaterial panel_material;
    panel_material.base_color_factor = {0.075, 0.12, 0.13, 1.0};
    panel_material.roughness = 0.68;
    carto::render::StandardMaterial lettering_material;
    lettering_material.base_color_factor = {0.92, 0.86, 0.68, 1.0};
    lettering_material.roughness = 0.4;
    lettering_material.emissive_factor = {0.28, 0.14, 0.025};
    lettering_material.emissive_strength = 1.1;
    require(static_cast<bool>(ui.dispatch(carto::application::CreateMaterialAction{
                ui.snapshot().project_revision, "General Store enamel panel", panel_material})) &&
            static_cast<bool>(ui.dispatch(carto::application::CreateMaterialAction{
                ui.snapshot().project_revision, "General Store warm lettering", lettering_material})),
        "could not author separate General Store panel and lettering materials");
    const auto materials = ui.snapshot().materials;
    require(materials.size() == 2U,
        "General Store material authoring produced an unexpected material count");
    const auto assign_sign_region = [&ui, mesh = *sign_object.mesh_asset](
        std::uint32_t slot, carto::render::MaterialId material) {
        return ui.dispatch(carto::application::SetMaterialRegionAssignmentAction{
            ui.snapshot().project_revision, {mesh, slot}, material, std::nullopt});
    };
    require(static_cast<bool>(assign_sign_region(
                carto::geometry::kSignTextPanelMaterialSlot, materials[0U].id)) &&
            static_cast<bool>(assign_sign_region(
                carto::geometry::kSignTextLetteringMaterialSlot, materials[1U].id)),
        "General Store sign panel and lettering did not receive independent materials");

    carto::project::PresentationCatalog presentation;
    carto::project::AuthoredCamera camera;
    camera.object = exterior_id;
    camera.identity = "condition.fixture.general-store.exterior-review";
    camera.bookmark_identity = "general-store.frontage";
    camera.review_purpose = carto::project::CameraReviewPurpose::silhouette_review;
    camera.vertical_fov_radians = 0.82;
    camera.near_plane = 0.05;
    camera.far_plane = 600.0;
    camera.active_for_review = true;
    camera.revision = carto::core::Revision{1U};
    carto::project::AuthoredLight practical;
    practical.object = sign_backing_id;
    practical.identity = "condition.fixture.general-store.front-practical";
    practical.semantic_class = "storefront-practical";
    practical.fixture_owner = "general-store-frontage";
    practical.power_owner = "general-store-service";
    practical.switch_owner = "general-store-entry-switch";
    practical.kind = carto::project::AuthoredLightKind::point;
    practical.linear_color = {1.0, 0.69, 0.38};
    practical.intensity = 24.0;
    practical.range = 18.0;
    practical.color_temperature_kelvin = 2'700.0;
    practical.normal_state = carto::project::LightNormalState::switchable;
    practical.revision = carto::core::Revision{1U};
    require(static_cast<bool>(presentation.insert_camera(camera)) &&
            static_cast<bool>(presentation.insert_light(practical)),
        "General Store review camera or practical-light source is invalid");
    const auto presentation_base = ui.snapshot();
    require(static_cast<bool>(ui.dispatch(carto::application::SetPresentationCatalogAction{
                presentation_base.project_generation,
                presentation_base.project_revision, std::move(presentation)})),
        "could not commit General Store camera and practical-light intent");

    std::vector<carto::condition::Id> clip_source_ids;
    const std::array<std::string_view, 3U> clip_names{
        "refrigerator", "freezer", "fluorescent"};
    for (std::size_t index = 0U; index < clip_names.size(); ++index) {
        auto wave = make_synthetic_wave();
        wave[44U + index * 2U] ^= static_cast<std::uint8_t>(index + 1U);
        const auto wave_path = fixture_root /
            ("general-store-" + std::string(clip_names[index]) + ".wav");
        write_bytes(wave_path, wave);
        require(static_cast<bool>(ui.import_condition_audio_clip(wave_path)),
            "could not import a synthetic General Store WAVE evidence source");
        const auto evidence_snapshot = ui.snapshot();
        const auto& evidence = evidence_snapshot.condition.sources.back();
        const auto expected_digest = carto::assets::sha256(wave);
        require(evidence.current && !evidence.reviewed &&
                evidence.digest == expected_digest &&
                evidence.locator.find(fixture_root.string()) == std::string::npos,
            "General Store WAVE import did not retain current, pathless digest evidence");
        clip_source_ids.push_back(evidence.id);
        require(static_cast<bool>(ui.set_condition_source_reviewed(evidence.id, true)),
            "could not review synthetic General Store source evidence");

        carto::condition::AuthoredAudioSource audio;
        audio.asset_id = document.assets[2U].id;
        audio.clip_source_id = evidence.id;
        audio.local_transform.translation = {
            static_cast<double>(index) * 1.25 - 1.25, 1.0, -1.0};
        audio.gain = index == 2U ? 0.18 : 0.35;
        audio.loop = true;
        audio.minimum_distance = 0.5;
        audio.maximum_distance = index == 2U ? 18.0 : 12.0;
        audio.bus = carto::condition::AudioBusClass::machinery;
        audio.reverb_send = 0.12;
        audio.normal_operating_state = "open";
        audio.familiarity_role = "general-store." + std::string(clip_names[index]);
        require(static_cast<bool>(ui.add_condition_audio_source(audio)),
            "could not author a revision-bound General Store audio source");
    }

    carto::condition::AuthoredAcousticZone interior_zone;
    interior_zone.asset_id = document.assets[2U].id;
    interior_zone.half_extents = {5.0, 2.0, 3.0};
    interior_zone.acoustic_class = "general-store-interior";
    interior_zone.reverb_intent = "small-retail-room";
    interior_zone.priority = 10U;
    interior_zone.semantic_region = "general-store.main-floor";
    require(static_cast<bool>(ui.add_condition_acoustic_zone(interior_zone)),
        "could not author the General Store interior acoustic zone");
    carto::condition::AuthoredAcousticZone cooler_zone;
    cooler_zone.asset_id = document.assets[2U].id;
    cooler_zone.shape = carto::condition::AcousticZoneShape::sphere;
    cooler_zone.half_extents = {};
    cooler_zone.radius = 2.5;
    cooler_zone.local_transform.translation = {3.0, 1.0, -1.5};
    cooler_zone.acoustic_class = "refrigerated-case";
    cooler_zone.reverb_intent = "short-enclosure";
    cooler_zone.priority = 20U;
    cooler_zone.semantic_region = "general-store.cooler";
    require(static_cast<bool>(ui.add_condition_acoustic_zone(cooler_zone)),
        "could not author the General Store cooler acoustic zone");

    const auto before_zone_undo = ui.snapshot();
    require(before_zone_undo.objects.size() == 13U &&
            before_zone_undo.condition.audio_source_count == 3U &&
            before_zone_undo.condition.acoustic_zone_count == 2U &&
            before_zone_undo.material_assignments.size() == 2U,
        "General Store integrated exterior/interior authoring is missing a source subsystem");
    require(static_cast<bool>(ui.dispatch(carto::application::UndoAction{})) &&
            ui.snapshot().condition.acoustic_zone_count == 1U &&
            static_cast<bool>(ui.dispatch(carto::application::RedoAction{})) &&
            ui.snapshot().condition.acoustic_zone_count == 2U,
        "General Store acoustic-zone transaction did not undo and redo exactly");
    require(static_cast<bool>(ui.build_condition_package()),
        "reviewed General Store CONDITION sources were not publication-admissible");

    const auto saved_revision = ui.snapshot();
    require(static_cast<bool>(ui.dispatch(
                carto::application::SaveProjectAction{project_path})),
        "could not save the integrated General Store project");
    const auto saved = carto::project::ProjectDocument::load(project_path);
    require(static_cast<bool>(saved) && static_cast<bool>(saved.value().validate()) &&
            saved.value().condition_authoring().has_value() &&
            saved.value().sign_text_sources().size() == 1U &&
            saved.value().sign_text_instance_receipts().size() == 1U &&
            saved.value().presentation_catalog().cameras().size() == 1U &&
            saved.value().presentation_catalog().lights().size() == 1U &&
            saved.value().material_catalog().assignments().size() == 2U,
        "saved General Store project lost condition, sign, presentation, or material source data");
    const auto saved_authoring = carto::condition::AuthoringDocument::deserialize(
        saved.value().condition_authoring()->bytes);
    require(static_cast<bool>(saved_authoring) &&
            saved_authoring.value().project_namespace == "condition.fixture.general-store" &&
            saved_authoring.value().assets[0U].authored_object_id == exterior_id.value &&
            saved_authoring.value().assets[1U].authored_object_id == sign_backing_id.value &&
            saved_authoring.value().assets[2U].authored_object_id == interior_id.value &&
            saved_authoring.value().audio_sources.size() == 3U &&
            saved_authoring.value().acoustic_zones.size() == 2U &&
            saved_authoring.value().sources.size() == 4U,
        "General Store saved source identity/revision, object bindings, or audio evidence drifted");
    const auto& saved_sign = saved.value().sign_text_sources().at(sign.source_identity);
    const auto& sign_receipt = saved.value().sign_text_instance_receipts().begin()->second;
    require(saved_sign.serialize() == sign.serialize() &&
            sign_receipt.source_snapshot == sign.serialize() &&
            sign_receipt.source_digest == fixture_digest(sign.serialize()) &&
            sign_receipt.source_revision == sign.source_revision,
        "General Store sign geometry lost its exact source revision or canonical receipt");
    const std::string project_bytes = saved.value().serialize();
    require(project_bytes.find(fixture_root.string()) == std::string::npos &&
            project_bytes.find("donor_repository") == std::string::npos &&
            project_bytes.find("donor_commit") == std::string::npos &&
            project_bytes.find("runtime_endpoint") == std::string::npos &&
            project_bytes.find("VANTA") == std::string::npos &&
            project_bytes.find("CONDITION runtime") == std::string::npos,
        "General Store .carto serialized a workstation path or private donor runtime identity");

    carto::application::ApplicationSession reopened_session;
    carto::ui::UiController reopened_ui(reopened_session);
    require(static_cast<bool>(reopened_ui.dispatch(
                carto::application::OpenProjectAction{project_path})),
        "native application could not reopen the General Store .carto project");
    const auto reopened = reopened_ui.snapshot();
    require(reopened.project_revision == saved_revision.project_revision &&
            reopened.objects.size() == 13U &&
            reopened.condition.audio_source_count == 3U &&
            reopened.condition.acoustic_zone_count == 2U &&
            reopened.sign_text_sources.size() == 1U &&
            reopened.material_assignments.size() == 2U,
        "General Store native application reopen changed geometry, sound zones, sign, or materials");

    const auto stale_invalidation = ui.invalidate_condition_source(
        clip_source_ids.front(), "general-store-audio-revision-2",
        fixture_digest("general-store-refrigerator-replacement-v2"));
    require(static_cast<bool>(stale_invalidation) &&
            ui.snapshot().condition.stale &&
            !ui.snapshot().condition.publication_ready && !ui.build_condition_package(),
        "General Store stale audio source remained publishable");
    require(static_cast<bool>(ui.dispatch(
                carto::application::SaveProjectAction{stale_project_path})),
        "could not save the fail-closed General Store stale-source fixture");
    carto::application::ApplicationSession stale_reopened_session;
    carto::ui::UiController stale_reopened_ui(stale_reopened_session);
    require(static_cast<bool>(stale_reopened_ui.dispatch(
                carto::application::OpenProjectAction{stale_project_path})) &&
            stale_reopened_ui.snapshot().condition.stale &&
            !stale_reopened_ui.snapshot().condition.publication_ready &&
            !stale_reopened_ui.build_condition_package(),
        "General Store save/reopen did not preserve fail-closed stale-source state");
    require(static_cast<bool>(ui.dispatch(carto::application::UndoAction{})) &&
            !ui.snapshot().condition.stale && ui.snapshot().condition.publication_ready &&
            static_cast<bool>(ui.build_condition_package()) &&
            static_cast<bool>(ui.dispatch(carto::application::RedoAction{})) &&
            ui.snapshot().condition.stale && !ui.snapshot().condition.publication_ready,
        "General Store source invalidation did not restore and reapply through ordered undo/redo");

    const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - fixture_started).count();
    std::cout << "general_store_fixture_project=" << project_path.string()
              << " stale_fixture_project=" << stale_project_path.string()
              << " source_revision=1 authored_objects=13 audio_sources=3 acoustic_zones=2"
              << " sign_revision=1 materials=2 camera_intent=true practical_light=true"
              << " stale_audio_rejected=true save_reopen=true undo_redo=true"
              << " donor_runtime=false fixture_elapsed_ms=" << elapsed_ms
              << " performance_benchmark=false\n";
}

void run(const std::filesystem::path& output_root) {
    std::filesystem::create_directories(output_root);
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto fixture_root = output_root / ("run-" + std::to_string(stamp));
    std::filesystem::create_directories(fixture_root);
    const auto wave_path = fixture_root / "synthetic-acoustic-tone.wav";
    const auto project_path = fixture_root / "condition-motel8-integrated.carto";
    const auto stale_project_path = fixture_root / "condition-motel8-stale-source.carto";

    const auto wave = make_synthetic_wave();
    write_bytes(wave_path, wave);
    const auto inspection = carto::condition::inspect_wave_clip(wave);
    require(inspection.has_value(), "generated WAVE clip did not pass the production inspector");
    require(inspection.value().channels == 1U &&
            inspection.value().sample_rate == 48'000U &&
            inspection.value().bits_per_sample == 16U &&
            inspection.value().frame_count == 48'000U,
        "generated WAVE clip metadata differs from the deterministic fixture contract");

    auto authoring_result = carto::condition::make_condition_fixture();
    require(authoring_result.has_value(), "could not create the base CONDITION authoring fixture");
    auto authoring = std::move(authoring_result).value();
    authoring.project_revision = carto::core::Revision{2U};
    require(authoring.assets.size() == 3U,
        "base CONDITION fixture does not contain the Motel 8 asset set");
    // Start unbound so the integrated fixture exercises native revision-bound
    // bindings for the exterior, sign, and room rather than trusting imported IDs.
    for (auto& asset : authoring.assets) asset.authored_object_id = 0U;

    constexpr std::string_view import_settings =
        "Cartographer.WaveImport/v1;RIFF-WAVE;PCM8-16-24-32-or-IEEE-F32-F64;metadata-only";
    const auto settings_bytes = std::span<const std::uint8_t>{
        reinterpret_cast<const std::uint8_t*>(import_settings.data()),
        import_settings.size()};
    const auto wave_digest = carto::assets::sha256(wave);
    authoring.sources.push_back(carto::condition::SourceEvidence{
        2U,
        carto::condition::SourceKind::imported_asset,
        "Synthetic acoustic QA tone",
        "audio/sha256/" + wave_digest.hex() + ".wav",
        "sha256:" + wave_digest.hex(),
        wave_digest,
        "Cartographer.WaveImport",
        "1",
        carto::assets::sha256(settings_bytes),
        false,
        true,
        {"WAVE inspected: PCM integer, 1 channel, 48000 Hz, 16 bit, 48000 frames"}});
    authoring.audio_sources.push_back(carto::condition::AuthoredAudioSource{
        1U, 3U, 2U, carto::core::Transform::identity(), 0.5, 1.0,
        true, true, 0.75, 12.0, 50U,
        carto::condition::AudioBusClass::ambient,
        carto::condition::AudioOcclusionIntent::none, 0.15,
        "fixture-active", "qa.synthetic-tone", true});
    authoring.acoustic_zones.push_back(carto::condition::AuthoredAcousticZone{
        1U, 3U, carto::condition::AcousticZoneShape::box,
        carto::core::Transform::identity(), {2.0, 1.5, 2.5}, 0.0,
        "motel-room-interior", 0.8, "motel-room-dusk", 50U,
        "condition.fixture.motel8.room", true});
    require(static_cast<bool>(authoring.validate()),
        "generated CONDITION acoustic authoring is invalid");

    carto::application::ApplicationSession session;
    carto::ui::UiController ui(session);
    require(static_cast<bool>(ui.dispatch(carto::application::NewProjectAction{
                    "CONDITION Motel 8 integrated authoring QA"})),
        "could not create the temporary Motel 8 QA project");
    require(static_cast<bool>(ui.dispatch(carto::application::CreateBoxAction{
                    "Motel 8 exterior", {12.0, 4.0, 8.0}})),
        "could not create the Motel 8 exterior scene object");
    carto::geometry::SignTextSource sign_source;
    sign_source.source_identity = "condition.fixture.motel8.sign";
    sign_source.source_revision = carto::core::Revision{1U};
    sign_source.text = "MOTEL 8\nVACANCY";
    sign_source.board_width = 3.8;
    sign_source.board_height = 1.4;
    sign_source.glyph_height = 0.48;
    require(static_cast<bool>(sign_source.validate()),
        "Motel 8 sign text source is invalid");
    require(static_cast<bool>(ui.dispatch(carto::application::SetSignTextSourceAction{
                    ui.snapshot().project_revision, sign_source})),
        "could not author the revisioned Motel 8 sign text source");
    require(static_cast<bool>(ui.dispatch(carto::application::CreateSignTextObjectAction{
                    ui.snapshot().project_revision, sign_source.source_identity,
                    sign_source.source_revision})),
        "could not materialize the project-owned Motel 8 sign mesh");
    require(static_cast<bool>(ui.dispatch(carto::application::CreateBoxAction{
                    "Motel 8 room", {8.0, 3.0, 6.0}})),
        "could not create the Motel 8 room scene object");
    require(static_cast<bool>(ui.dispatch(carto::application::CreateBoxAction{
                    "Motel 8 door leaf", {0.9, 2.0, 0.12}})),
        "could not create the separate Motel 8 door-leaf mesh");
    require(static_cast<bool>(ui.dispatch(carto::application::ImportConditionAuthoringAction{
                    authoring.serialize()})),
        "could not import the generated Motel 8 CONDITION authoring fixture");
    const auto before_review = ui.snapshot();
    const auto imported_audio_source = std::find_if(
        before_review.condition.sources.begin(), before_review.condition.sources.end(),
        [](const auto& source) { return source.id == 2U; });
    require(imported_audio_source != before_review.condition.sources.end() &&
            !imported_audio_source->reviewed && imported_audio_source->current &&
            !ui.build_condition_package(),
        "imported WAVE evidence bypassed its required human-review gate");
    require(static_cast<bool>(ui.set_condition_source_reviewed(2U, true)),
        "native UI could not explicitly review the synthetic WAVE evidence");
    const auto after_review = ui.snapshot();
    require(after_review.project_revision == before_review.project_revision.next() &&
            after_review.condition.authored_revision ==
                before_review.condition.authored_revision.next() &&
            after_review.condition.sources.back().reviewed,
        "WAVE source review did not preserve the project transaction/revision boundary");
    const auto before_binding = ui.snapshot();
    require(before_binding.objects.size() == 4U,
        "Motel 8 fixture does not contain exterior, sign, room, and door-leaf objects");
    std::array<carto::scene::ObjectId, 4U> motel_objects{};
    for (std::size_t index = 0U; index < motel_objects.size(); ++index) {
        motel_objects[index] = before_binding.objects[index].object.id;
    }
    const auto place_fixture_object = [&ui](
        carto::scene::ObjectId object,
        carto::core::Vec3d translation,
        carto::core::Quaternion rotation) {
        const auto before = ui.snapshot();
        const auto found = std::find_if(before.objects.begin(), before.objects.end(),
            [object](const auto& view) { return view.object.id == object; });
        require(found != before.objects.end(),
            "Motel 8 layout references a missing scene object");
        auto transform = found->object.local_transform;
        transform.translation = translation;
        transform.rotation = rotation;
        require(static_cast<bool>(ui.dispatch(
                carto::application::SetObjectTransformAction{
                    object, transform, before.project_revision})),
            "Motel 8 fixture could not place a scene object through the project command");
    };
    place_fixture_object(motel_objects[1U], {-24.0, 0.8, -1.9},
        {0.0, -0.7071067811865476, 0.0, 0.7071067811865476});
    place_fixture_object(motel_objects[2U], {0.0, 0.0, 10.0},
        carto::core::Quaternion::identity());
    place_fixture_object(motel_objects[3U], {0.0, 0.0, 6.85},
        carto::core::Quaternion::identity());

    const auto motel_scene = ui.snapshot();
    const auto* sign_instance = motel_scene.viewport.scene.find(motel_objects[1U]);
    require(sign_instance != nullptr && sign_instance->mesh != nullptr &&
            sign_instance->mesh->valid(),
        "Motel 8 observation fixture has no current compiled sign mesh");
    carto::core::Vec3d sign_minimum{
        std::numeric_limits<double>::infinity(),
        std::numeric_limits<double>::infinity(),
        std::numeric_limits<double>::infinity()};
    carto::core::Vec3d sign_maximum{
        -std::numeric_limits<double>::infinity(),
        -std::numeric_limits<double>::infinity(),
        -std::numeric_limits<double>::infinity()};
    for (const auto local : sign_instance->mesh->positions) {
        const auto world = sign_instance->world_transform.translation +
            sign_instance->world_transform.rotation.rotate(
                carto::core::componentwise_multiply(
                    sign_instance->world_transform.scale, local));
        sign_minimum.x = std::min(sign_minimum.x, world.x);
        sign_minimum.y = std::min(sign_minimum.y, world.y);
        sign_minimum.z = std::min(sign_minimum.z, world.z);
        sign_maximum.x = std::max(sign_maximum.x, world.x);
        sign_maximum.y = std::max(sign_maximum.y, world.y);
        sign_maximum.z = std::max(sign_maximum.z, world.z);
    }
    const carto::core::Vec3d sign_target{
        (sign_minimum.x + sign_maximum.x) * 0.5,
        (sign_minimum.y + sign_maximum.y) * 0.5,
        (sign_minimum.z + sign_maximum.z) * 0.5};
    struct MotelObservationStation {
        const char* name;
        carto::core::Vec3d observer;
        carto::editor::ObservationDisposition expected_sign_disposition;
    };
    const std::array<MotelObservationStation, 4U> observation_stations{{
        {"highway approach", {-30.0, 1.5, 0.0},
            carto::editor::ObservationDisposition::visible},
        {"parking lot", {-8.0, 1.5, 8.0},
            carto::editor::ObservationDisposition::visible},
        {"room doorway", {0.0, 1.5, 6.5},
            carto::editor::ObservationDisposition::visible},
        {"bed", {0.0, 1.0, 10.0},
            carto::editor::ObservationDisposition::occluded}}};
    const auto observations_before = ui.snapshot();
    for (std::size_t index = 0U; index < observation_stations.size(); ++index) {
        const auto& station = observation_stations[index];
        carto::editor::AuthoringObservationQuery query;
        query.observer = station.observer;
        query.forward = (sign_target - station.observer).normalized();
        query.up = {0.0, 1.0, 0.0};
        query.horizontal_fov_radians = 1.3962634015954636;
        query.vertical_fov_radians = 1.0471975511965976;
        query.source_revision = observations_before.project_revision;
        query.targets = {{static_cast<std::uint64_t>(index + 1U), motel_objects[1U],
            sign_target, carto::editor::ObservationTargetKind::sign}};
        carto::editor::AuthoringObservationOptions options;
        options.max_distance = 100.0;
        const auto report = ui.evaluate_authoring_observation(query, options);
        require(report.has_value() && report.value().targets.size() == 1U,
            std::string("Motel 8 observation failed from ") + station.name);
        if (report.value().targets.front().disposition !=
                station.expected_sign_disposition) {
            const auto& sample = report.value().targets.front();
            const std::string blocker = sample.sightline.has_value() &&
                    sample.sightline->blocker_object.has_value()
                ? std::to_string(sample.sightline->blocker_object->value)
                : std::string("none");
            throw std::runtime_error(
                std::string("Motel 8 sign visibility differed from authored expectation at ") +
                station.name + " disposition=" +
                std::to_string(static_cast<int>(sample.disposition)) +
                " blocker_object=" + blocker);
        }
        const auto repeated = ui.evaluate_authoring_observation(query, options);
        require(repeated.has_value() &&
                repeated.value().targets.front().disposition ==
                    report.value().targets.front().disposition &&
                repeated.value().triangles_tested == report.value().triangles_tested,
            std::string("Motel 8 observation was not deterministic at ") + station.name);
    }
    const auto observations_after = ui.snapshot();
    require(observations_after.project_revision == observations_before.project_revision &&
            observations_after.operations.size() == observations_before.operations.size(),
        "Motel 8 observation queries mutated project revision or operation history");

    std::array<carto::scene::ObjectId, 3U> owner_objects{};
    for (std::size_t index = 0U; index < authoring.assets.size(); ++index) {
        const auto before = ui.snapshot();
        owner_objects[index] = before.objects[index].object.id;
        const auto binding = ui.bind_condition_asset_to_object(
            authoring.assets[index].id, owner_objects[index],
            before.project_revision, before.condition.authored_revision);
        require(binding.has_value() && binding.value().document_changed,
            "native UI command could not bind every Motel 8 asset to its scene object");
        const auto after = ui.snapshot();
        require(after.project_revision == before.project_revision.next() &&
                after.condition.authored_revision ==
                    before.condition.authored_revision.next(),
            "native Motel 8 asset binding did not advance both revisions exactly once");
        require(!after.operations.empty() && after.operations.back().action ==
                "Bind CONDITION Asset to Scene Object",
            "native Motel 8 binding did not leave an operation receipt");
    }
    const auto before_assembly = ui.snapshot();
    carto::kinematic::KinematicGraph door_graph;
    carto::kinematic::KinematicNode door_frame;
    door_frame.id = carto::kinematic::JointId{1U};
    door_frame.kind = carto::kinematic::JointKind::fixed;
    door_frame.semantic_type = "assembly.frame";
    carto::kinematic::KinematicNode door_hinge;
    door_hinge.id = carto::kinematic::JointId{2U};
    door_hinge.parent = carto::kinematic::JointId{1U};
    door_hinge.kind = carto::kinematic::JointKind::hinge;
    door_hinge.rest.translation = {0.0, 1.0, 0.0};
    door_hinge.semantic_type = "assembly.hinge";
    require(static_cast<bool>(door_graph.insert_node(door_frame)) &&
            static_cast<bool>(door_graph.insert_node(door_hinge)),
        "could not construct the source-only Motel 8 door articulation");
    carto::kinematic::JointLimit door_travel;
    door_travel.joint = carto::kinematic::JointId{2U};
    door_travel.angular_enabled = true;
    door_travel.angular_min = {0.0, 0.0, 0.0};
    door_travel.angular_max = {1.8325957145940461, 0.0, 0.0};
    carto::kinematic::JointLimitSet door_limits;
    require(static_cast<bool>(door_limits.insert(door_travel)),
        "could not author Motel 8 door travel limits");
    carto::condition::KinematicAssemblySource door_assembly;
    door_assembly.asset_id = authoring.assets[2U].id;
    door_assembly.graph_bytes = door_graph.serialize();
    door_assembly.joint_limit_bytes = door_limits.serialize();
    const auto door_object = std::find_if(before_assembly.objects.begin(),
        before_assembly.objects.end(), [](const auto& object) {
            return object.object.name == "Motel 8 door leaf";
        });
    require(door_object != before_assembly.objects.end() &&
            door_object->object.mesh_asset.has_value() &&
            door_object->mesh_revision.has_value(),
        "Motel 8 door leaf has no revisioned mesh binding");
    door_assembly.part_bindings.push_back(
        carto::condition::KinematicAssemblySource::PartBinding{
            2U, door_object->object.id.value, *door_object->object.mesh_asset,
            *door_object->mesh_revision});
    auto stale_part_binding = door_assembly;
    stale_part_binding.part_bindings.front().mesh_revision =
        stale_part_binding.part_bindings.front().mesh_revision.next();
    require(!ui.dispatch(carto::application::AddConditionKinematicAssemblyAction{
                before_assembly.project_revision,
                before_assembly.condition.authored_revision,
                stale_part_binding}) &&
            ui.snapshot().project_revision == before_assembly.project_revision,
        "CONDITION articulation accepted a stale door-leaf mesh revision");
    require(static_cast<bool>(ui.dispatch(
                carto::application::AddConditionKinematicAssemblyAction{
                    before_assembly.project_revision,
                    before_assembly.condition.authored_revision,
                    door_assembly})),
        "native CONDITION authoring could not add the Motel 8 door articulation");
    auto after_assembly = ui.snapshot();
    require(after_assembly.project_revision == before_assembly.project_revision.next(),
        "Motel 8 articulation did not advance exactly one project revision");
    require(after_assembly.condition.authored_revision ==
                before_assembly.condition.authored_revision.next(),
        "Motel 8 articulation did not advance exactly one authoring revision");
    require(after_assembly.condition.kinematic_assembly_count == 1U &&
            after_assembly.condition.kinematic_assemblies.size() == 1U &&
            after_assembly.condition.kinematic_assemblies.front().part_binding_count == 1U &&
            after_assembly.condition.kinematic_assemblies.front().part_bindings_current &&
            after_assembly.condition.kinematic_assemblies.front().hinge_joint_ids ==
                std::vector<std::uint64_t>{2U},
        "Motel 8 articulation is missing from the application snapshot");
    require(!after_assembly.operations.empty(),
        "Motel 8 articulation did not preserve a human journal receipt");
    require(after_assembly.operations.back().action ==
                "Add CONDITION Kinematic Assembly",
        std::string("Motel 8 articulation journaled as: ") +
            after_assembly.operations.back().action);
    const auto authoring_before_pose_preview = session.condition_authoring();
    require(authoring_before_pose_preview.has_value(),
        "Motel 8 authoring source could not be read before pose preview");
    const auto closed_pose = ui.preview_condition_kinematic_hinge(
        after_assembly, after_assembly.condition.kinematic_assemblies.front().id,
        2U, 0.0);
    const auto open_pose = ui.preview_condition_kinematic_hinge(
        after_assembly, after_assembly.condition.kinematic_assemblies.front().id,
        2U, 1.0);
    require(closed_pose && open_pose &&
            closed_pose.value().part_poses.size() == 1U &&
            open_pose.value().part_poses.size() == 1U,
        "current Motel 8 articulation could not produce a bound door pose preview");
    const auto closed_door_pose = closed_pose.value().part_poses.front();
    const auto open_door_pose = open_pose.value().part_poses.front();
    require(closed_door_pose.scene_object_id == door_object->object.id.value &&
            open_door_pose.scene_object_id == door_object->object.id.value &&
            std::abs(closed_door_pose.world_transform.translation.x -
                door_object->world_transform.translation.x) < 1.0e-12 &&
            std::abs(closed_door_pose.world_transform.translation.y -
                door_object->world_transform.translation.y) < 1.0e-12 &&
            std::abs(closed_door_pose.world_transform.translation.z -
                door_object->world_transform.translation.z) < 1.0e-12 &&
            std::abs(open_door_pose.world_transform.translation.z -
                closed_door_pose.world_transform.translation.z) > 0.5,
        "read-only door pose preview did not rotate the bound leaf about its authored hinge");
    require(closed_pose.value().project_revision == after_assembly.project_revision &&
            open_pose.value().source_revision ==
                after_assembly.condition.kinematic_assemblies.front().source_revision &&
            ui.snapshot().project_revision == after_assembly.project_revision &&
            ui.snapshot().operations.size() == after_assembly.operations.size() &&
            session.condition_authoring().value().serialize() ==
                authoring_before_pose_preview.value().serialize(),
        "door pose preview mutated project revisions, journal, or serialized authoring");
    require(!ui.preview_condition_kinematic_hinge(
                before_assembly, after_assembly.condition.kinematic_assemblies.front().id,
                2U, 0.5),
        "stale pre-assembly snapshot was accepted for hinge pose preview");
    require(static_cast<bool>(ui.dispatch(carto::application::UndoAction{})) &&
            ui.snapshot().condition.kinematic_assemblies.empty() &&
            static_cast<bool>(ui.dispatch(carto::application::RedoAction{})),
        "Motel 8 articulation did not undo and redo as one source transaction");
    after_assembly = ui.snapshot();
    auto imported_assembly_document = session.condition_authoring();
    require(imported_assembly_document.has_value() &&
            imported_assembly_document.value().kinematic_assemblies.size() == 1U,
        "Motel 8 articulation is not present in project-owned authoring source");
    require(static_cast<bool>(ui.dispatch(carto::application::SelectObjectAction{
                door_object->object.id})) &&
            static_cast<bool>(ui.dispatch(carto::application::SetSelectionModeAction{
                carto::editor::SelectionMode::face})) &&
            static_cast<bool>(ui.dispatch(carto::application::SelectFaceAction{
                door_object->object.id, carto::geometry::FaceId{1U}})),
        "could not select the bound Motel 8 door-leaf face for a source-edit probe");
    carto::editor::ToolArguments door_mesh_edit;
    door_mesh_edit.distance = 0.05;
    require(static_cast<bool>(ui.dispatch(carto::application::InvokeToolAction{
                "mesh.extrude-face", door_mesh_edit})),
        "could not change the bound Motel 8 door-leaf mesh revision");
    const auto after_door_mesh_edit = ui.snapshot();
    require(after_door_mesh_edit.condition.stale &&
            !after_door_mesh_edit.condition.publication_ready &&
            !after_door_mesh_edit.condition.kinematic_assemblies.front().part_bindings_current &&
            !ui.build_condition_package() &&
            !ui.preview_condition_kinematic_hinge(after_door_mesh_edit,
                after_door_mesh_edit.condition.kinematic_assemblies.front().id,
                2U, 0.5),
        "editing the articulated mesh did not stale the binding and block publication");
    auto edited_assembly = imported_assembly_document.value().kinematic_assemblies.front();
    const auto edited_door_object = std::find_if(after_door_mesh_edit.objects.begin(),
        after_door_mesh_edit.objects.end(), [&door_object](const auto& object) {
            return object.object.id == door_object->object.id;
        });
    require(edited_door_object != after_door_mesh_edit.objects.end() &&
            edited_door_object->mesh_revision.has_value(),
        "edited Motel 8 door leaf has no current mesh revision for explicit rebind");
    edited_assembly.part_bindings.front().mesh_revision =
        *edited_door_object->mesh_revision;
    door_assembly.part_bindings = edited_assembly.part_bindings;
    const auto before_assembly_edit = ui.snapshot();
    const auto original_assembly_revision = edited_assembly.source_revision;
    auto edited_limits = carto::kinematic::JointLimitSet::deserialize(
        edited_assembly.joint_limit_bytes);
    require(edited_limits.has_value(), "Motel 8 door travel limits did not reopen");
    carto::kinematic::JointLimit expanded_travel = door_travel;
    expanded_travel.angular_max.x = 1.9198621771937625;
    carto::kinematic::JointLimitSet revised_limits;
    require(static_cast<bool>(revised_limits.insert(expanded_travel)),
        "could not revise Motel 8 door travel limits");
    edited_assembly.joint_limit_bytes = revised_limits.serialize();
    require(static_cast<bool>(ui.dispatch(
                carto::application::ReplaceConditionAuthoredRecordAction{
                    before_assembly_edit.project_revision,
                    before_assembly_edit.condition.authored_revision,
                    carto::application::ConditionAuthoredRecord{edited_assembly}})),
        "Motel 8 door travel rebind did not use the project command boundary");
    const auto after_assembly_edit = ui.snapshot();
    require(after_assembly_edit.condition.kinematic_assemblies.front().source_revision ==
                original_assembly_revision.next() &&
            after_assembly_edit.project_revision == before_assembly_edit.project_revision.next() &&
            after_assembly_edit.condition.kinematic_assemblies.front().part_bindings_current &&
            after_assembly_edit.condition.publication_ready,
        "Motel 8 door articulation rebind did not restore current source admission");
    edited_assembly.source_revision = original_assembly_revision;
    const auto before_stale_assembly_edit = ui.snapshot();
    require(!ui.dispatch(carto::application::ReplaceConditionAuthoredRecordAction{
                before_stale_assembly_edit.project_revision,
                before_stale_assembly_edit.condition.authored_revision,
                carto::application::ConditionAuthoredRecord{edited_assembly}}) &&
            ui.snapshot().project_revision == before_stale_assembly_edit.project_revision,
        "Motel 8 articulation accepted a stale source-revision edit");
    const auto after_binding = ui.snapshot();
    const auto stale_binding = ui.bind_condition_asset_to_object(
        authoring.assets.front().id, owner_objects.back(),
        before_binding.project_revision, before_binding.condition.authored_revision);
    require(!stale_binding && ui.snapshot().project_revision ==
            after_binding.project_revision,
        "integrated fixture accepted a stale project/authoring revision binding");

    const auto sign_object = std::find_if(after_binding.objects.begin(),
        after_binding.objects.end(), [&](const auto& object) {
            return object.object.id == owner_objects[1U];
        });
    require(sign_object != after_binding.objects.end() &&
            sign_object->object.mesh_asset.has_value(),
        "Motel 8 sign has no authored scene mesh");
    const std::uint64_t sign_mesh_asset = *sign_object->object.mesh_asset;
    carto::render::StandardMaterial panel_definition;
    panel_definition.base_color_factor = {0.035, 0.055, 0.085, 1.0};
    panel_definition.metallic = 0.72;
    panel_definition.roughness = 0.26;
    require(static_cast<bool>(ui.dispatch(carto::application::CreateMaterialAction{
                    ui.snapshot().project_revision, "Motel 8 sign panel enamel",
                    panel_definition})),
        "could not author the Motel 8 sign panel material");
    const auto panel_material_id = ui.snapshot().materials.back().id;
    carto::render::StandardMaterial lettering_definition;
    lettering_definition.base_color_factor = {0.92, 0.78, 0.48, 1.0};
    lettering_definition.roughness = 0.32;
    lettering_definition.emissive_factor = {0.85, 0.24, 0.035};
    lettering_definition.emissive_strength = 3.0;
    require(static_cast<bool>(ui.dispatch(carto::application::CreateMaterialAction{
                    ui.snapshot().project_revision, "Motel 8 raised lettering",
                    lettering_definition})),
        "could not author emissive source factors for Motel 8 lettering");
    const auto lettering_material_id = ui.snapshot().materials.back().id;
    require(panel_material_id != lettering_material_id,
        "Motel 8 panel and raised lettering must use independent material records");
    const carto::project::MaterialRegionKey panel_region{
        sign_mesh_asset, carto::geometry::kSignTextPanelMaterialSlot};
    const carto::project::MaterialRegionKey lettering_region{
        sign_mesh_asset, carto::geometry::kSignTextLetteringMaterialSlot};
    require(static_cast<bool>(ui.dispatch(
                carto::application::SetMaterialRegionAssignmentAction{
                    ui.snapshot().project_revision, panel_region,
                    panel_material_id, std::nullopt})),
        "could not assign the sign-panel finish to its material slot");
    require(static_cast<bool>(ui.dispatch(
                carto::application::SetMaterialRegionAssignmentAction{
                    ui.snapshot().project_revision, lettering_region,
                    lettering_material_id, std::nullopt})),
        "could not assign the lettering finish to its material slot");

    carto::project::AuthoredCamera review_camera;
    review_camera.object = owner_objects[0U];
    review_camera.identity = "condition.fixture.motel8.sign-review-camera";
    review_camera.bookmark_identity = "motel8.sign.approach";
    review_camera.review_purpose =
        carto::project::CameraReviewPurpose::familiarity_baseline;
    review_camera.vertical_fov_radians = 0.82;
    review_camera.near_plane = 0.05;
    review_camera.far_plane = 800.0;
    review_camera.active_for_review = true;
    review_camera.revision = carto::core::Revision{1U};
    carto::project::AuthoredLight sign_light;
    sign_light.object = owner_objects[1U];
    sign_light.identity = "condition.fixture.motel8.marquee-light";
    sign_light.semantic_class = "sign-marquee";
    sign_light.fixture_owner = "motel8-sign";
    sign_light.power_owner = "motel8-service";
    sign_light.switch_owner = "motel8-front-desk";
    sign_light.kind = carto::project::AuthoredLightKind::point;
    sign_light.linear_color = {1.0, 0.56, 0.22};
    sign_light.intensity = 18.0;
    sign_light.range = 16.0;
    sign_light.revision = carto::core::Revision{1U};
    carto::project::PresentationCatalog presentation;
    require(static_cast<bool>(presentation.insert_camera(review_camera)) &&
            static_cast<bool>(presentation.insert_light(sign_light)),
        "Motel 8 review camera or marquee-light source is invalid");
    const auto before_presentation = ui.snapshot();
    require(static_cast<bool>(ui.dispatch(
                carto::application::SetPresentationCatalogAction{
                    before_presentation.project_generation,
                    before_presentation.project_revision,
                    std::move(presentation)})),
        "could not commit project-owned Motel 8 camera and light intent");

    const auto before_sign_state = session.condition_authoring();
    require(before_sign_state && before_sign_state.value().surface_states.size() == 2U &&
            before_sign_state.value().surface_states[1U].asset_id == authoring.assets[1U].id,
        "fixture does not expose the sign-specific project state record");
    carto::condition::AuthoredStateDelta damage_delta;
    damage_delta.id = 1U;
    damage_delta.target_record_id = before_sign_state.value().surface_states[1U].id;
    damage_delta.target_asset_id = authoring.assets[1U].id;
    damage_delta.target_kind = carto::condition::StateTargetKind::surface_state;
    damage_delta.property = carto::condition::StateProperty::damage;
    damage_delta.before.text_value = "intact";
    damage_delta.after.text_value = "dented-sign";
    carto::condition::AuthoredStateBranch damaged_branch;
    damaged_branch.id = 1U;
    damaged_branch.name = "Motel 8 sign — dented";
    damaged_branch.applicability = "after-impact";
    damaged_branch.deltas.push_back(damage_delta);
    require(static_cast<bool>(ui.add_condition_state_branch(damaged_branch)),
        "could not author the sign damage branch through the project history path");

    carto::condition::AuthoredStateDelta repair_damage_delta = damage_delta;
    repair_damage_delta.id = 1U;
    repair_damage_delta.before.text_value = "dented-sign";
    repair_damage_delta.after.text_value = "intact";
    carto::condition::AuthoredStateDelta repair_maintenance_delta;
    repair_maintenance_delta.id = 2U;
    repair_maintenance_delta.target_record_id =
        before_sign_state.value().surface_states[1U].id;
    repair_maintenance_delta.target_asset_id = authoring.assets[1U].id;
    repair_maintenance_delta.target_kind =
        carto::condition::StateTargetKind::surface_state;
    repair_maintenance_delta.property = carto::condition::StateProperty::maintenance;
    repair_maintenance_delta.before.text_value = "aging";
    repair_maintenance_delta.after.text_value = "improvised-repair";
    carto::condition::AuthoredStateBranch repaired_branch;
    repaired_branch.id = 2U;
    repaired_branch.name = "Motel 8 sign — improvised repair";
    repaired_branch.parent_branch_id = damaged_branch.id;
    repaired_branch.applicability = "after-maintenance";
    repaired_branch.deltas = {repair_damage_delta, repair_maintenance_delta};
    require(static_cast<bool>(ui.add_condition_state_branch(repaired_branch)),
        "could not author the sign repair branch as a child of the damage branch");
    const auto sign_damage_comparison = ui.compare_condition_state_branches(0U, 1U);
    const auto sign_repair_comparison = ui.compare_condition_state_branches(1U, 2U);
    const auto sign_repaired_baseline = ui.compare_condition_state_branches(0U, 2U);
    require(sign_damage_comparison.has_value(),
        "could not compare the sign damage branch to baseline");
    require(sign_repair_comparison.has_value(),
        "could not compare the sign repair branch to its damaged parent");
    require(sign_repaired_baseline.has_value(),
        "could not compare the repaired sign branch to baseline");
    require(sign_damage_comparison.value().differences.size() == 1U,
        "sign damage branch did not produce exactly one changed source property");
    require(sign_damage_comparison.value().differences.front().right_value.text_value ==
            "dented-sign",
        "sign damage comparison lost its authored dented-sign value");
    require(sign_repair_comparison.value().differences.size() == 2U,
        "damage-to-repair comparison did not preserve both damage and maintenance changes");
    require(sign_repair_comparison.value().common_ancestor_branch_id == 1U &&
            sign_repaired_baseline.value().differences.size() == 1U &&
            sign_repaired_baseline.value().differences.front().property ==
                carto::condition::StateProperty::maintenance &&
            sign_repaired_baseline.value().differences.front().right_value.text_value ==
                "improvised-repair",
        "repair comparison failed to preserve its parent lineage or restored baseline state");
    const auto after_sign_branches = session.condition_authoring();
    require(after_sign_branches &&
            after_sign_branches.value().surface_states[1U].damage ==
                carto::condition::DamageState::intact &&
            after_sign_branches.value().surface_states[1U].maintenance ==
                carto::condition::MaintenanceState::aging,
        "authored damage/repair alternatives mutated the sign's source baseline");
    require(static_cast<bool>(ui.dispatch(carto::application::UndoAction{})) &&
            ui.snapshot().condition.state_branch_count == 1U &&
            static_cast<bool>(ui.dispatch(carto::application::RedoAction{})) &&
            ui.snapshot().condition.state_branch_count == 2U,
        "Motel 8 repair-state authoring did not undo and redo as one journaled edit");

    auto current = ui.snapshot();
    carto::condition::InteractionAnchor anchor;
    anchor.asset_id = authoring.assets[2U].id;
    anchor.kind = carto::condition::AnchorKind::interaction;
    anchor.interaction_class = "motel.room.entry";
    anchor.transform.translation = {0.0, 0.0, -2.5};
    anchor.approach_direction = {0.0, 0.0, 1.0};
    anchor.clearance = {1.2, 2.0, 1.2};
    const auto before_anchor = current;
    require(static_cast<bool>(ui.add_condition_interaction_anchor(
            anchor, current.project_revision, current.condition.authored_revision)),
        "native UI could not author the Motel 8 room entry anchor");
    current = ui.snapshot();
    require(current.project_revision == before_anchor.project_revision.next() &&
            current.condition.authored_revision ==
                before_anchor.condition.authored_revision.next() &&
            current.operations.back().action == "Add CONDITION Interaction Anchor",
        "Motel 8 anchor did not advance one journaled authoring transaction");

    carto::condition::PrefabRecord prefab;
    prefab.name = "Motel 8 Room Assembly";
    prefab.asset_ids = {authoring.assets[1U].id, authoring.assets[2U].id};
    prefab.sockets = {"room.entry", "room.sleep"};
    const auto before_prefab = current;
    require(static_cast<bool>(ui.add_condition_prefab(
            prefab, current.project_revision, current.condition.authored_revision)),
        "native UI could not author the Motel 8 room prefab");
    current = ui.snapshot();
    require(current.project_revision == before_prefab.project_revision.next() &&
            current.condition.authored_revision ==
                before_prefab.condition.authored_revision.next() &&
            current.operations.back().action == "Add CONDITION Prefab",
        "Motel 8 prefab did not advance one journaled authoring transaction");
    const auto prefab_id = current.condition.prefabs.back().id;

    const auto before_instance = current;
    require(static_cast<bool>(ui.instantiate_condition_prefab(
            prefab_id, "Motel 8 Room Study A", {24.0, 0.0, 12.0},
            current.project_revision, current.condition.authored_revision)),
        "native UI could not place the source-linked Motel 8 prefab");
    current = ui.snapshot();
    require(current.project_revision == before_instance.project_revision.next() &&
            current.condition.authored_revision ==
                before_instance.condition.authored_revision.next() &&
            current.condition.prefab_instances.size() == 1U &&
            current.condition.prefab_instances.front().current &&
            current.condition.prefab_instances.front().member_count == 2U &&
            current.objects.size() == 7U &&
            current.operations.back().action == "Instantiate CONDITION Prefab",
        "Motel 8 prefab placement did not atomically bind two source members and one root");
    require(!current.condition.stale && current.condition.publication_ready &&
            static_cast<bool>(ui.build_condition_package()),
        "current Motel 8 prefab instance blocked package publication");
    const auto placed_instance = current.condition.prefab_instances.front();
    const auto before_instance_undo = current;
    require(static_cast<bool>(ui.dispatch(carto::application::UndoAction{})),
        "could not undo the integrated Motel 8 prefab placement");
    require(ui.snapshot().objects.size() == 4U &&
            ui.snapshot().condition.prefab_instances.empty(),
        "Motel 8 instance undo left scene objects or its source record behind");
    require(static_cast<bool>(ui.dispatch(carto::application::RedoAction{})),
        "could not redo the integrated Motel 8 prefab placement");
    current = ui.snapshot();
    require(current.objects.size() == 7U &&
            current.condition.prefab_instances.size() == 1U &&
            current.condition.prefab_instances.front().id == placed_instance.id &&
            current.condition.prefab_instances.front().current &&
            current.project_revision > before_instance_undo.project_revision,
        "Motel 8 instance redo did not restore the exact source-linked placement");

    carto::condition::CollectionRecord collection;
    collection.name = "Motel 8 Production Set";
    collection.asset_ids = {authoring.assets[0U].id,
                            authoring.assets[1U].id,
                            authoring.assets[2U].id};
    collection.semantic_class = "condition.motel8.production";
    const auto before_collection = current;
    require(static_cast<bool>(ui.add_condition_collection(
            collection, current.project_revision, current.condition.authored_revision)),
        "native UI could not author the Motel 8 production collection");
    current = ui.snapshot();
    require(current.project_revision == before_collection.project_revision.next() &&
            current.condition.authored_revision ==
                before_collection.condition.authored_revision.next() &&
            current.operations.back().action == "Add CONDITION Collection",
        "Motel 8 collection did not advance one journaled authoring transaction");

    carto::condition::RegionKitManifest region_kit;
    region_kit.name = "Motel 8 Region Kit";
    region_kit.asset_ids = collection.asset_ids;
    region_kit.prefab_ids = {prefab_id};
    region_kit.required_classes = {carto::condition::AssetClass::architecture,
                                   carto::condition::AssetClass::prop};
    region_kit.interaction_coverage = 0.75;
    region_kit.familiarity_coverage = 0.75;
    const auto before_region_kit = current;
    require(static_cast<bool>(ui.add_condition_region_kit(
            region_kit, current.project_revision, current.condition.authored_revision)),
        "native UI could not author the Motel 8 region kit");
    current = ui.snapshot();
    require(current.project_revision == before_region_kit.project_revision.next() &&
            current.condition.authored_revision ==
                before_region_kit.condition.authored_revision.next() &&
            current.operations.back().action == "Add CONDITION Region Kit",
        "Motel 8 region kit did not advance one journaled authoring transaction");

    const auto before_undo = current;
    require(static_cast<bool>(ui.dispatch(carto::application::UndoAction{})),
        "could not undo the final Motel 8 authoring transaction");
    require(ui.snapshot().condition.region_kits.size() + 1U ==
            before_undo.condition.region_kits.size(),
        "Motel 8 region-kit authoring was not independently undoable");
    require(static_cast<bool>(ui.dispatch(carto::application::RedoAction{})),
        "could not redo the final Motel 8 authoring transaction");
    current = ui.snapshot();
    require(current.condition.region_kits.size() == before_undo.condition.region_kits.size(),
        "Motel 8 region-kit redo did not restore the authored record");
    require(static_cast<bool>(ui.dispatch(
                carto::application::SaveProjectAction{project_path})),
        "could not save the integrated Motel 8 QA project");

    const auto reopened = carto::project::ProjectDocument::load(project_path);
    require(reopened.has_value(), "integrated Motel 8 QA project does not reopen");
    require(reopened.value().scene().objects_sorted().size() == 7U,
        "integrated Motel 8 QA project did not preserve source objects and prefab instance");
    require(reopened.value().condition_authoring().has_value(),
        "integrated Motel 8 QA project is missing CONDITION authoring");
    const auto reopened_authoring = carto::condition::AuthoringDocument::deserialize(
        reopened.value().condition_authoring()->bytes);
    require(reopened_authoring.has_value(), "reopened Motel 8 authoring is invalid");
    require(reopened_authoring.value().audio_sources.size() == 1U &&
            reopened_authoring.value().acoustic_zones.size() == 1U &&
            reopened_authoring.value().interaction_anchors.size() ==
                current.condition.interaction_anchor_count &&
            reopened_authoring.value().prefabs.size() == current.condition.prefab_count &&
            reopened_authoring.value().prefab_instances.size() ==
                current.condition.prefab_instance_count &&
            reopened_authoring.value().prefab_instances.size() == 1U &&
            reopened_authoring.value().collections.size() == current.condition.collection_count &&
            reopened_authoring.value().region_kits.size() == current.condition.region_kit_count,
        "reopened Motel 8 authoring lost audio, anchors, prefab, collection, or kit records");
    require(reopened_authoring.value().kinematic_assemblies.size() == 1U &&
            reopened_authoring.value().kinematic_assemblies.front().asset_id ==
                authoring.assets[2U].id &&
            reopened_authoring.value().kinematic_assemblies.front().graph_bytes ==
                door_graph.serialize() &&
            reopened_authoring.value().kinematic_assemblies.front().joint_limit_bytes ==
                revised_limits.serialize() &&
            reopened_authoring.value().kinematic_assemblies.front().part_bindings ==
                door_assembly.part_bindings,
        "reopened Motel 8 project lost or changed the revisioned articulation source");
    require(reopened_authoring.value().surface_states.size() == 2U &&
            reopened_authoring.value().surface_states[1U].asset_id == authoring.assets[1U].id &&
            reopened_authoring.value().state_branches.size() == 2U,
        "reopened Motel 8 project lost sign condition or damage/repair branches");
    const auto reopened_objects = reopened.value().scene().objects_sorted();
    const auto reopened_sign_object = std::find_if(
        reopened_objects.begin(), reopened_objects.end(), [&](const auto& object) {
            return object.id == owner_objects[1U];
        });
    require(reopened_sign_object != reopened_objects.end() &&
            reopened_sign_object->mesh_asset.has_value() &&
            *reopened_sign_object->mesh_asset == sign_mesh_asset,
        "reopened Motel 8 sign object lost its project-owned mesh binding");
    const std::uint64_t reopened_sign_mesh_asset = *reopened_sign_object->mesh_asset;
    const std::string sign_source_snapshot = sign_source.serialize();
    const auto sign_source_digest = carto::assets::sha256(
        std::span<const std::uint8_t>{
            reinterpret_cast<const std::uint8_t*>(sign_source_snapshot.data()),
            sign_source_snapshot.size()});
    const auto reopened_sign_source = reopened.value().sign_text_sources().find(
        sign_source.source_identity);
    require(reopened_sign_source != reopened.value().sign_text_sources().end() &&
            reopened_sign_source->second.serialize() == sign_source_snapshot,
        "reopened project lost or changed its revisioned Motel 8 sign source");
    const auto reopened_sign_receipt =
        reopened.value().sign_text_instance_receipts().find(sign_object->object.id);
    require(reopened_sign_receipt !=
                reopened.value().sign_text_instance_receipts().end() &&
            reopened_sign_receipt->second.mesh_asset == reopened_sign_mesh_asset &&
            reopened_sign_receipt->second.source_identity == sign_source.source_identity &&
            reopened_sign_receipt->second.source_revision == sign_source.source_revision &&
            reopened_sign_receipt->second.source_snapshot == sign_source_snapshot &&
            reopened_sign_receipt->second.source_digest == sign_source_digest,
        "reopened sign mesh lost the exact source snapshot and digest receipt");

    const auto& reopened_materials = reopened.value().material_catalog();
    const auto reopened_panel_material =
        reopened_materials.materials().find(panel_material_id);
    const auto reopened_lettering_material =
        reopened_materials.materials().find(lettering_material_id);
    const auto reopened_panel_assignment =
        reopened_materials.assignments().find(panel_region);
    const auto reopened_lettering_assignment =
        reopened_materials.assignments().find(lettering_region);
    require(reopened_panel_material != reopened_materials.materials().end() &&
            reopened_lettering_material != reopened_materials.materials().end() &&
            reopened_panel_assignment != reopened_materials.assignments().end() &&
            reopened_lettering_assignment != reopened_materials.assignments().end() &&
            reopened_panel_assignment->second.material == panel_material_id &&
            reopened_lettering_assignment->second.material == lettering_material_id &&
            reopened_lettering_material->second.definition.emissive_factor.x == 0.85 &&
            reopened_lettering_material->second.definition.emissive_factor.y == 0.24 &&
            reopened_lettering_material->second.definition.emissive_factor.z == 0.035 &&
            reopened_lettering_material->second.definition.emissive_strength == 3.0,
        "reopened sign materials lost independent slot assignments or emissive source factors");
    const auto& reopened_presentation = reopened.value().presentation_catalog();
    const auto reopened_camera =
        reopened_presentation.cameras().find(owner_objects[0U]);
    const auto reopened_light =
        reopened_presentation.lights().find(owner_objects[1U]);
    require(reopened_camera != reopened_presentation.cameras().end() &&
            reopened_camera->second.identity == review_camera.identity &&
            reopened_camera->second.bookmark_identity == review_camera.bookmark_identity &&
            reopened_camera->second.active_for_review &&
            reopened_light != reopened_presentation.lights().end() &&
            reopened_light->second.identity == sign_light.identity &&
            reopened_light->second.semantic_class == sign_light.semantic_class &&
            reopened_light->second.intensity == sign_light.intensity,
        "reopened Motel 8 review camera or fixture-light intent was not preserved");
    for (std::size_t index = 0U; index < owner_objects.size(); ++index) {
        require(reopened_authoring.value().assets[index].authored_object_id ==
                owner_objects[index].value,
            "reopened Motel 8 asset binding no longer matches its source scene object");
    }

    const std::string serialized_project = reopened.value().serialize();
    require(serialized_project.find(fixture_root.string()) == std::string::npos &&
            serialized_project.find("RIFF") == std::string::npos &&
            serialized_project.find("synthetic-acoustic-tone.wav") == std::string::npos,
        "project leaked a workstation path or embedded WAVE sample bytes");

    const auto package = carto::condition::Package::publish(
        reopened_authoring.value(), reopened.value().revision(),
        reopened_authoring.value().project_namespace);
    require(package.has_value(), "generated acoustic QA package did not publish");
    const auto admission = carto::condition::admit_for_consumer(package.value());
    require(admission && admission.value().status ==
            carto::condition::PackageAdmissionStatus::admit,
        "integrated Motel 8 package failed consumer admission");

    const auto replacement_identity = std::string_view("condition-motel8-source-v2");
    const auto replacement_digest = carto::assets::sha256(
        std::span<const std::uint8_t>{
            reinterpret_cast<const std::uint8_t*>(replacement_identity.data()),
            replacement_identity.size()});
    require(static_cast<bool>(ui.invalidate_condition_source(
            1U, "fixture-revision-2", replacement_digest)),
        "could not exercise source invalidation in the integrated Motel 8 fixture");
    const auto invalidated = ui.snapshot();
    require(invalidated.condition.stale && !invalidated.condition.publication_ready &&
            !ui.build_condition_package(),
        "integrated Motel 8 fixture still published after source invalidation");
    require(static_cast<bool>(ui.dispatch(carto::application::UndoAction{})),
        "could not undo the Motel 8 source invalidation");
    const auto restored_source = ui.snapshot();
    require(!restored_source.condition.stale && restored_source.condition.publication_ready &&
            static_cast<bool>(ui.build_condition_package()),
        "undo did not restore current Motel 8 evidence and package readiness");
    require(static_cast<bool>(ui.dispatch(carto::application::RedoAction{})),
        "could not redo the Motel 8 source invalidation");
    const auto redone_invalidation = ui.snapshot();
    require(redone_invalidation.condition.stale &&
            !redone_invalidation.condition.publication_ready &&
            !ui.build_condition_package(),
        "redo did not restore fail-closed Motel 8 source invalidation");

    require(static_cast<bool>(ui.dispatch(carto::application::UndoAction{})),
        "could not restore the current Motel 8 state for the integrated fixture");
    const auto current_fixture = ui.snapshot();
    require(!current_fixture.condition.stale &&
            current_fixture.condition.publication_ready &&
            static_cast<bool>(ui.build_condition_package()),
        "integrated Motel 8 fixture did not return to current source evidence");
    require(static_cast<bool>(ui.dispatch(
                carto::application::SaveProjectAction{project_path})),
        "could not persist the final current Motel 8 journal state");
    carto::application::ApplicationSession current_reopened_session;
    carto::ui::UiController current_reopened_ui(current_reopened_session);
    require(static_cast<bool>(current_reopened_ui.dispatch(
                carto::application::OpenProjectAction{project_path})),
        "native application could not reopen the current integrated Motel 8 project");
    const auto current_application_reopened = current_reopened_ui.snapshot();
    require(current_application_reopened.project_revision == current_fixture.project_revision &&
            !current_application_reopened.condition.stale &&
            current_application_reopened.condition.publication_ready &&
            current_application_reopened.objects.size() == 7U &&
            current_application_reopened.condition.prefab_instances.size() == 1U &&
            current_application_reopened.condition.prefab_instances.front().current,
        "native reopen did not preserve the current journaled Motel 8 source state");

    carto::application::ApplicationSession stale_session;
    carto::ui::UiController stale_ui(stale_session);
    require(static_cast<bool>(stale_ui.dispatch(
                carto::application::OpenProjectAction{project_path})),
        "could not open the current Motel 8 project for the stale-source branch");
    require(static_cast<bool>(stale_ui.dispatch(
                carto::application::SaveProjectAction{stale_project_path})),
        "could not create an independent Motel 8 stale-source journal baseline");
    require(static_cast<bool>(stale_ui.invalidate_condition_source(
                1U, "fixture-revision-2", replacement_digest)),
        "could not invalidate Motel 8 source evidence in the isolated stale branch");
    const auto final_stale_fixture = stale_ui.snapshot();
    require(final_stale_fixture.condition.stale &&
            !final_stale_fixture.condition.publication_ready &&
            !stale_ui.build_condition_package(),
        "stale Motel 8 negative fixture became publication-ready before save");
    require(static_cast<bool>(stale_ui.dispatch(
                carto::application::SaveProjectAction{stale_project_path})),
        "could not persist the isolated stale Motel 8 journal state");
    carto::application::ApplicationSession reopened_session;
    carto::ui::UiController reopened_ui(reopened_session);
    require(static_cast<bool>(reopened_ui.dispatch(
                carto::application::OpenProjectAction{stale_project_path})),
        "native application could not reopen the final Motel 8 journal state");
    const auto application_reopened = reopened_ui.snapshot();
    require(application_reopened.project_revision == final_stale_fixture.project_revision &&
            application_reopened.condition.stale &&
            !application_reopened.condition.publication_ready &&
            application_reopened.objects.size() == 7U &&
            application_reopened.condition.prefab_instances.size() == 1U &&
            !application_reopened.condition.prefab_instances.front().current,
        "native application reopen did not preserve the final journaled Motel 8 state");

    carto::application::ApplicationSession final_current_session;
    carto::ui::UiController final_current_ui(final_current_session);
    require(static_cast<bool>(final_current_ui.dispatch(
                carto::application::OpenProjectAction{project_path})),
        "stale-source fixture generation damaged the integrated Motel 8 project journal");
    const auto final_current_reopened = final_current_ui.snapshot();
    require(final_current_reopened.project_revision == current_fixture.project_revision &&
            !final_current_reopened.condition.stale &&
            final_current_reopened.condition.publication_ready &&
            final_current_reopened.condition.prefab_instances.size() == 1U &&
            final_current_reopened.condition.prefab_instances.front().current,
        "integrated Motel 8 project changed while generating its stale-source branch");

    run_gas_station_effect_fixture(fixture_root);
    run_biological_condition_fixture(fixture_root);
    run_pine_ridge_price_board_fixture(fixture_root);
    run_safe_place_state_fixture(fixture_root);
    run_general_store_fixture(fixture_root);
    std::cout << "biological_fixture_project="
              << (fixture_root / "condition-wildlife-human.carto").string()
              << " biological_sources=3 proxy_instances=3 coverings=fur,hair,feather"
              << " sculpt_stack=1 private_mesh_evaluation=false"
              << " private_runtime=false synthetic_coarse_proxy=true\n"
              << "fixture_project=" << project_path.string() << '\n'
              << "stale_fixture_project=" << stale_project_path.string() << '\n'
              << "fixture_wave=" << wave_path.string() << '\n'
              << "wave_digest=" << wave_digest.hex() << '\n'
              << "scene_objects=7 bound_assets=3 audio_source=1 acoustic_zone=1"
              << " ui_authored_anchor=1 prefab=1 prefab_instance=1 collection=1 region_kit=1"
              << " stale_source_rejected=true"
              << " application_reopen=true journal_tail_matches=true"
              << " synthetic_fixture=true runtime_simulation=false\n";
}

} // namespace

int main(int argc, char** argv) {
    try {
        if (argc != 2) {
            throw std::runtime_error(
                "usage: cartographer_condition_authoring_ui_fixture_tests <output-root>");
        }
        run(argv[1]);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
