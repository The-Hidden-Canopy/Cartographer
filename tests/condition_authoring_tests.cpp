#include <carto/application/application.hpp>
#include <carto/condition/authoring.hpp>
#include <carto/condition/effect_preview.hpp>
#include <carto/condition/vegetation_preview.hpp>
#include <carto/condition/state_branch.hpp>
#include <carto/condition/wave_import.hpp>
#include <carto/geometry/sign.hpp>
#include <carto/ui/ui.hpp>

#include <array>
#include <algorithm>
#include <bit>
#include <charconv>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <map>
#include <span>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>

namespace {

class TestFailure final : public std::runtime_error {
public:
    explicit TestFailure(const std::string& message) : std::runtime_error(message) {}
};

#define REQUIRE(condition)                                                                  \
    do {                                                                                    \
        if (!(condition)) throw TestFailure(std::string("requirement failed: ") + #condition); \
    } while (false)

carto::assets::Sha256Digest digest(std::string_view value) {
    return carto::assets::sha256(std::span<const std::uint8_t>{
        reinterpret_cast<const std::uint8_t*>(value.data()), value.size()});
}

void append_le16(std::string& output, std::uint16_t value) {
    output.push_back(static_cast<char>(value & 0xffU));
    output.push_back(static_cast<char>((value >> 8U) & 0xffU));
}

void append_le32(std::string& output, std::uint32_t value) {
    for (unsigned int shift = 0U; shift < 32U; shift += 8U) {
        output.push_back(static_cast<char>((value >> shift) & 0xffU));
    }
}

std::string make_wave(
    std::uint16_t format_tag,
    std::uint16_t channels,
    std::uint32_t sample_rate,
    std::uint16_t bits_per_sample,
    std::string audio_data,
    bool add_odd_unknown_chunk = false) {
    const std::uint16_t block_align = static_cast<std::uint16_t>(
        channels * (bits_per_sample / 8U));
    std::string chunks;
    if (add_odd_unknown_chunk) {
        chunks += "JUNK";
        append_le32(chunks, 1U);
        chunks.push_back('x');
        chunks.push_back('\0');
    }
    chunks += "fmt ";
    append_le32(chunks, 16U);
    append_le16(chunks, format_tag);
    append_le16(chunks, channels);
    append_le32(chunks, sample_rate);
    append_le32(chunks, sample_rate * block_align);
    append_le16(chunks, block_align);
    append_le16(chunks, bits_per_sample);
    chunks += "data";
    append_le32(chunks, static_cast<std::uint32_t>(audio_data.size()));
    chunks += audio_data;
    if ((audio_data.size() & 1U) != 0U) chunks.push_back('\0');
    std::string bytes = "RIFF";
    append_le32(bytes, static_cast<std::uint32_t>(chunks.size() + 4U));
    bytes += "WAVE";
    bytes += chunks;
    return bytes;
}

carto::condition::AuthoringDocument gas_station_effect_fixture() {
    auto fixture = carto::condition::make_condition_fixture();
    if (!fixture) throw TestFailure(fixture.error().message);
    auto& document = fixture.value();
    document.project_namespace = "condition.fixture.gas-station-effects";
    auto& source = document.sources.front();
    source.label = "Gas Station deterministic effect fixture";
    source.logical_locator = "fixture://condition/gas-station-effects";
    source.revision = "gas-station-effects-1";
    source.digest = digest("gas-station-effects-source-v1");
    source.import_settings_digest = digest("gas-station-effects-import-v1");
    for (auto& asset : document.assets) {
        asset.source_revision = source.revision;
        asset.source_digest = source.digest;
    }
    document.assets[0].stable_name = "gas_station_compressor";
    document.assets[1].stable_name = "gas_station_runoff_edge";
    document.assets[2].stable_name = "gas_station_service_vehicle";
    document.assets[1].asset_class = carto::condition::AssetClass::terrain;
    document.assets[2].asset_class = carto::condition::AssetClass::vehicle;

    document.effect_sources = {
        {1U, 1U, "gas-station.compressor.steam-air", carto::condition::EffectEmissionShape::point,
            carto::core::Transform::identity(), {}, 0.0,
            carto::condition::EffectIntentClass::physical_state,
            "compressor.running", "compressor.off", true},
        {2U, 2U, "gas-station.runoff.splash", carto::condition::EffectEmissionShape::box,
            carto::core::Transform::identity(), {1.5, 0.15, 0.4}, 0.0,
            carto::condition::EffectIntentClass::physical_state,
            "rain.active", "rain.stopped", true},
        {3U, 3U, "gas-station.vehicle.smoke", carto::condition::EffectEmissionShape::sphere,
            carto::core::Transform::identity(), {}, 0.25,
            carto::condition::EffectIntentClass::physical_state,
            "engine.running", "engine.stopped", true},
    };
    document.particle_emitters = {
        {1U, 1U, 5.0, 2.0, 0.0, 4.0, 0U, 0.0, {}, {}, {0.0, 0.7, 0.0},
            1.0 / 60.0, 64U, 501U, true},
        {2U, 2U, 12.0, 0.8, 0.0, 4.0, 8U, 0.1, {0.0, 0.6, 0.0},
            {0.2, 0.1, 0.2}, {}, 1.0 / 60.0, 96U, 502U, true},
        {3U, 3U, 3.0, 2.5, 0.0, 4.0, 0U, 0.0, {0.0, 0.2, 0.0},
            {0.1, 0.1, 0.1}, {0.0, 0.25, 0.0}, 1.0 / 60.0, 64U, 503U, true},
    };
    document.field_influences = {
        {1U, 1U, carto::condition::EffectInfluenceKind::wind, "gas_station.wind_u",
            carto::core::Revision(1U), digest("gas-station-wind-u-v1"),
            {1.0, 0.0, 0.0}, 0.35},
        {2U, 2U, carto::condition::EffectInfluenceKind::drainage, "gas_station.drainage",
            carto::core::Revision(1U), digest("gas-station-drainage-v1"),
            {0.0, -1.0, 0.0}, 0.2},
        {3U, 3U, carto::condition::EffectInfluenceKind::turbulence, "gas_station.wind_v",
            carto::core::Revision(1U), digest("gas-station-wind-v-v1"),
            {0.0, 0.0, 1.0}, 0.1},
    };
    return std::move(document);
}

void wave_clip_inspection_is_bounded_and_fail_closed() {
    const std::string pcm_data{"\x01\x00\xff\x7f\x02\x00\xfe\x7f", 8U};
    const auto pcm = make_wave(1U, 2U, 48000U, 16U, pcm_data, true);
    const auto bytes = std::span<const std::uint8_t>{
        reinterpret_cast<const std::uint8_t*>(pcm.data()), pcm.size()};
    const auto inspection = carto::condition::inspect_wave_clip(bytes);
    REQUIRE(inspection);
    REQUIRE(inspection.value().encoding == carto::condition::WaveSampleEncoding::pcm_integer);
    REQUIRE(inspection.value().channels == 2U);
    REQUIRE(inspection.value().sample_rate == 48000U);
    REQUIRE(inspection.value().bits_per_sample == 16U);
    REQUIRE(inspection.value().frame_count == 2U);
    REQUIRE(inspection.value().audio_bytes == 8U);
    REQUIRE(inspection.value().duration_seconds() > 0.00004);

    const auto audition = carto::condition::make_wave_audition_excerpt(bytes);
    REQUIRE(audition);
    const auto audition_inspection = carto::condition::inspect_wave_clip(audition.value());
    REQUIRE(audition_inspection);
    REQUIRE(audition_inspection.value().encoding ==
        carto::condition::WaveSampleEncoding::pcm_integer);
    REQUIRE(audition_inspection.value().channels == 2U);
    REQUIRE(audition_inspection.value().bits_per_sample == 16U);
    REQUIRE(audition_inspection.value().sample_rate == 48000U);
    REQUIRE(audition_inspection.value().frame_count == 2U);
    REQUIRE(audition.value().size() == 52U);
    REQUIRE(!carto::condition::make_wave_audition_excerpt(bytes, 0.0));
    REQUIRE(!carto::condition::make_wave_audition_excerpt(bytes,
        std::numeric_limits<double>::quiet_NaN()));
    const std::string five_seconds_of_silence(48000U * 5U * 2U, '\0');
    const auto long_wave = make_wave(1U, 1U, 48000U, 16U, five_seconds_of_silence);
    const auto long_audition = carto::condition::make_wave_audition_excerpt(
        std::span<const std::uint8_t>{
            reinterpret_cast<const std::uint8_t*>(long_wave.data()), long_wave.size()});
    REQUIRE(long_audition);
    const auto bounded_audition = carto::condition::inspect_wave_clip(long_audition.value());
    REQUIRE(bounded_audition);
    REQUIRE(bounded_audition.value().duration_seconds() <=
        carto::condition::kMaxWaveAuditionSeconds);
    REQUIRE(bounded_audition.value().frame_count == 48000U * 3U);

    auto bad_riff = pcm;
    bad_riff[0] = 'X';
    REQUIRE(!carto::condition::inspect_wave_clip(std::span<const std::uint8_t>{
        reinterpret_cast<const std::uint8_t*>(bad_riff.data()), bad_riff.size()}));
    auto bad_size = pcm;
    bad_size[4] = static_cast<char>(bad_size[4] + 1);
    REQUIRE(!carto::condition::inspect_wave_clip(std::span<const std::uint8_t>{
        reinterpret_cast<const std::uint8_t*>(bad_size.data()), bad_size.size()}));

    auto bad_alignment = pcm;
    bad_alignment[32] = 1;
    bad_alignment[33] = 0;
    REQUIRE(!carto::condition::inspect_wave_clip(std::span<const std::uint8_t>{
        reinterpret_cast<const std::uint8_t*>(bad_alignment.data()), bad_alignment.size()}));

    const auto compressed = make_wave(6U, 1U, 48000U, 16U, std::string{"\0\0", 2U});
    REQUIRE(!carto::condition::inspect_wave_clip(std::span<const std::uint8_t>{
        reinterpret_cast<const std::uint8_t*>(compressed.data()), compressed.size()}));

    const auto floating_nan = make_wave(3U, 1U, 48000U, 32U,
        std::string{"\0\0\xc0\x7f", 4U});
    REQUIRE(!carto::condition::inspect_wave_clip(std::span<const std::uint8_t>{
        reinterpret_cast<const std::uint8_t*>(floating_nan.data()), floating_nan.size()}));

    std::vector<std::uint8_t> oversized(carto::condition::kMaxWaveClipBytes + 1U);
    REQUIRE(!carto::condition::inspect_wave_clip(oversized));
}

void fixture_round_trips_and_publishes() {
    const auto fixture = carto::condition::make_condition_fixture();
    if (!fixture) throw TestFailure(fixture.error().message);
    REQUIRE(fixture.value().sources.size() == 1U);
    REQUIRE(fixture.value().assets.size() == 3U);
    REQUIRE(fixture.value().safe_places.size() == 1U);
    REQUIRE(fixture.value().familiarity.size() == 1U);
    REQUIRE(fixture.value().variants.size() == 2U);
    REQUIRE(fixture.value().region_kits.size() == 1U);

    const auto bytes = fixture.value().serialize();
    const auto reopened = carto::condition::AuthoringDocument::deserialize(bytes);
    REQUIRE(reopened);
    REQUIRE(reopened.value().serialize() == bytes);
    REQUIRE(reopened.value().canonical_digest().value() == fixture.value().canonical_digest().value());

    const auto package = carto::condition::Package::publish(
        reopened.value(), carto::core::Revision(4U), reopened.value().project_namespace);
    REQUIRE(package);
    const auto package_reopened = carto::condition::Package::deserialize(package.value().serialize());
    REQUIRE(package_reopened);
    const auto admission = carto::condition::admit_for_consumer(package_reopened.value());
    REQUIRE(admission);
    REQUIRE(admission.value().status == carto::condition::PackageAdmissionStatus::admit);
    REQUIRE(admission.value().admitted_authoring_digest == package.value().authoring_digest);
}

void physical_boundary_rejects_presentation_mutation() {
    auto fixture = carto::condition::make_condition_fixture();
    if (!fixture) throw TestFailure(fixture.error().message);
    auto invalid_variant = fixture.value().variants.at(1U);
    invalid_variant.collision_impact_declared = true;
    fixture.value().variants.at(1U) = invalid_variant;
    REQUIRE(!fixture.value().validate());
}

void invalidation_preserves_source_identity_and_blocks_publication() {
    auto fixture = carto::condition::make_condition_fixture();
    if (!fixture) throw TestFailure(fixture.error().message);
    const auto old_asset = fixture.value().assets.front();
    REQUIRE(fixture.value().invalidate_source(1U, "fixture-2", digest("replacement-source")));
    REQUIRE(fixture.value().assets.front().id == old_asset.id);
    REQUIRE(fixture.value().has_stale_sources());
    REQUIRE(fixture.value().project_revision == carto::core::Revision(1U));
    REQUIRE(!carto::condition::Package::publish(
        fixture.value(), carto::core::Revision(1U), fixture.value().project_namespace));
}

void invalidation_refuses_revision_exhaustion_without_partial_change() {
    auto fixture = carto::condition::make_condition_fixture();
    REQUIRE(fixture);
    fixture.value().project_revision = carto::core::Revision(
        carto::core::Revision::max_value() - 1U);
    REQUIRE(fixture.value().validate());
    const auto before = fixture.value().serialize();
    REQUIRE(!fixture.value().invalidate_source(
        1U, "fixture-next", digest("condition-next-source")));
    REQUIRE(fixture.value().serialize() == before);
}

carto::condition::AuthoringDocument fixture_with_audio_source_evidence() {
    auto fixture = carto::condition::make_condition_fixture();
    if (!fixture) throw TestFailure(fixture.error().message);
    fixture.value().sources.push_back(carto::condition::SourceEvidence{
        2U, carto::condition::SourceKind::imported_asset, "Motel AC hum clip",
        "audio/motel8/ac-hum.wav", "fixture-audio-1", digest("motel8-ac-hum-bytes"),
        "Cartographer fixture", "1", digest("motel8-audio-import-settings"), true, true, {}});
    return std::move(fixture.value());
}

carto::condition::AuthoringDocument fixture_with_general_store_audio_evidence() {
    auto fixture = carto::condition::make_condition_fixture();
    if (!fixture) throw TestFailure(fixture.error().message);
    const auto fixture_source = fixture.value().sources.front();
    for (const auto& [id, name] : std::array<std::pair<carto::condition::Id, std::string_view>, 3>{
             {{2U, "refrigerator"}, {3U, "freezer"}, {4U, "fluorescent"}}}) {
        const std::string key = "general-store-" + std::string(name);
        fixture.value().sources.push_back(carto::condition::SourceEvidence{
            id, carto::condition::SourceKind::imported_asset,
            "General Store " + std::string(name) + " clip",
            "audio/general-store/" + std::string(name) + ".wav", "fixture-audio-1",
            digest(key + "-bytes"), "Cartographer fixture", "1",
            digest(key + "-import-settings"), true, true, {}});
        const carto::condition::Id asset_id = id + 2U;
        fixture.value().assets.push_back(carto::condition::SourceAsset{
            asset_id, "general_store_" + std::string(name),
            carto::condition::AssetClass::fixture,
            carto::condition::FidelityClass::standard, 1U,
            fixture_source.revision, fixture_source.digest,
            digest(key + "-geometry"), digest(key + "-material"), asset_id,
            {"audio-source", "fixture"}, {}});
    }
    return std::move(fixture.value());
}

void authored_audio_sources_and_acoustic_zones_round_trip_and_fail_closed() {
    auto fixture = fixture_with_audio_source_evidence();
    fixture.audio_sources.push_back(carto::condition::AuthoredAudioSource{
        1U, 3U, 2U, carto::core::Transform::identity(), 0.8, 1.0,
        true, true, 0.5, 18.0, 100U, carto::condition::AudioBusClass::machinery,
        carto::condition::AudioOcclusionIntent::static_geometry, 0.25,
        "operational", "motel.room.ac-hum", true});
    fixture.acoustic_zones.push_back(carto::condition::AuthoredAcousticZone{
        1U, 3U, carto::condition::AcousticZoneShape::box,
        carto::core::Transform::identity(), {2.0, 1.5, 3.0}, 0.0,
        "small-room", 0.9, "motel-room", 10U, "motel8.room.101", true});
    REQUIRE(fixture.validate());

    const auto bytes = fixture.serialize();
    const auto reopened = carto::condition::AuthoringDocument::deserialize(bytes);
    REQUIRE(reopened);
    REQUIRE(reopened.value().audio_sources.size() == 1U);
    REQUIRE(reopened.value().acoustic_zones.size() == 1U);
    REQUIRE(reopened.value().serialize() == bytes);
    REQUIRE(reopened.value().canonical_digest().value() == fixture.canonical_digest().value());
    const auto package = carto::condition::Package::publish(
        reopened.value(), carto::core::Revision(2U), reopened.value().project_namespace);
    REQUIRE(package);
    const auto admission = carto::condition::admit_for_consumer(package.value());
    REQUIRE(admission);
    REQUIRE(admission.value().status == carto::condition::PackageAdmissionStatus::admit);

    auto missing_clip = fixture;
    missing_clip.audio_sources.front().clip_source_id = 999U;
    REQUIRE(!missing_clip.validate());

    auto non_asset_clip = fixture;
    non_asset_clip.sources.back().kind = carto::condition::SourceKind::authored_fixture;
    REQUIRE(!non_asset_clip.validate());

    auto invalid_gain = fixture;
    invalid_gain.audio_sources.front().gain = std::numeric_limits<double>::quiet_NaN();
    REQUIRE(!invalid_gain.validate());

    auto invalid_zone = fixture;
    invalid_zone.acoustic_zones.front().shape = carto::condition::AcousticZoneShape::sphere;
    REQUIRE(!invalid_zone.validate());

    auto excessive_audio = fixture;
    excessive_audio.audio_sources.resize(
        carto::condition::AuthoringDocument::kMaxAudioRecordsPerKind + 1U);
    REQUIRE(!excessive_audio.validate());

    auto excessive_audio_bytes = fixture.serialize();
    const std::string audio_count = "AUDIO_SOURCES 1";
    const auto audio_count_offset = excessive_audio_bytes.find(audio_count);
    REQUIRE(audio_count_offset != std::string::npos);
    excessive_audio_bytes.replace(audio_count_offset, audio_count.size(),
        "AUDIO_SOURCES " + std::to_string(
            carto::condition::AuthoringDocument::kMaxAudioRecordsPerKind + 1U));
    REQUIRE(!carto::condition::AuthoringDocument::deserialize(excessive_audio_bytes));

    auto stale_clip = fixture;
    REQUIRE(stale_clip.invalidate_source(
        2U, "fixture-audio-2", digest("motel8-ac-hum-replacement")));
    REQUIRE(stale_clip.has_stale_sources());
    REQUIRE(!carto::condition::Package::publish(
        stale_clip, carto::core::Revision(3U), stale_clip.project_namespace));
}

void general_store_audio_fixture_covers_native_source_contract() {
    auto fixture = fixture_with_general_store_audio_evidence();
    const auto source_template = [&](carto::condition::Id id,
                                     carto::condition::Id asset_id,
                                     carto::condition::Id clip_id,
                                     std::string role,
                                     double maximum_distance) {
        carto::condition::AuthoredAudioSource source;
        source.id = id;
        source.asset_id = asset_id;
        source.clip_source_id = clip_id;
        source.gain = 0.45;
        source.loop = true;
        source.minimum_distance = 1.0;
        source.maximum_distance = maximum_distance;
        source.bus = carto::condition::AudioBusClass::machinery;
        source.reverb_send = 0.2;
        source.normal_operating_state = "running";
        source.familiarity_role = std::move(role);
        return source;
    };
    fixture.audio_sources = {
        source_template(1U, 4U, 2U, "store.refrigerator", 14.0),
        source_template(2U, 5U, 3U, "store.freezer", 12.0),
        source_template(3U, 6U, 4U, "store.fluorescent", 7.0),
    };
    fixture.audio_sources[1U].enabled = false;
    fixture.acoustic_zones.push_back(carto::condition::AuthoredAcousticZone{
        1U, 4U, carto::condition::AcousticZoneShape::box,
        carto::core::Transform::identity(), {8.0, 3.0, 6.0}, 0.0,
        "commercial-interior", 1.0, "general-store-room", 10U,
        "general-store.main", true});
    fixture.acoustic_zones.push_back(carto::condition::AuthoredAcousticZone{
        2U, 5U, carto::condition::AcousticZoneShape::sphere,
        carto::core::Transform::identity(), {}, 3.0,
        "cooler", 0.8, "cooler-short", 20U,
        "general-store.cooler", true});
    REQUIRE(fixture.validate());
    const auto reopened = carto::condition::AuthoringDocument::deserialize(fixture.serialize());
    REQUIRE(reopened);
    REQUIRE(reopened.value().audio_sources.size() == 3U);
    REQUIRE(!reopened.value().audio_sources[1U].enabled);
    REQUIRE(reopened.value().acoustic_zones.size() == 2U);
    const auto package = carto::condition::Package::publish(
        reopened.value(), carto::core::Revision(1U), reopened.value().project_namespace);
    REQUIRE(package);
    const auto admission = carto::condition::admit_for_consumer(package.value());
    REQUIRE(admission);
    REQUIRE(admission.value().status == carto::condition::PackageAdmissionStatus::admit);
}

void remove_kinematic_assembly_section_for_legacy_schema(std::string& serialized) {
    const std::string empty_section = "KINEMATIC_ASSEMBLIES 0\n";
    const auto section_offset = serialized.find(empty_section);
    REQUIRE(section_offset != std::string::npos);
    serialized.erase(section_offset, empty_section.size());
}

void remove_prefab_instance_section_for_legacy_schema(std::string& serialized) {
    const auto section_start = serialized.find("PREFAB_INSTANCES ");
    REQUIRE(section_start != std::string::npos);
    const auto next_section = serialized.find("REGION_KITS ", section_start);
    REQUIRE(next_section != std::string::npos);
    serialized.erase(section_start, next_section - section_start);
}

void condition_authoring_v1_reopens_with_empty_audio_sections() {
    const auto fixture = carto::condition::make_condition_fixture();
    REQUIRE(fixture);
    std::string legacy = fixture.value().serialize();
    const std::string audio_sources = "AUDIO_SOURCES 0\n";
    const auto audio_offset = legacy.find(audio_sources);
    REQUIRE(audio_offset != std::string::npos);
    legacy.erase(audio_offset, audio_sources.size());
    const std::string acoustic_zones = "ACOUSTIC_ZONES 0\n";
    const auto zone_offset = legacy.find(acoustic_zones);
    REQUIRE(zone_offset != std::string::npos);
    legacy.erase(zone_offset, acoustic_zones.size());
    for (const std::string_view section : {
             "EFFECT_SOURCES 0\n", "PARTICLE_EMITTERS 0\n", "FIELD_INFLUENCES 0\n",
             "VEGETATION_SCATTERS 0\n", "STATE_BRANCHES 0\n"}) {
        const auto offset = legacy.find(section);
        REQUIRE(offset != std::string::npos);
        legacy.erase(offset, section.size());
    }
    remove_kinematic_assembly_section_for_legacy_schema(legacy);
    remove_prefab_instance_section_for_legacy_schema(legacy);
    const auto header_end = legacy.find('\n');
    REQUIRE(header_end != std::string::npos);
    legacy.replace(0U, header_end, "CARTOGRAPHER_CONDITION_AUTHORING 1");

    const auto migrated = carto::condition::AuthoringDocument::deserialize(legacy);
    REQUIRE(migrated);
    REQUIRE(migrated.value().audio_sources.empty());
    REQUIRE(migrated.value().acoustic_zones.empty());
    REQUIRE(migrated.value().effect_sources.empty());
    REQUIRE(migrated.value().particle_emitters.empty());
    REQUIRE(migrated.value().field_influences.empty());
    REQUIRE(carto::condition::AuthoringDocument::deserialize(migrated.value().serialize()));
}

void condition_authoring_v2_reopens_with_empty_effect_sections() {
    const auto fixture = carto::condition::make_condition_fixture();
    REQUIRE(fixture);
    std::string legacy = fixture.value().serialize();
    for (const std::string_view section : {
             "EFFECT_SOURCES 0\n", "PARTICLE_EMITTERS 0\n", "FIELD_INFLUENCES 0\n",
             "VEGETATION_SCATTERS 0\n", "STATE_BRANCHES 0\n"}) {
        const auto offset = legacy.find(section);
        REQUIRE(offset != std::string::npos);
        legacy.erase(offset, section.size());
    }
    remove_kinematic_assembly_section_for_legacy_schema(legacy);
    remove_prefab_instance_section_for_legacy_schema(legacy);
    const auto header_end = legacy.find('\n');
    REQUIRE(header_end != std::string::npos);
    legacy.replace(0U, header_end, "CARTOGRAPHER_CONDITION_AUTHORING 2");
    const auto migrated = carto::condition::AuthoringDocument::deserialize(legacy);
    REQUIRE(migrated);
    REQUIRE(migrated.value().effect_sources.empty());
    REQUIRE(migrated.value().particle_emitters.empty());
    REQUIRE(migrated.value().field_influences.empty());
    REQUIRE(carto::condition::AuthoringDocument::deserialize(migrated.value().serialize()));
}

void condition_authoring_v3_reopens_with_empty_vegetation_sections() {
    const auto fixture = carto::condition::make_condition_fixture();
    REQUIRE(fixture);
    std::string legacy = fixture.value().serialize();
    const std::string vegetation = "VEGETATION_SCATTERS 0\n";
    const auto offset = legacy.find(vegetation);
    REQUIRE(offset != std::string::npos);
    legacy.erase(offset, vegetation.size());
    const std::string state_branches = "STATE_BRANCHES 0\n";
    const auto state_offset = legacy.find(state_branches);
    REQUIRE(state_offset != std::string::npos);
    legacy.erase(state_offset, state_branches.size());
    remove_kinematic_assembly_section_for_legacy_schema(legacy);
    remove_prefab_instance_section_for_legacy_schema(legacy);
    const auto header_end = legacy.find('\n');
    REQUIRE(header_end != std::string::npos);
    legacy.replace(0U, header_end, "CARTOGRAPHER_CONDITION_AUTHORING 3");
    const auto migrated = carto::condition::AuthoringDocument::deserialize(legacy);
    REQUIRE(migrated);
    REQUIRE(migrated.value().vegetation_scatters.empty());
    REQUIRE(carto::condition::AuthoringDocument::deserialize(migrated.value().serialize()));
}

void condition_authoring_v4_reopens_with_empty_state_branches() {
    const auto fixture = carto::condition::make_condition_fixture();
    REQUIRE(fixture);
    std::string legacy = fixture.value().serialize();
    const std::string state_branches = "STATE_BRANCHES 0\n";
    const auto offset = legacy.find(state_branches);
    REQUIRE(offset != std::string::npos);
    legacy.erase(offset, state_branches.size());
    remove_kinematic_assembly_section_for_legacy_schema(legacy);
    remove_prefab_instance_section_for_legacy_schema(legacy);
    const auto header_end = legacy.find('\n');
    REQUIRE(header_end != std::string::npos);
    legacy.replace(0U, header_end, "CARTOGRAPHER_CONDITION_AUTHORING 4");
    const auto migrated = carto::condition::AuthoringDocument::deserialize(legacy);
    REQUIRE(migrated);
    REQUIRE(migrated.value().state_branches.empty());
    REQUIRE(carto::condition::AuthoringDocument::deserialize(migrated.value().serialize()));
}

void condition_authoring_v8_reopens_with_empty_prefab_instances() {
    const auto fixture = carto::condition::make_condition_fixture();
    REQUIRE(fixture);
    std::string legacy = fixture.value().serialize();
    const std::string instance_section = "PREFAB_INSTANCES 0\n";
    const auto offset = legacy.find(instance_section);
    REQUIRE(offset != std::string::npos);
    legacy.erase(offset, instance_section.size());
    const auto header_end = legacy.find('\n');
    REQUIRE(header_end != std::string::npos);
    legacy.replace(0U, header_end, "CARTOGRAPHER_CONDITION_AUTHORING 8");
    const auto migrated = carto::condition::AuthoringDocument::deserialize(legacy);
    REQUIRE(migrated);
    REQUIRE(migrated.value().prefab_instances.empty());
    REQUIRE(carto::condition::AuthoringDocument::deserialize(migrated.value().serialize()));
}

void authored_state_branches_compare_without_overwriting_source_and_reject_conflicts() {
    auto fixture = carto::condition::make_condition_fixture();
    REQUIRE(fixture);
    auto& authoring = fixture.value();
    carto::condition::AuthoredStateDelta privacy_delta;
    privacy_delta.id = 1U;
    privacy_delta.target_record_id = 1U;
    privacy_delta.target_asset_id = 1U;
    privacy_delta.target_kind = carto::condition::StateTargetKind::safe_place_profile;
    privacy_delta.property = carto::condition::StateProperty::privacy;
    privacy_delta.before.numeric_value = 0.8;
    privacy_delta.after.numeric_value = 0.65;
    carto::condition::AuthoredStateBranch maintenance;
    maintenance.id = 1U;
    maintenance.name = "maintenance-2024";
    maintenance.applicability = "maintained-present";
    maintenance.deltas = {privacy_delta};

    carto::condition::AuthoredStateDelta damage_delta;
    damage_delta.id = 1U;
    damage_delta.target_record_id = 1U;
    damage_delta.target_asset_id = 1U;
    damage_delta.target_kind = carto::condition::StateTargetKind::surface_state;
    damage_delta.property = carto::condition::StateProperty::damage;
    damage_delta.before.text_value = "intact";
    damage_delta.after.text_value = "roof-damage";
    carto::condition::AuthoredStateBranch storm;
    storm.id = 2U;
    storm.name = "storm-damage";
    storm.applicability = "after-storm";
    storm.deltas = {damage_delta};

    auto contested_delta = privacy_delta;
    contested_delta.id = 1U;
    contested_delta.after.numeric_value = 0.4;
    carto::condition::AuthoredStateBranch alternate;
    alternate.id = 3U;
    alternate.name = "privacy-reduced-alternate";
    alternate.deltas = {contested_delta};

    auto child_delta = privacy_delta;
    child_delta.id = 1U;
    child_delta.before.numeric_value = 0.65;
    child_delta.after.numeric_value = 0.9;
    carto::condition::AuthoredStateBranch recontextualized;
    recontextualized.id = 4U;
    recontextualized.name = "recontextualized-safe-place";
    recontextualized.parent_branch_id = 1U;
    recontextualized.deltas = {child_delta};
    authoring.state_branches = {maintenance, storm, alternate, recontextualized};
    REQUIRE(authoring.validate());

    const double source_privacy = authoring.safe_places.front().privacy;
    const double source_wetness = authoring.surface_states.front().wetness;
    const auto baseline_compare = carto::condition::compare_state_branches(authoring, 0U, 1U);
    REQUIRE(baseline_compare);
    REQUIRE(baseline_compare.value().differences.size() == 1U);
    REQUIRE(baseline_compare.value().differences.front().property ==
        carto::condition::StateProperty::privacy);
    const auto sibling_compare = carto::condition::compare_state_branches(authoring, 1U, 2U);
    REQUIRE(sibling_compare);
    REQUIRE(sibling_compare.value().common_ancestor_branch_id == 0U);
    REQUIRE(sibling_compare.value().differences.size() == 2U);
    REQUIRE(std::none_of(sibling_compare.value().differences.begin(),
        sibling_compare.value().differences.end(), [](const auto& difference) {
            return difference.contested;
        }));
    const auto contested = carto::condition::compare_state_branches(authoring, 1U, 3U);
    REQUIRE(contested);
    REQUIRE(contested.value().differences.size() == 1U);
    REQUIRE(contested.value().differences.front().contested);
    REQUIRE(contested.value().differences.front().common_value.numeric_value == 0.8);
    REQUIRE(!carto::condition::resolve_state_branch_merge(
        authoring, contested.value(), {}, 5U, "unresolved-conflict"));
    const std::vector<carto::condition::StateMergeChoice> choose_left{{
        0U, carto::condition::StateMergeSide::left}};
    const auto left_resolution = carto::condition::resolve_state_branch_merge(
        authoring, contested.value(), choose_left, 5U, "left-resolution");
    REQUIRE(left_resolution);
    REQUIRE(left_resolution.value().parent_branch_id == 0U);
    REQUIRE(left_resolution.value().deltas.size() == 1U);
    REQUIRE(left_resolution.value().deltas.front().before.numeric_value == 0.8);
    REQUIRE(left_resolution.value().deltas.front().after.numeric_value == 0.65);
    const std::vector<carto::condition::StateMergeChoice> invalid_choice{{
        1U, carto::condition::StateMergeSide::right}};
    REQUIRE(!carto::condition::resolve_state_branch_merge(
        authoring, contested.value(), invalid_choice, 5U, "invalid-resolution"));
    auto caller_modified_comparison = contested.value();
    caller_modified_comparison.differences.clear();
    REQUIRE(!carto::condition::resolve_state_branch_merge(
        authoring, caller_modified_comparison, {}, 5U, "modified-comparison"));

    const std::vector<carto::condition::StateMergeChoice> sibling_choices{
        {0U, carto::condition::StateMergeSide::left},
        {1U, carto::condition::StateMergeSide::right}};
    const auto sibling_resolution = carto::condition::resolve_state_branch_merge(
        authoring, sibling_compare.value(), sibling_choices, 5U, "sibling-resolution");
    REQUIRE(sibling_resolution);
    REQUIRE(sibling_resolution.value().parent_branch_id == 0U);
    REQUIRE(sibling_resolution.value().deltas.size() == 2U);
    REQUIRE(sibling_resolution.value().deltas[0].before.numeric_value == 0.8);
    REQUIRE(sibling_resolution.value().deltas[0].after.numeric_value == 0.65);
    REQUIRE(sibling_resolution.value().deltas[1].before.text_value == "intact");
    REQUIRE(sibling_resolution.value().deltas[1].after.text_value == "roof-damage");

    auto shared_authoring = authoring;
    auto shared_privacy_delta = privacy_delta;
    shared_privacy_delta.after.numeric_value = 0.7;
    carto::condition::AuthoredStateBranch shared_left;
    shared_left.id = 10U;
    shared_left.name = "shared-privacy-left";
    shared_left.deltas = {shared_privacy_delta};
    auto shared_right = shared_left;
    shared_right.id = 11U;
    shared_right.name = "shared-privacy-and-damage-right";
    auto right_damage_delta = damage_delta;
    right_damage_delta.id = 2U;
    shared_right.deltas.push_back(right_damage_delta);
    shared_authoring.state_branches.push_back(shared_left);
    shared_authoring.state_branches.push_back(shared_right);
    REQUIRE(shared_authoring.validate());
    const auto shared_comparison = carto::condition::compare_state_branches(
        shared_authoring, 10U, 11U);
    REQUIRE(shared_comparison);
    REQUIRE(shared_comparison.value().differences.size() == 1U);
    const std::vector<carto::condition::StateMergeChoice> choose_shared_damage{{
        0U, carto::condition::StateMergeSide::right}};
    const auto shared_resolution = carto::condition::resolve_state_branch_merge(
        shared_authoring, shared_comparison.value(), choose_shared_damage,
        12U, "shared-value-resolution");
    REQUIRE(shared_resolution);
    REQUIRE(shared_resolution.value().deltas.size() == 2U);
    REQUIRE(shared_resolution.value().deltas[0].after.numeric_value == 0.7);
    REQUIRE(shared_resolution.value().deltas[1].after.text_value == "roof-damage");

    auto changed_after_comparison = authoring;
    changed_after_comparison.state_branches[0].applicability = "revised-context";
    REQUIRE(changed_after_comparison.validate());
    const auto stale_resolution = carto::condition::resolve_state_branch_merge(
        changed_after_comparison, contested.value(), choose_left,
        5U, "stale-resolution");
    REQUIRE(!stale_resolution);
    REQUIRE(stale_resolution.error().code == carto::core::ErrorCode::stale_data);

    const auto child_compare = carto::condition::compare_state_branches(authoring, 1U, 4U);
    REQUIRE(child_compare);
    REQUIRE(child_compare.value().common_ancestor_branch_id == 1U);
    REQUIRE(!child_compare.value().differences.front().contested);
    REQUIRE(authoring.safe_places.front().privacy == source_privacy);
    REQUIRE(authoring.surface_states.front().wetness == source_wetness);

    const auto encoded = authoring.serialize();
    const auto reopened = carto::condition::AuthoringDocument::deserialize(encoded);
    REQUIRE(reopened);
    REQUIRE(reopened.value().serialize() == encoded);
    const auto reopened_compare = carto::condition::compare_state_branches(
        reopened.value(), 1U, 3U);
    REQUIRE(reopened_compare);
    REQUIRE(reopened_compare.value().comparison_digest ==
        contested.value().comparison_digest);

    auto changed_baseline = authoring;
    changed_baseline.safe_places.front().privacy = 0.9;
    REQUIRE(changed_baseline.validate());
    REQUIRE(!carto::condition::compare_state_branches(changed_baseline, 0U, 1U));

    auto cyclic = authoring;
    cyclic.state_branches.front().parent_branch_id = 4U;
    REQUIRE(!cyclic.validate());
    auto wrong_target = authoring;
    wrong_target.state_branches.front().deltas.front().target_asset_id = 2U;
    REQUIRE(!wrong_target.validate());
    auto invalid_property = authoring;
    invalid_property.state_branches.front().deltas.front().after.numeric_value = 1.5;
    REQUIRE(!invalid_property.validate());
}

std::map<std::string, carto::spatial_field::ScalarField2D> make_effect_preview_fields(
    carto::condition::AuthoringDocument& authoring) {
    using carto::spatial_field::FieldSemantic;
    using carto::spatial_field::ScalarField2D;
    std::map<std::string, ScalarField2D> fields;
    const std::array<std::pair<std::string_view, FieldSemantic>, 3> specifications{{
        {"gas_station.wind_u", FieldSemantic::wind_u},
        {"gas_station.drainage", FieldSemantic::drainage_flow},
        {"gas_station.wind_v", FieldSemantic::wind_v},
    }};
    for (const auto& [identity, semantic] : specifications) {
        const auto field = ScalarField2D::create(std::string(identity), semantic,
            {-20.0, -20.0}, 2.0, 21U, 21U, -1.0, 1.0, 0.25);
        REQUIRE(field);
        fields.emplace(std::string(identity), field.value());
    }
    for (auto& binding : authoring.field_influences) {
        const auto& field = fields.at(binding.field_identity);
        binding.expected_field_revision = field.source_revision();
        binding.expected_field_digest = field.canonical_digest().value();
    }
    REQUIRE(authoring.validate());
    return fields;
}

std::map<std::string, carto::spatial_field::ScalarField2D> make_forest_fields(
    carto::condition::AuthoringDocument& authoring,
    std::string_view elevation_identity = "forest.elevation",
    carto::core::Vec2d elevation_origin = {0.0, 0.0}) {
    using carto::spatial_field::FieldSemantic;
    using carto::spatial_field::ScalarField2D;
    std::map<std::string, ScalarField2D> fields;
    const std::array<std::tuple<std::string_view, FieldSemantic, double, double, double,
        carto::core::Vec2d>, 5> specifications{{
        {"forest.moisture", FieldSemantic::moisture, 0.0, 1.0, 0.7, {0.0, 0.0}},
        {"forest.shade", FieldSemantic::shade, 0.0, 1.0, 0.8, {0.0, 0.0}},
        {"forest.disturbance", FieldSemantic::disturbance, 0.0, 1.0, 0.1, {0.0, 0.0}},
        {"forest.suitability", FieldSemantic::vegetation_suitability, 0.0, 1.0, 0.9, {0.0, 0.0}},
        {elevation_identity, FieldSemantic::terrain_elevation, 0.0, 100.0, 25.0,
            elevation_origin},
    }};
    for (const auto& [identity, semantic, minimum, maximum, initial, origin] : specifications) {
        auto field = ScalarField2D::create(std::string(identity), semantic, origin,
            1.0, 4U, 4U, minimum, maximum, initial);
        REQUIRE(field);
        fields.emplace(std::string(identity), std::move(field).value());
    }
    authoring.assets.front().asset_class = carto::condition::AssetClass::vegetation;
    const auto binding = [&fields](std::string_view identity) {
        const auto& field = fields.at(std::string(identity));
        return carto::condition::FieldSourceBinding{
            field.identity(), field.source_revision(), field.canonical_digest().value()};
    };
    carto::condition::VegetationScatterRecipe recipe;
    recipe.id = 50U;
    recipe.species_asset_id = authoring.assets.front().id;
    recipe.moisture = binding("forest.moisture");
    recipe.shade = binding("forest.shade");
    recipe.disturbance = binding("forest.disturbance");
    recipe.suitability = binding("forest.suitability");
    recipe.elevation = binding(elevation_identity);
    recipe.cell_spacing = 1.0;
    recipe.density_per_square_meter = 10.0;
    recipe.minimum_suitability = 0.2;
    recipe.minimum_scale = 0.8;
    recipe.maximum_scale = 1.2;
    recipe.maximum_instances = 32U;
    recipe.deterministic_seed = 71U;
    authoring.vegetation_scatters = {recipe};
    REQUIRE(authoring.validate());
    return fields;
}

void vegetation_scatter_round_trips_and_derives_deterministic_read_only_preview() {
    auto authoring = carto::condition::make_condition_fixture();
    REQUIRE(authoring);
    auto fields = make_forest_fields(authoring.value());
    const auto encoded = authoring.value().serialize();
    const auto reopened = carto::condition::AuthoringDocument::deserialize(encoded);
    REQUIRE(reopened);
    REQUIRE(reopened.value().vegetation_scatters.size() == 1U);
    REQUIRE(reopened.value().serialize() == encoded);

    const auto digest_before = authoring.value().canonical_digest().value();
    const auto first = carto::condition::preview_vegetation(
        authoring.value(), fields, 50U);
    const auto second = carto::condition::preview_vegetation(
        authoring.value(), fields, 50U);
    REQUIRE(first);
    REQUIRE(second);
    REQUIRE(first.value().preview_digest == second.value().preview_digest);
    REQUIRE(first.value().instances.size() == 16U);
    REQUIRE(first.value().candidate_count == 16U);
    REQUIRE(first.value().fields.size() == 5U);
    REQUIRE(first.value().instances.front().position.y == 25.0);
    REQUIRE(first.value().authoring_digest == digest_before);
    REQUIRE(authoring.value().canonical_digest().value() == digest_before);

    auto disabled = authoring.value();
    disabled.vegetation_scatters.front().enabled = false;
    const auto empty = carto::condition::preview_vegetation(disabled, fields, 50U);
    REQUIRE(empty);
    REQUIRE(empty.value().instances.empty());
}

void vegetation_scatter_preview_rejects_stale_missing_wrong_semantic_and_unbounded_inputs() {
    auto authoring = carto::condition::make_condition_fixture();
    REQUIRE(authoring);
    auto fields = make_forest_fields(authoring.value());

    auto stale_fields = fields;
    auto stale_moisture = carto::spatial_field::ScalarField2D::create(
        "forest.moisture", carto::spatial_field::FieldSemantic::moisture,
        {0.0, 0.0}, 1.0, 4U, 4U, 0.0, 1.0, 0.4);
    REQUIRE(stale_moisture);
    stale_fields["forest.moisture"] = stale_moisture.value();
    REQUIRE(!carto::condition::preview_vegetation(authoring.value(), stale_fields, 50U));

    auto missing_fields = fields;
    missing_fields.erase("forest.shade");
    REQUIRE(!carto::condition::preview_vegetation(authoring.value(), missing_fields, 50U));

    auto wrong_semantic_fields = fields;
    auto wrong_shade = carto::spatial_field::ScalarField2D::create(
        "forest.shade", carto::spatial_field::FieldSemantic::generic,
        {0.0, 0.0}, 1.0, 4U, 4U, 0.0, 1.0, 0.8);
    REQUIRE(wrong_shade);
    wrong_semantic_fields["forest.shade"] = wrong_shade.value();
    REQUIRE(!carto::condition::preview_vegetation(
        authoring.value(), wrong_semantic_fields, 50U));

    auto uncovered_authoring = carto::condition::make_condition_fixture();
    REQUIRE(uncovered_authoring);
    auto uncovered_fields = make_forest_fields(
        uncovered_authoring.value(), "forest.elevation", {100.0, 100.0});
    REQUIRE(!carto::condition::preview_vegetation(
        uncovered_authoring.value(), uncovered_fields, 50U));

    auto too_dense = authoring.value();
    too_dense.vegetation_scatters.front().maximum_instances = 1U;
    REQUIRE(!carto::condition::preview_vegetation(too_dense, fields, 50U));

    auto too_many_candidates = authoring.value();
    too_many_candidates.vegetation_scatters.front().cell_spacing = 0.1;
    REQUIRE(!carto::condition::preview_vegetation(too_many_candidates, fields, 50U));

    auto wrong_asset = authoring.value();
    wrong_asset.assets.front().asset_class = carto::condition::AssetClass::prop;
    REQUIRE(!wrong_asset.validate());
}

void vegetation_scatter_binds_generic_plant_source_with_revision_and_digest() {
    auto authoring = carto::condition::make_condition_fixture();
    REQUIRE(authoring);
    const auto fields = make_forest_fields(authoring.value());
    const carto::core::Revision source_revision{1U};
    const auto source_digest = digest("forest.conifer canonical source v1");
    authoring.value().vegetation_scatters.front().species_asset_id = 0U;
    authoring.value().vegetation_scatters.front().plant_source =
        carto::condition::PlantSpeciesSourceBinding{
            "forest.conifer", source_revision, source_digest};
    auto ambiguous_species = authoring.value();
    ambiguous_species.vegetation_scatters.front().species_asset_id = 1U;
    REQUIRE(!ambiguous_species.validate());
    REQUIRE(authoring.value().validate());

    const auto encoded = authoring.value().serialize();
    const auto reopened = carto::condition::AuthoringDocument::deserialize(encoded);
    REQUIRE(reopened);
    REQUIRE(reopened.value().vegetation_scatters.front().plant_source.has_value());
    REQUIRE(reopened.value().serialize() == encoded);

    std::map<std::string, carto::condition::VegetationPlantSourceReceipt> plants;
    plants.emplace("forest.conifer", carto::condition::VegetationPlantSourceReceipt{
        "forest.conifer", source_revision, source_digest});
    const auto preview = carto::condition::preview_vegetation(
        reopened.value(), fields, plants, 50U);
    REQUIRE(preview);
    REQUIRE(preview.value().plant_source.has_value());
    REQUIRE(preview.value().plant_source->identity == "forest.conifer");
    REQUIRE(preview.value().plant_source->source_revision == source_revision);
    REQUIRE(preview.value().plant_source->digest == source_digest);
    REQUIRE(preview.value().instances.size() == 16U);
    REQUIRE(preview.value().instances.front().species_asset_id == 0U);

    auto changed_evidence = plants;
    changed_evidence["forest.conifer"] = carto::condition::VegetationPlantSourceReceipt{
        "forest.conifer", source_revision, digest("forest.conifer canonical source v2")};
    REQUIRE(!carto::condition::preview_vegetation(
        reopened.value(), fields, changed_evidence, 50U));

    auto changed_source = plants;
    changed_source["forest.conifer"] = carto::condition::VegetationPlantSourceReceipt{
        "forest.conifer", carto::core::Revision{2U}, source_digest};
    REQUIRE(!carto::condition::preview_vegetation(reopened.value(), fields, changed_source, 50U));
    auto mislabeled_source = plants;
    mislabeled_source["forest.conifer"] = carto::condition::VegetationPlantSourceReceipt{
        "forest.other", source_revision, source_digest};
    REQUIRE(!carto::condition::preview_vegetation(reopened.value(), fields, mislabeled_source, 50U));
    plants.clear();
    REQUIRE(!carto::condition::preview_vegetation(reopened.value(), fields, plants, 50U));

    auto legacy_document = authoring.value();
    legacy_document.vegetation_scatters.front().species_asset_id = 1U;
    legacy_document.vegetation_scatters.front().plant_source.reset();
    std::string schema_five = legacy_document.serialize();
    const auto row_start = schema_five.find("VEGETATION_SCATTER 50 ");
    REQUIRE(row_start != std::string::npos);
    const auto row_end = schema_five.find('\n', row_start);
    REQUIRE(row_end != std::string::npos);
    REQUIRE(row_end >= 2U && schema_five.substr(row_end - 2U, 2U) == " 0");
    schema_five.erase(row_end - 2U, 2U);
    remove_kinematic_assembly_section_for_legacy_schema(schema_five);
    remove_prefab_instance_section_for_legacy_schema(schema_five);
    const auto header_end = schema_five.find('\n');
    REQUIRE(header_end != std::string::npos);
    schema_five.replace(0U, header_end, "CARTOGRAPHER_CONDITION_AUTHORING 5");
    const auto migrated = carto::condition::AuthoringDocument::deserialize(schema_five);
    REQUIRE(migrated);
    REQUIRE(!migrated.value().vegetation_scatters.front().plant_source.has_value());
}

void effect_source_emitter_field_binding_round_trip_and_adversarial_validation() {
    auto authoring = gas_station_effect_fixture();
    auto fields = make_effect_preview_fields(authoring);
    const auto encoded = authoring.serialize();
    const auto reopened = carto::condition::AuthoringDocument::deserialize(encoded);
    REQUIRE(reopened);
    REQUIRE(reopened.value().effect_sources.size() == 3U);
    REQUIRE(reopened.value().particle_emitters.size() == 3U);
    REQUIRE(reopened.value().field_influences.size() == 3U);
    REQUIRE(reopened.value().serialize() == encoded);
    REQUIRE(reopened.value().canonical_digest().value() == authoring.canonical_digest().value());
    const auto package = carto::condition::Package::publish(
        reopened.value(), carto::core::Revision(2U), reopened.value().project_namespace);
    REQUIRE(package);
    REQUIRE(carto::condition::admit_for_consumer(package.value()));

    auto bad_shape = authoring;
    bad_shape.effect_sources.front().shape = static_cast<carto::condition::EffectEmissionShape>(255U);
    REQUIRE(!bad_shape.validate());
    auto bad_state = authoring;
    bad_state.effect_sources.front().disabled_operating_state =
        bad_state.effect_sources.front().normal_operating_state;
    REQUIRE(!bad_state.validate());
    auto unbounded_emitter = authoring;
    unbounded_emitter.particle_emitters.front().total_particle_bound = 2049U;
    REQUIRE(!unbounded_emitter.validate());
    auto bad_burst = authoring;
    bad_burst.particle_emitters.at(1U).burst_count = 1U;
    bad_burst.particle_emitters.at(1U).burst_time_seconds = 8.0;
    REQUIRE(!bad_burst.validate());
    auto bad_field_identity = authoring;
    bad_field_identity.field_influences.front().field_identity = "../wind";
    REQUIRE(!bad_field_identity.validate());
    auto bad_revision = authoring;
    bad_revision.field_influences.front().expected_field_revision = carto::core::Revision(0U);
    REQUIRE(!bad_revision.validate());
    auto bad_digest = authoring;
    bad_digest.field_influences.front().expected_field_digest = {};
    REQUIRE(!bad_digest.validate());
    auto non_unit_direction = authoring;
    non_unit_direction.field_influences.front().direction = {2.0, 0.0, 0.0};
    REQUIRE(!non_unit_direction.validate());
    auto missing_effect = authoring;
    missing_effect.particle_emitters.front().effect_source_id = 999U;
    REQUIRE(!missing_effect.validate());
}

void condition_kinematic_assembly_round_trips_and_rejects_stale_or_unbounded_sources() {
    auto fixture = carto::condition::make_condition_fixture();
    REQUIRE(fixture);
    auto authoring = fixture.value();
    authoring.project_revision = carto::core::Revision(1U);

    carto::kinematic::KinematicGraph graph;
    carto::kinematic::KinematicNode frame;
    frame.id = carto::kinematic::JointId{1U};
    frame.kind = carto::kinematic::JointKind::fixed;
    frame.semantic_type = "assembly.frame";
    carto::kinematic::KinematicNode hinge;
    hinge.id = carto::kinematic::JointId{2U};
    hinge.parent = carto::kinematic::JointId{1U};
    hinge.kind = carto::kinematic::JointKind::hinge;
    hinge.rest.translation = {0.0, 1.0, 0.0};
    hinge.semantic_type = "door.hinge";
    REQUIRE(graph.insert_node(frame));
    REQUIRE(graph.insert_node(hinge));

    carto::kinematic::JointLimit limit;
    limit.joint = carto::kinematic::JointId{2U};
    limit.angular_enabled = true;
    limit.angular_min = {-1.5707963267948966, 0.0, 0.0};
    limit.angular_max = {1.5707963267948966, 0.0, 0.0};
    carto::kinematic::JointLimitSet limits;
    REQUIRE(limits.insert(limit));

    carto::condition::KinematicAssemblySource assembly;
    assembly.id = 101U;
    assembly.asset_id = authoring.assets.front().id;
    assembly.source_identity = "condition.fixture.motel8.room.door";
    assembly.source_revision = authoring.project_revision;
    assembly.graph_bytes = graph.serialize();
    assembly.joint_limit_bytes = limits.serialize();
    assembly.part_bindings.push_back(
        carto::condition::KinematicAssemblySource::PartBinding{
            2U, 42U, 99U, carto::core::Revision{3U}});
    authoring.kinematic_assemblies.push_back(assembly);
    REQUIRE(authoring.validate());

    const std::string encoded = authoring.serialize();
    const auto reopened = carto::condition::AuthoringDocument::deserialize(encoded);
    REQUIRE(reopened);
    REQUIRE(reopened.value().serialize() == encoded);
    REQUIRE(reopened.value().kinematic_assemblies.size() == 1U);
    REQUIRE(reopened.value().kinematic_assemblies.front().part_bindings ==
        assembly.part_bindings);
    auto unbound_assembly = authoring;
    unbound_assembly.kinematic_assemblies.front().part_bindings.clear();
    const auto unbound_publication = unbound_assembly.validate_for_publication();
    REQUIRE(!unbound_publication);
    REQUIRE(unbound_publication.error().message.find("articulated assembly") !=
        std::string::npos);

    std::string schema_seven = encoded;
    const std::string schema_nine_header = "CARTOGRAPHER_CONDITION_AUTHORING 9";
    const auto schema_header = schema_seven.find(schema_nine_header);
    REQUIRE(schema_header == 0U);
    schema_seven.replace(schema_header, schema_nine_header.size(),
        "CARTOGRAPHER_CONDITION_AUTHORING 7");
    const auto binding_start = schema_seven.find("KINEMATIC_PART_BINDING ");
    REQUIRE(binding_start != std::string::npos);
    const auto binding_end = schema_seven.find('\n', binding_start);
    REQUIRE(binding_end != std::string::npos);
    schema_seven.erase(binding_start, binding_end - binding_start + 1U);
    const auto assembly_start = schema_seven.find("KINEMATIC_ASSEMBLY ");
    REQUIRE(assembly_start != std::string::npos);
    const auto assembly_end = schema_seven.find('\n', assembly_start);
    REQUIRE(assembly_end != std::string::npos);
    const auto binding_count_start = schema_seven.rfind(' ', assembly_end - 1U);
    REQUIRE(binding_count_start != std::string::npos && binding_count_start > assembly_start);
    schema_seven.erase(binding_count_start, assembly_end - binding_count_start);
    remove_prefab_instance_section_for_legacy_schema(schema_seven);
    const auto migrated_v7 = carto::condition::AuthoringDocument::deserialize(schema_seven);
    REQUIRE(migrated_v7);
    REQUIRE(migrated_v7.value().kinematic_assemblies.size() == 1U);
    REQUIRE(migrated_v7.value().kinematic_assemblies.front().part_bindings.empty());
    const auto migrated_v7_publication = migrated_v7.value().validate_for_publication();
    REQUIRE(!migrated_v7_publication);
    REQUIRE(migrated_v7_publication.error().message.find("articulated assembly") !=
        std::string::npos);
    const auto reopened_graph = carto::kinematic::KinematicGraph::deserialize(
        reopened.value().kinematic_assemblies.front().graph_bytes);
    const auto reopened_limits = carto::kinematic::JointLimitSet::deserialize(
        reopened.value().kinematic_assemblies.front().joint_limit_bytes);
    REQUIRE(reopened_graph && reopened_limits);
    REQUIRE(reopened_graph.value().nodes().size() == 2U);
    REQUIRE(reopened_limits.value().find(carto::kinematic::JointId{2U}) != nullptr);

    auto fixed_joint_binding = authoring;
    fixed_joint_binding.kinematic_assemblies.front().part_bindings.front().joint_id = 1U;
    REQUIRE(!fixed_joint_binding.validate());
    auto zero_mesh_revision = authoring;
    zero_mesh_revision.kinematic_assemblies.front().part_bindings.front().mesh_revision =
        carto::core::Revision{};
    REQUIRE(!zero_mesh_revision.validate());
    auto duplicate_scene_object = authoring;
    duplicate_scene_object.kinematic_assemblies.front().part_bindings.push_back(
        carto::condition::KinematicAssemblySource::PartBinding{
            2U, 42U, 100U, carto::core::Revision{1U}});
    REQUIRE(!duplicate_scene_object.validate());
    auto multiply_controlled_object = authoring;
    auto second_assembly = assembly;
    second_assembly.id += 1U;
    second_assembly.source_identity += ".second-controller";
    second_assembly.part_bindings.front().joint_id = 2U;
    multiply_controlled_object.kinematic_assemblies.push_back(second_assembly);
    std::sort(multiply_controlled_object.kinematic_assemblies.begin(),
        multiply_controlled_object.kinematic_assemblies.end(),
        [](const auto& left, const auto& right) { return left.id < right.id; });
    REQUIRE(!multiply_controlled_object.validate());

    auto aggregate_oversized = authoring;
    for (std::uint64_t index = 0U; index < 8U; ++index) {
        auto oversized = assembly;
        oversized.id += index + 1U;
        oversized.source_identity += ".oversized." + std::to_string(index);
        oversized.graph_bytes.assign(carto::condition::AuthoringDocument::
            kMaxKinematicPayloadBytes, 'x');
        oversized.joint_limit_bytes.clear();
        aggregate_oversized.kinematic_assemblies.push_back(std::move(oversized));
    }
    REQUIRE(!aggregate_oversized.validate());

    auto missing_travel = authoring;
    missing_travel.kinematic_assemblies.front().joint_limit_bytes =
        carto::kinematic::JointLimitSet{}.serialize();
    REQUIRE(!missing_travel.validate());
    auto missing_asset = authoring;
    missing_asset.kinematic_assemblies.front().asset_id = 999999U;
    REQUIRE(!missing_asset.validate());
    auto future_revision = authoring;
    future_revision.kinematic_assemblies.front().source_revision =
        carto::core::Revision(2U);
    REQUIRE(!future_revision.validate());
    auto duplicate_identity = authoring;
    auto duplicate = duplicate_identity.kinematic_assemblies.front();
    duplicate.id += 1U;
    duplicate_identity.kinematic_assemblies.push_back(duplicate);
    std::sort(duplicate_identity.kinematic_assemblies.begin(),
        duplicate_identity.kinematic_assemblies.end(),
        [](const auto& left, const auto& right) { return left.id < right.id; });
    REQUIRE(!duplicate_identity.validate());

    auto legacy = carto::condition::make_condition_fixture();
    REQUIRE(legacy);
    std::string schema_six = legacy.value().serialize();
    const std::string empty_section = "KINEMATIC_ASSEMBLIES 0\n";
    const auto section_start = schema_six.find(empty_section);
    REQUIRE(section_start != std::string::npos);
    schema_six.erase(section_start, empty_section.size());
    remove_prefab_instance_section_for_legacy_schema(schema_six);
    const auto header_end = schema_six.find('\n');
    REQUIRE(header_end != std::string::npos);
    schema_six.replace(0U, header_end, "CARTOGRAPHER_CONDITION_AUTHORING 6");
    const auto migrated = carto::condition::AuthoringDocument::deserialize(schema_six);
    REQUIRE(migrated);
    REQUIRE(migrated.value().kinematic_assemblies.empty());
    REQUIRE(carto::condition::AuthoringDocument::deserialize(migrated.value().serialize()));
}

void effect_preview_is_deterministic_state_bound_and_read_only() {
    auto authoring = gas_station_effect_fixture();
    const auto fields = make_effect_preview_fields(authoring);
    const auto before = authoring.serialize();
    const auto revision_before = authoring.project_revision;
    const auto normal = carto::condition::preview_effect(
        authoring, fields, 1U, "compressor.running", 2.0);
    const auto repeated = carto::condition::preview_effect(
        authoring, fields, 1U, "compressor.running", 2.0);
    REQUIRE(normal);
    REQUIRE(repeated);
    REQUIRE(!normal.value().particles.empty());
    REQUIRE(normal.value().preview_digest == repeated.value().preview_digest);
    REQUIRE(normal.value().particles.size() == repeated.value().particles.size());
    REQUIRE(normal.value().particles.front().position.x == repeated.value().particles.front().position.x);

    const auto disabled = carto::condition::preview_effect(
        authoring, fields, 1U, "compressor.off", 2.0);
    REQUIRE(disabled);
    REQUIRE(disabled.value().particles.empty());
    REQUIRE(!carto::condition::preview_effect(
        authoring, fields, 1U, "compressor.unknown", 2.0));
    REQUIRE(!carto::condition::preview_effect(
        authoring, fields, 1U, "compressor.running", 601.0));
    REQUIRE(authoring.serialize() == before);
    REQUIRE(authoring.project_revision == revision_before);

    const std::array<std::tuple<carto::condition::Id, std::string_view, std::string_view>, 3>
        gas_station_states{{
            {1U, "compressor.running", "compressor.off"},
            {2U, "rain.active", "rain.stopped"},
            {3U, "engine.running", "engine.stopped"},
        }};
    for (const auto& [effect_id, normal_state, disabled_state] : gas_station_states) {
        const auto active = carto::condition::preview_effect(
            authoring, fields, effect_id, normal_state, 2.0);
        const auto active_again = carto::condition::preview_effect(
            authoring, fields, effect_id, normal_state, 2.0);
        const auto inactive = carto::condition::preview_effect(
            authoring, fields, effect_id, disabled_state, 2.0);
        REQUIRE(active);
        REQUIRE(active_again);
        REQUIRE(inactive);
        REQUIRE(!active.value().particles.empty());
        REQUIRE(active.value().particles.size() <= 2048U);
        REQUIRE(active.value().preview_digest == active_again.value().preview_digest);
        REQUIRE(inactive.value().particles.empty());
    }

    auto stale_digest = authoring;
    stale_digest.field_influences.front().expected_field_digest = digest("stale-field-digest");
    REQUIRE(stale_digest.validate());
    REQUIRE(!carto::condition::preview_effect(
        stale_digest, fields, 1U, "compressor.running", 2.0));
    auto missing_field = fields;
    missing_field.erase("gas_station.wind_u");
    REQUIRE(!carto::condition::preview_effect(
        authoring, missing_field, 1U, "compressor.running", 2.0));
}

void effect_preview_rejects_stale_revision_and_out_of_coverage_field() {
    auto authoring = gas_station_effect_fixture();
    auto fields = make_effect_preview_fields(authoring);
    auto& wind = fields.at("gas_station.wind_u");
    const auto painted = wind.paint(carto::spatial_field::FieldBrush{
        wind.source_revision(), {0.0, 0.0}, 2.0, 0.75, 1.0,
        carto::spatial_field::BrushMode::replace});
    REQUIRE(painted);
    REQUIRE(!carto::condition::preview_effect(
        authoring, fields, 1U, "compressor.running", 1.0));

    auto outside_fields = make_effect_preview_fields(authoring);
    const auto outside = carto::spatial_field::ScalarField2D::create(
        "gas_station.wind_u", carto::spatial_field::FieldSemantic::wind_u,
        {1000.0, 1000.0}, 2.0, 21U, 21U, -1.0, 1.0, 0.25);
    REQUIRE(outside);
    outside_fields["gas_station.wind_u"] = outside.value();
    auto& binding = authoring.field_influences.front();
    binding.expected_field_revision = outside.value().source_revision();
    binding.expected_field_digest = outside.value().canonical_digest().value();
    REQUIRE(authoring.validate());
    REQUIRE(!carto::condition::preview_effect(
        authoring, outside_fields, 1U, "compressor.running", 1.0));
}

void ui_acoustic_geometry_edits_are_revision_bound_and_undoable() {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto root = std::filesystem::current_path() / "build" / "test-temp" /
        ("cartographer-condition-acoustic-gizmo-tests-" + std::to_string(stamp));
    std::filesystem::create_directories(root);

    carto::application::ApplicationSession session;
    carto::ui::UiController ui(session);
    REQUIRE(ui.dispatch(carto::application::NewProjectAction{"CONDITION acoustic gizmos"}));
    REQUIRE(ui.import_condition_fixture());
    auto authoring = fixture_with_audio_source_evidence();
    authoring.audio_sources.push_back(carto::condition::AuthoredAudioSource{
        1U, 3U, 2U, carto::core::Transform::identity(), 0.8, 1.0,
        true, true, 0.5, 18.0, 100U, carto::condition::AudioBusClass::machinery,
        carto::condition::AudioOcclusionIntent::static_geometry, 0.25,
        "operational", "motel.room.ac-hum", true});
    authoring.acoustic_zones.push_back(carto::condition::AuthoredAcousticZone{
        1U, 3U, carto::condition::AcousticZoneShape::box,
        carto::core::Transform::identity(), {2.0, 1.5, 3.0}, 0.0,
        "small-room", 0.9, "motel-room", 10U, "motel8.room.101", true});
    authoring.project_revision = ui.snapshot().condition.authored_revision.next();
    const auto source_path = root / "acoustic-authoring.carto-condition-authoring";
    {
        std::ofstream stream(source_path, std::ios::binary);
        REQUIRE(stream.good());
        stream << authoring.serialize();
        REQUIRE(stream.good());
    }
    REQUIRE(ui.import_condition_authoring(source_path));

    const auto before = session.condition_authoring();
    REQUIRE(before);
    REQUIRE(ui.update_condition_audio_source_distances(
        1U, before.value().project_revision, 0.75, 22.0));
    const auto after_audio = session.condition_authoring();
    REQUIRE(after_audio);
    REQUIRE(after_audio.value().audio_sources.front().minimum_distance == 0.75);
    REQUIRE(after_audio.value().audio_sources.front().maximum_distance == 22.0);
    REQUIRE(!ui.update_condition_audio_source_distances(
        1U, before.value().project_revision, 1.0, 12.0));
    REQUIRE(ui.set_condition_audio_source_enabled(
        1U, after_audio.value().project_revision, false));
    const auto disabled_source = session.condition_authoring();
    REQUIRE(disabled_source);
    REQUIRE(!disabled_source.value().audio_sources.front().enabled);
    REQUIRE(!ui.update_condition_audio_source_distances(
        1U, disabled_source.value().project_revision, 12.0, 12.0));

    const auto zone_before = disabled_source.value().acoustic_zones.front().half_extents;
    REQUIRE(ui.update_condition_acoustic_zone_volume(
        1U, disabled_source.value().project_revision, {4.0, 2.5, 5.0}, 0.0));
    const auto after_zone = session.condition_authoring();
    REQUIRE(after_zone);
    REQUIRE(after_zone.value().acoustic_zones.front().half_extents.x == 4.0);
    REQUIRE(after_zone.value().project_revision == disabled_source.value().project_revision.next());
    REQUIRE(ui.dispatch(carto::application::UndoAction{}));
    const auto undone = session.condition_authoring();
    REQUIRE(undone);
    REQUIRE(undone.value().acoustic_zones.front().half_extents.x == zone_before.x);
    REQUIRE(!undone.value().audio_sources.front().enabled);
    REQUIRE(ui.dispatch(carto::application::RedoAction{}));
    const auto redone = session.condition_authoring();
    REQUIRE(redone);
    REQUIRE(redone.value().acoustic_zones.front().half_extents.x == 4.0);

    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
}

void ui_condition_asset_binding_is_revision_checked_and_undoable() {
    carto::application::ApplicationSession session;
    carto::ui::UiController ui(session);
    REQUIRE(ui.dispatch(carto::application::NewProjectAction{
        "CONDITION scene-object binding"}));
    REQUIRE(ui.dispatch(carto::application::CreateBoxAction{
        "Bound CONDITION object", {2.0, 2.0, 2.0}}));
    REQUIRE(ui.import_condition_fixture());

    const auto before = ui.snapshot();
    REQUIRE(before.condition.assets.size() == 3U);
    REQUIRE(before.selection.objects.empty());
    const auto object = session.snapshot().objects.front().object.id;
    const auto bound = ui.bind_condition_asset_to_object(
        2U, object, before.project_revision,
        before.condition.authored_revision);
    REQUIRE(bound);

    const auto after = ui.snapshot();
    const auto asset = std::find_if(after.condition.assets.begin(),
        after.condition.assets.end(), [](const auto& value) { return value.id == 2U; });
    REQUIRE(asset != after.condition.assets.end());
    REQUIRE(asset->authored_object_id == object.value);
    REQUIRE(after.condition.authored_revision ==
        before.condition.authored_revision.next());
    REQUIRE(after.project_revision > before.project_revision);
    REQUIRE(!after.operations.empty());
    REQUIRE(after.operations.back().action ==
        "Bind CONDITION Asset to Scene Object");

    const auto unchanged = ui.bind_condition_asset_to_object(
        2U, object, after.project_revision,
        after.condition.authored_revision);
    REQUIRE(unchanged);
    REQUIRE(!unchanged.value().document_changed);
    REQUIRE(ui.snapshot().project_revision == after.project_revision);

    const auto stale_project = ui.bind_condition_asset_to_object(
        2U, object, before.project_revision,
        before.condition.authored_revision);
    REQUIRE(!stale_project);
    const auto stale_authoring = ui.bind_condition_asset_to_object(
        2U, object, after.project_revision,
        before.condition.authored_revision);
    REQUIRE(!stale_authoring);
    const auto missing_asset = ui.bind_condition_asset_to_object(
        999U, object, after.project_revision,
        after.condition.authored_revision);
    REQUIRE(!missing_asset);
    const auto missing_object = ui.bind_condition_asset_to_object(
        2U, carto::scene::ObjectId{999U}, after.project_revision,
        after.condition.authored_revision);
    REQUIRE(!missing_object);
    REQUIRE(ui.snapshot().condition.assets[1].authored_object_id == object.value);
    REQUIRE(ui.snapshot().project_revision == after.project_revision);

    const auto ai_attempt = ui.apply_ai_intent(
        "bind condition asset 2 to object 1");
    REQUIRE(!ai_attempt);
    REQUIRE(ui.snapshot().condition.assets[1].authored_object_id == object.value);
    REQUIRE(ui.dispatch(carto::application::UndoAction{}));
    const auto undone = ui.snapshot();
    const auto restored_asset = std::find_if(undone.condition.assets.begin(),
        undone.condition.assets.end(), [](const auto& value) { return value.id == 2U; });
    REQUIRE(restored_asset != undone.condition.assets.end());
    REQUIRE(restored_asset->authored_object_id == 2U);
    REQUIRE(ui.dispatch(carto::application::RedoAction{}));
    const auto redone = ui.snapshot();
    const auto rebound_asset = std::find_if(redone.condition.assets.begin(),
        redone.condition.assets.end(), [](const auto& value) { return value.id == 2U; });
    REQUIRE(rebound_asset != redone.condition.assets.end());
    REQUIRE(rebound_asset->authored_object_id == object.value);
}

void ui_effect_preview_uses_project_fields_and_is_non_mutating_across_history() {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto root = std::filesystem::current_path() / "build" / "test-temp" /
        ("cartographer-condition-effect-tests-" + std::to_string(stamp));
    std::filesystem::create_directories(root);

    carto::application::ApplicationSession session;
    carto::ui::UiController ui(session);
    REQUIRE(ui.dispatch(carto::application::NewProjectAction{"CONDITION effects"}));
    REQUIRE(ui.import_condition_fixture());
    auto authoring = gas_station_effect_fixture();
    const auto fields = make_effect_preview_fields(authoring);
    for (const auto& [identity, field] : fields) {
        REQUIRE(ui.dispatch(carto::application::CreateSpatialFieldAction{
            identity, field.semantic(), field.origin_uv(), field.cell_size(), field.width(),
            field.height(), field.minimum_value(), field.maximum_value(),
            field.values().front()}));
    }
    authoring.project_revision = ui.snapshot().condition.authored_revision.next();
    const auto source_path = root / "gas-station-effects.carto-condition-authoring";
    {
        std::ofstream stream(source_path, std::ios::binary);
        REQUIRE(stream.good());
        stream << authoring.serialize();
        REQUIRE(stream.good());
    }
    REQUIRE(ui.import_condition_authoring(source_path));

    const auto project_revision_before = ui.snapshot().project_revision;
    const auto normal = ui.preview_condition_effect(1U, "compressor.running", 2.0);
    REQUIRE(normal);
    REQUIRE(!normal.value().particles.empty());
    REQUIRE(ui.snapshot().project_revision == project_revision_before);

    const auto project_path = root / "gas-station-effects.carto";
    REQUIRE(ui.dispatch(carto::application::SaveProjectAction{project_path}));
    carto::application::ApplicationSession reopened_session;
    carto::ui::UiController reopened_ui(reopened_session);
    REQUIRE(reopened_ui.dispatch(carto::application::OpenProjectAction{project_path}));
    const auto reopened_preview = reopened_ui.preview_condition_effect(
        1U, "compressor.running", 2.0);
    REQUIRE(reopened_preview);
    REQUIRE(reopened_preview.value().preview_digest == normal.value().preview_digest);

    REQUIRE(ui.dispatch(carto::application::UndoAction{}));
    REQUIRE(!ui.preview_condition_effect(1U, "compressor.running", 2.0));
    REQUIRE(ui.dispatch(carto::application::RedoAction{}));
    REQUIRE(ui.preview_condition_effect(1U, "compressor.running", 2.0));

    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
}

void condition_rural_road_forest_workflow_is_revision_bound_and_portable() {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto root = std::filesystem::current_path() / "build" / "test-temp" /
        ("cartographer-condition-rural-road-forest-" + std::to_string(stamp));
    std::filesystem::create_directories(root);

    carto::application::ApplicationSession session;
    carto::ui::UiController ui(session);
    REQUIRE(ui.dispatch(carto::application::NewProjectAction{"Rural Road / Forest"}));

    REQUIRE(ui.import_condition_fixture());

    auto authoring = carto::condition::make_condition_fixture();
    REQUIRE(authoring);
    constexpr std::string_view terrain_identity = "terrain.tile.x0.z0";
    auto fields = make_forest_fields(authoring.value(), terrain_identity, {0.0, 0.0});
    auto recipe = authoring.value().vegetation_scatters.front();
    authoring.value().vegetation_scatters.clear();
    for (const auto& [identity, field] : fields) {
        REQUIRE(ui.dispatch(carto::application::CreateSpatialFieldAction{
            identity, field.semantic(), field.origin_uv(), field.cell_size(), field.width(),
            field.height(), field.minimum_value(), field.maximum_value(),
            field.values().front()}));
    }
    REQUIRE(ui.dispatch(carto::application::CreateSpatialFieldAction{
        "cartographer.wind.u", carto::spatial_field::FieldSemantic::wind_u,
        {0.0, 0.0}, 1.0, 4U, 4U, -10.0, 10.0, 1.0}));
    REQUIRE(ui.dispatch(carto::application::CreateSpatialFieldAction{
        "cartographer.wind.v", carto::spatial_field::FieldSemantic::wind_v,
        {0.0, 0.0}, 1.0, 4U, 4U, -10.0, 10.0, 0.0}));

    authoring.value().project_revision = ui.snapshot().condition.authored_revision.next();
    const auto authoring_path = root / "rural-road-forest.carto-condition-authoring";
    {
        std::ofstream stream(authoring_path, std::ios::binary);
        REQUIRE(stream.good());
        stream << authoring.value().serialize();
        REQUIRE(stream.good());
    }
    REQUIRE(ui.import_condition_authoring(authoring_path));
    REQUIRE(ui.add_condition_vegetation_scatter(recipe));

    carto::geometry::CurvePath centerline;
    centerline.source_identity = "cartographer.rural-road.centerline";
    centerline.source_revision = carto::core::Revision{1U};
    centerline.segments.push_back({
        {0.75, 24.0, 1.5}, {1.25, 24.0, 1.5},
        {1.75, 24.0, 1.5}, {2.25, 24.0, 1.5}});
    carto::project::RoadSource road;
    road.source_identity = "cartographer.rural-road.primary";
    road.source_revision = carto::core::Revision{1U};
    road.centerline_identity = centerline.source_identity;
    road.centerline_revision = centerline.source_revision;
    road.width_meters = 1.2;
    road.shoulder_meters = 0.1;
    road.maximum_grade_ratio = 0.25;
    road.minimum_curve_radius_meters = 1.0;
    road.drainage_intent = carto::project::RoadDrainageIntent::roadside_ditch;
    REQUIRE(ui.dispatch(carto::application::SetRoadAuthoringAction{
        ui.snapshot().project_revision, centerline, road}));

    carto::geometry::CurvePath secondary_path;
    secondary_path.source_identity = "cartographer.forest.secondary-trail.centerline";
    secondary_path.source_revision = carto::core::Revision{1U};
    secondary_path.segments.push_back({
        {0.25, 25.0, 2.65}, {1.0, 25.0, 2.65},
        {2.0, 25.0, 2.65}, {3.0, 25.0, 2.65}});
    carto::project::RoadSource secondary_trail;
    secondary_trail.source_identity = "cartographer.forest.secondary-trail";
    secondary_trail.source_revision = carto::core::Revision{1U};
    secondary_trail.centerline_identity = secondary_path.source_identity;
    secondary_trail.centerline_revision = secondary_path.source_revision;
    secondary_trail.width_meters = 0.65;
    secondary_trail.shoulder_meters = 0.05;
    secondary_trail.maximum_grade_ratio = 0.4;
    secondary_trail.minimum_curve_radius_meters = 0.5;
    secondary_trail.surface_class = carto::project::RoadSurfaceClass::trail;
    secondary_trail.function_class = carto::project::RoadFunctionClass::pedestrian;
    REQUIRE(ui.dispatch(carto::application::SetRoadAuthoringAction{
        ui.snapshot().project_revision, secondary_path, secondary_trail}));

    const auto before_source_revision = ui.snapshot();
    const auto stale_road_surface = session.road_surface_preview(road.source_identity, 8U);
    if (!stale_road_surface) {
        throw TestFailure("rural road surface preview failed: " +
            stale_road_surface.error().message);
    }
    REQUIRE(stale_road_surface);
    const auto stale_road_terrain = session.road_terrain_preview(
        road.source_identity, std::string(terrain_identity),
        carto::application::RoadTerrainCoordinateFrame::world_xz_elevation_y, 8U);
    if (!stale_road_terrain) {
        throw TestFailure("rural road/terrain preview failed: " +
            stale_road_terrain.error().message);
    }
    if (const auto validation = stale_road_terrain.value().validate(); !validation) {
        throw TestFailure("rural road/terrain preview is invalid: " +
            validation.error().message);
    }

    auto revised_centerline = centerline;
    revised_centerline.source_revision = carto::core::Revision{2U};
    auto revised_road = road;
    revised_road.source_revision = carto::core::Revision{2U};
    revised_road.centerline_revision = revised_centerline.source_revision;
    revised_road.width_meters = 1.35;
    REQUIRE(ui.dispatch(carto::application::SetRoadAuthoringAction{
        before_source_revision.project_revision, revised_centerline, revised_road}));
    const auto before_stale_commit = ui.snapshot();
    const carto::application::CreateRoadSurfaceInstanceAction stale_road_action{
        before_stale_commit.project_generation,
        before_stale_commit.project_revision,
        stale_road_surface.value().road_identity,
        stale_road_surface.value().road_revision,
        stale_road_surface.value().road_digest,
        stale_road_surface.value().mesh.centerline_identity,
        stale_road_surface.value().mesh.centerline_revision,
        stale_road_surface.value().centerline_digest,
        stale_road_surface.value().mesh.samples_per_segment};
    const auto stale_road_commit = ui.dispatch(stale_road_action);
    REQUIRE(!stale_road_commit &&
        stale_road_commit.error().code == carto::core::ErrorCode::stale_data);
    const auto after_stale_commit = ui.snapshot();
    REQUIRE(after_stale_commit.project_revision == before_stale_commit.project_revision &&
        after_stale_commit.undo_count == before_stale_commit.undo_count &&
        after_stale_commit.operations.size() == before_stale_commit.operations.size() &&
        after_stale_commit.objects.empty());

    const auto authoring_snapshot = ui.snapshot();
    const auto road_surface = session.road_surface_preview(road.source_identity, 8U);
    REQUIRE(road_surface);
    REQUIRE(road_surface.value().road_revision == revised_road.source_revision &&
        road_surface.value().road_digest != stale_road_surface.value().road_digest);
    const auto road_terrain = session.road_terrain_preview(
        road.source_identity, std::string(terrain_identity),
        carto::application::RoadTerrainCoordinateFrame::world_xz_elevation_y, 8U);
    if (!road_terrain) {
        throw TestFailure("current rural road/terrain preview failed: " +
            road_terrain.error().message);
    }
    if (const auto validation = road_terrain.value().validate(); !validation) {
        throw TestFailure("current rural road/terrain preview is invalid: " +
            validation.error().message);
    }
    REQUIRE(road_terrain.value().cut_volume_cubic_meters > 0.0);
    const auto secondary_surface = session.road_surface_preview(
        secondary_trail.source_identity, 8U);
    REQUIRE(secondary_surface &&
        secondary_surface.value().road_identity == secondary_trail.source_identity &&
        secondary_surface.value().mesh.centerline_identity == secondary_path.source_identity);
    const auto wind_preview = session.spatial_vector_field_preview(
        "cartographer.wind.u", "cartographer.wind.v");
    REQUIRE(wind_preview);
    const auto vegetation_preview = ui.preview_condition_vegetation(recipe.id);
    REQUIRE(vegetation_preview && vegetation_preview.value().instances.size() >= 2U);
    REQUIRE(ui.snapshot().project_revision == authoring_snapshot.project_revision);

    const auto primary_road_view = std::find_if(
        authoring_snapshot.road_sources.begin(), authoring_snapshot.road_sources.end(),
        [&road](const auto& candidate) {
            return candidate.identity == road.source_identity;
        });
    const auto primary_centerline_view = std::find_if(
        authoring_snapshot.curve_paths.begin(), authoring_snapshot.curve_paths.end(),
        [&centerline](const auto& candidate) {
            return candidate.identity == centerline.source_identity;
        });
    const auto terrain_view = std::find_if(
        authoring_snapshot.spatial_fields.begin(), authoring_snapshot.spatial_fields.end(),
        [](const auto& field) { return field.identity == "terrain.tile.x0.z0"; });
    REQUIRE(primary_road_view != authoring_snapshot.road_sources.end());
    REQUIRE(primary_centerline_view != authoring_snapshot.curve_paths.end());
    REQUIRE(terrain_view != authoring_snapshot.spatial_fields.end());
    REQUIRE(!stale_road_surface.value().matches(
        authoring_snapshot.project_generation, *primary_road_view,
        *primary_centerline_view));
    REQUIRE(!stale_road_terrain.value().matches_sources(
        authoring_snapshot.project_generation, *primary_road_view,
        *primary_centerline_view, *terrain_view));
    REQUIRE(road_terrain.value().matches_sources(
        authoring_snapshot.project_generation, *primary_road_view,
        *primary_centerline_view, *terrain_view));
    const auto preview_project_revision = ui.snapshot().project_revision;
    REQUIRE(ui.dispatch(carto::application::CreateRoadSurfaceInstanceAction{
        authoring_snapshot.project_generation, preview_project_revision,
        road_surface.value().road_identity, road_surface.value().road_revision,
        road_surface.value().road_digest, road_surface.value().mesh.centerline_identity,
        road_surface.value().mesh.centerline_revision, road_surface.value().centerline_digest,
        road_surface.value().mesh.samples_per_segment}));
    REQUIRE(ui.dispatch(carto::application::CreateRoadSurfaceInstanceAction{
        authoring_snapshot.project_generation, ui.snapshot().project_revision,
        secondary_surface.value().road_identity, secondary_surface.value().road_revision,
        secondary_surface.value().road_digest,
        secondary_surface.value().mesh.centerline_identity,
        secondary_surface.value().mesh.centerline_revision,
        secondary_surface.value().centerline_digest,
        secondary_surface.value().mesh.samples_per_segment}));

    const auto observation_snapshot = ui.snapshot();
    const auto curve_source_for_observation =
        session.curve_path_source(primary_centerline_view->identity);
    REQUIRE(curve_source_for_observation);
    const auto curve_route_visibility = ui.evaluate_curve_route_visibility(
        primary_centerline_view->identity, observation_snapshot.project_revision,
        {6.0, 26.0, 1.5}, 8U);
    REQUIRE(curve_route_visibility);
    REQUIRE(curve_route_visibility.value().query.curve_identity ==
        primary_centerline_view->identity);
    REQUIRE(curve_route_visibility.value().query.curve_revision ==
        primary_centerline_view->source_revision);
    REQUIRE(curve_route_visibility.value().query.curve_digest ==
        primary_centerline_view->digest);
    REQUIRE(curve_route_visibility.value().visibility.samples.size() == 9U);
    REQUIRE(curve_route_visibility.value().validate(
        curve_source_for_observation.value(), observation_snapshot.project_revision));
    REQUIRE(curve_route_visibility.value().query.route.observer_positions.front().x == 0.75);
    REQUIRE(curve_route_visibility.value().query.route.observer_positions.back().x == 2.25);
    const auto repeated_curve_route_visibility = ui.evaluate_curve_route_visibility(
        primary_centerline_view->identity, observation_snapshot.project_revision,
        {6.0, 26.0, 1.5}, 8U);
    REQUIRE(repeated_curve_route_visibility);
    REQUIRE(repeated_curve_route_visibility.value().query.curve_digest ==
        curve_route_visibility.value().query.curve_digest);
    REQUIRE(repeated_curve_route_visibility.value().visibility.cumulative_route_distance ==
        curve_route_visibility.value().visibility.cumulative_route_distance);
    for (std::size_t index = 0U;
         index < curve_route_visibility.value().visibility.samples.size(); ++index) {
        REQUIRE(repeated_curve_route_visibility.value().visibility.samples[index].visible ==
            curve_route_visibility.value().visibility.samples[index].visible);
        REQUIRE(repeated_curve_route_visibility.value().visibility.samples[index].blocker_object ==
            curve_route_visibility.value().visibility.samples[index].blocker_object);
    }
    REQUIRE(!ui.evaluate_curve_route_visibility(
        primary_centerline_view->identity, observation_snapshot.project_revision.next(),
        {6.0, 26.0, 1.5}, 8U));
    const auto before_curve_observation = ui.snapshot();
    REQUIRE(before_curve_observation.project_revision == observation_snapshot.project_revision);
    REQUIRE(before_curve_observation.operations.size() == observation_snapshot.operations.size());
    carto::editor::RouteVisibilityQuery landmark_route{
        {{0.0, 26.0, 1.5}, {1.5, 26.0, 1.5}, {3.0, 26.0, 1.5}}, {},
        {6.0, 26.0, 1.5}, observation_snapshot.project_revision, std::nullopt};
    const auto landmark_visibility = ui.evaluate_route_visibility(landmark_route);
    REQUIRE(landmark_visibility);
    REQUIRE(landmark_visibility.value().samples.size() ==
        landmark_route.observer_positions.size());
    REQUIRE(landmark_visibility.value().source_revision ==
        observation_snapshot.viewport.project_revision);
    REQUIRE(landmark_visibility.value().triangles_tested > 0U);
    REQUIRE(landmark_visibility.value().validate(
        landmark_route, {}, observation_snapshot.project_revision));
    const auto repeated_landmark_visibility = ui.evaluate_route_visibility(landmark_route);
    REQUIRE(repeated_landmark_visibility);
    for (std::size_t index = 0U;
         index < landmark_visibility.value().samples.size(); ++index) {
        REQUIRE(repeated_landmark_visibility.value().samples[index].visible ==
            landmark_visibility.value().samples[index].visible);
        REQUIRE(repeated_landmark_visibility.value().samples[index].blocker_object ==
            landmark_visibility.value().samples[index].blocker_object);
    }
    carto::editor::LandmarkViewshedQuery forest_viewshed{
        {0.0, 30.0, 1.5}, std::nullopt, 2U, 2U,
        {{5.0, 26.0, 0.5}, {5.0, 26.0, 2.5},
         {7.0, 26.0, 0.5}, {7.0, 26.0, 2.5}},
        observation_snapshot.project_revision};
    const auto viewshed = ui.evaluate_landmark_viewshed(forest_viewshed);
    REQUIRE(viewshed);
    REQUIRE(viewshed.value().cells.size() == 4U);
    REQUIRE(viewshed.value().visible_cells <= 4U);
    REQUIRE(viewshed.value().validate(
        forest_viewshed, {}, observation_snapshot.project_revision));
    const auto repeated_viewshed = ui.evaluate_landmark_viewshed(forest_viewshed);
    REQUIRE(repeated_viewshed);
    REQUIRE(repeated_viewshed.value().visible_cells == viewshed.value().visible_cells);
    for (std::size_t index = 0U; index < viewshed.value().cells.size(); ++index) {
        REQUIRE(repeated_viewshed.value().cells[index].blocker_object ==
            viewshed.value().cells[index].blocker_object);
    }
    REQUIRE(ui.snapshot().project_revision == observation_snapshot.project_revision);
    REQUIRE(ui.snapshot().operations.size() == observation_snapshot.operations.size());

    const auto fixture_directory = std::filesystem::current_path() /
        "condition-authoring-fixtures" /
        ("rural-road-forest-run-" + std::to_string(stamp));
    std::filesystem::create_directories(fixture_directory);
    const auto project_path = fixture_directory / "condition-rural-road-forest.carto";
    REQUIRE(ui.dispatch(carto::application::SaveProjectAction{project_path}));
    const auto saved = carto::project::ProjectDocument::load(project_path);
    REQUIRE(saved);
    REQUIRE(saved.value().road_surface_instance_receipts().size() == 2U);
    REQUIRE(saved.value().spatial_fields().size() == 7U);
    REQUIRE(saved.value().road_sources().size() == 2U);
    REQUIRE(saved.value().curve_paths().size() == 2U);
    REQUIRE(saved.value().road_sources().at(road.source_identity).source_revision ==
        revised_road.source_revision);
    REQUIRE(saved.value().road_sources().at(road.source_identity).width_meters ==
        revised_road.width_meters);
    REQUIRE(saved.value().road_sources().at(road.source_identity).drainage_intent ==
        carto::project::RoadDrainageIntent::roadside_ditch);
    REQUIRE(saved.value().road_sources().at(secondary_trail.source_identity).surface_class ==
        carto::project::RoadSurfaceClass::trail);
    REQUIRE(saved.value().road_sources().at(secondary_trail.source_identity).function_class ==
        carto::project::RoadFunctionClass::pedestrian);
    const auto has_road_receipt = [&saved](std::string_view identity) {
        return std::any_of(
            saved.value().road_surface_instance_receipts().begin(),
            saved.value().road_surface_instance_receipts().end(),
            [identity](const auto& entry) {
                return entry.second.road_identity == identity;
            });
    };
    REQUIRE(has_road_receipt(road.source_identity));
    REQUIRE(has_road_receipt(secondary_trail.source_identity));
    const auto serialized_project = saved.value().serialize();
    const std::string temporary_source_path = root.string();
    const std::string portable_temporary_source_path = root.generic_string();
    const std::string temporary_source_identity = root.filename().string();
    REQUIRE(serialized_project.find(temporary_source_path) == std::string::npos);
    REQUIRE(serialized_project.find(portable_temporary_source_path) == std::string::npos);
    REQUIRE(serialized_project.find(temporary_source_identity) == std::string::npos);
    constexpr std::array<std::string_view, 7U> private_donor_markers{
        "VANTA", "STRATA", "TrueForm", "REMENENCE", "MENAGERIE", "OWH",
        "HiddenCanopy"};
    for (const std::string_view marker : private_donor_markers) {
        REQUIRE(serialized_project.find(marker) == std::string::npos);
    }

    carto::application::ApplicationSession reopened_session;
    carto::ui::UiController reopened_ui(reopened_session);
    REQUIRE(reopened_ui.dispatch(carto::application::OpenProjectAction{project_path}));
    REQUIRE(reopened_ui.snapshot().objects.size() == 2U);
    const auto reopened_road_terrain = reopened_session.road_terrain_preview(
        road.source_identity, std::string(terrain_identity),
        carto::application::RoadTerrainCoordinateFrame::world_xz_elevation_y, 8U);
    REQUIRE(reopened_road_terrain &&
        reopened_road_terrain.value().preview_digest == road_terrain.value().preview_digest);
    const auto reopened_secondary_surface = reopened_session.road_surface_preview(
        secondary_trail.source_identity, 8U);
    REQUIRE(reopened_secondary_surface &&
        reopened_secondary_surface.value().road_digest ==
            secondary_surface.value().road_digest &&
        reopened_secondary_surface.value().centerline_digest ==
            secondary_surface.value().centerline_digest);
    const auto reopened_vegetation = reopened_ui.preview_condition_vegetation(recipe.id);
    REQUIRE(reopened_vegetation);
    REQUIRE(reopened_vegetation.value().application_revision ==
        reopened_ui.snapshot().project_revision);
    REQUIRE(reopened_vegetation.value().preview_digest !=
        vegetation_preview.value().preview_digest);
    REQUIRE(reopened_vegetation.value().instances.size() ==
        vegetation_preview.value().instances.size());
    for (std::size_t index = 0U; index < vegetation_preview.value().instances.size(); ++index) {
        REQUIRE(reopened_vegetation.value().instances[index].stable_id ==
            vegetation_preview.value().instances[index].stable_id);
    }

    REQUIRE(ui.dispatch(carto::application::UndoAction{}));
    REQUIRE(!ui.evaluate_route_visibility(landmark_route));
    REQUIRE(!ui.evaluate_curve_route_visibility(
        primary_centerline_view->identity, observation_snapshot.project_revision,
        {6.0, 26.0, 1.5}, 8U));
    REQUIRE(!ui.evaluate_landmark_viewshed(forest_viewshed));
    REQUIRE(ui.snapshot().objects.size() == 1U);
    REQUIRE(ui.dispatch(carto::application::RedoAction{}));
    REQUIRE(ui.snapshot().objects.size() == 2U);

    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
}

void ui_vegetation_recipe_commits_reopens_undoes_and_rejects_stale_sources() {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto root = std::filesystem::temp_directory_path() /
        ("carto-vegetation-ui-" + std::to_string(stamp));
    std::filesystem::create_directories(root);
    const auto source_path = root / "forest-authoring.carto-condition-authoring";
    const auto project_path = root / "condition-conifer-family.carto";

    carto::application::ApplicationSession session;
    carto::ui::UiController ui(session);
    REQUIRE(ui.dispatch(carto::application::NewProjectAction{"Cartographer conifer family"}));
    REQUIRE(ui.import_condition_fixture());
    auto authoring = carto::condition::make_condition_fixture();
    REQUIRE(authoring);
    auto fields = make_forest_fields(authoring.value());
    auto recipe = authoring.value().vegetation_scatters.front();
    REQUIRE(recipe.species_asset_id == authoring.value().assets.front().id);
    authoring.value().vegetation_scatters.clear();
    for (const auto& [identity, field] : fields) {
        REQUIRE(ui.dispatch(carto::application::CreateSpatialFieldAction{
            identity, field.semantic(), field.origin_uv(), field.cell_size(), field.width(),
            field.height(), field.minimum_value(), field.maximum_value(),
            field.values().front()}));
    }
    authoring.value().project_revision = ui.snapshot().condition.authored_revision.next();
    {
        std::ofstream stream(source_path, std::ios::binary);
        REQUIRE(stream.good());
        stream << authoring.value().serialize();
        REQUIRE(stream.good());
    }
    REQUIRE(ui.import_condition_authoring(source_path));
    REQUIRE(ui.snapshot().condition.assets.front().asset_class == "vegetation");
    REQUIRE(ui.dispatch(carto::application::SaveProjectAction{project_path}));

    const auto plant_source_path = root / "conifer-prototype.obj";
    {
        std::ofstream mesh(plant_source_path, std::ios::binary);
        REQUIRE(mesh.good());
        mesh << "v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n";
        REQUIRE(mesh.good());
    }
    const auto imported_plant = ui.import_mesh(
        plant_source_path, carto::ui::MeshImportFormat::obj);
    REQUIRE(imported_plant && imported_plant.value().source_linked);
    const auto source_snapshot = ui.snapshot();
    const auto plant_source = std::find_if(source_snapshot.source_assets.begin(),
        source_snapshot.source_assets.end(), [&imported_plant](const auto& item) {
            return item.identity == imported_plant.value().source_identity;
        });
    REQUIRE(plant_source != source_snapshot.source_assets.end());
    constexpr std::string_view mesh_prefix = "mesh.asset.";
    REQUIRE(plant_source->artifact_identity.starts_with(mesh_prefix));
    const std::string_view mesh_id_text = std::string_view(
        plant_source->artifact_identity).substr(mesh_prefix.size());
    std::uint64_t mesh_asset = 0U;
    const auto parsed_mesh = std::from_chars(mesh_id_text.data(),
        mesh_id_text.data() + mesh_id_text.size(), mesh_asset);
    REQUIRE(!mesh_id_text.empty() && parsed_mesh.ec == std::errc{} &&
        parsed_mesh.ptr == mesh_id_text.data() + mesh_id_text.size() && mesh_asset != 0U);
    const auto prototype = std::find_if(source_snapshot.objects.begin(),
        source_snapshot.objects.end(), [mesh_asset](const auto& item) {
            return item.object.mesh_asset == mesh_asset &&
                !item.object.parent.has_value() && item.object.visible && !item.object.locked;
        });
    REQUIRE(prototype != source_snapshot.objects.end());
    const auto prototype_id = prototype->object.id;

    const auto project_revision_before_preview = ui.snapshot().project_revision;
    recipe.species_asset_id = 0U;
    recipe.plant_source = carto::condition::PlantSpeciesSourceBinding{
        plant_source->identity, plant_source->source_generation,
        plant_source->source_digest};
    REQUIRE(ui.add_condition_vegetation_scatter(recipe));
    REQUIRE(ui.snapshot().condition.vegetation_scatter_count == 1U);
    REQUIRE(ui.snapshot().condition.vegetation_scatter_ids.front() == recipe.id);
    REQUIRE(ui.snapshot().operations.back().category == carto::ui::OperationCategory::provider);
    const auto preview = ui.preview_condition_vegetation(recipe.id);
    REQUIRE(preview);
    REQUIRE(preview.value().instances.size() == 16U);
    REQUIRE(preview.value().plant_source.has_value());
    REQUIRE(preview.value().plant_source->identity == plant_source->identity);
    REQUIRE(preview.value().application_revision == ui.snapshot().project_revision);
    REQUIRE(ui.snapshot().project_revision > project_revision_before_preview);
    const auto revision_after_authoring = ui.snapshot().project_revision;
    const auto repeated_preview = ui.preview_condition_vegetation(recipe.id);
    REQUIRE(repeated_preview &&
        repeated_preview.value().preview_digest == preview.value().preview_digest &&
        repeated_preview.value().application_revision == preview.value().application_revision &&
        repeated_preview.value().instances.size() == preview.value().instances.size());
    for (std::size_t index = 0U; index < preview.value().instances.size(); ++index) {
        REQUIRE(repeated_preview.value().instances[index].stable_id ==
            preview.value().instances[index].stable_id);
    }
    REQUIRE(ui.snapshot().project_revision == revision_after_authoring);

    REQUIRE(ui.dispatch(carto::application::SaveProjectAction{project_path}));
    carto::application::ApplicationSession reopened_session;
    carto::ui::UiController reopened_ui(reopened_session);
    REQUIRE(reopened_ui.dispatch(carto::application::OpenProjectAction{project_path}));
    REQUIRE(reopened_ui.snapshot().condition.vegetation_scatter_count == 1U);
    const auto reopened_preview = reopened_ui.preview_condition_vegetation(recipe.id);
    REQUIRE(reopened_preview);
    REQUIRE(reopened_preview.value().preview_digest == preview.value().preview_digest);
    REQUIRE(reopened_preview.value().plant_source.has_value());
    REQUIRE(reopened_preview.value().application_revision == preview.value().application_revision);
    REQUIRE(reopened_preview.value().instances.size() == preview.value().instances.size());
    for (std::size_t index = 0U; index < preview.value().instances.size(); ++index) {
        REQUIRE(reopened_preview.value().instances[index].stable_id ==
            preview.value().instances[index].stable_id);
    }

    REQUIRE(ui.dispatch(carto::application::CreateBoxAction{
        "Unrelated revision change", {1.0, 1.0, 1.0}}));
    const auto stale_after_unrelated_edit = ui.commit_condition_vegetation_preview(
        preview.value(), prototype_id);
    REQUIRE(!stale_after_unrelated_edit);
    const auto fresh_preview = ui.preview_condition_vegetation(recipe.id);
    REQUIRE(fresh_preview && fresh_preview.value().preview_digest !=
        preview.value().preview_digest);
    REQUIRE(fresh_preview.value().instances.size() == preview.value().instances.size());
    for (std::size_t index = 0U; index < preview.value().instances.size(); ++index) {
        REQUIRE(fresh_preview.value().instances[index].stable_id ==
            preview.value().instances[index].stable_id);
    }

    const auto before_rejection = ui.snapshot();
    const auto wrong_source = ui.commit_condition_vegetation_preview(fresh_preview.value(),
        carto::scene::ObjectId{std::numeric_limits<std::uint64_t>::max()});
    REQUIRE(!wrong_source);
    REQUIRE(ui.snapshot().project_revision == before_rejection.project_revision);
    REQUIRE(ui.snapshot().objects.size() == before_rejection.objects.size());

    const auto committed = ui.commit_condition_vegetation_preview(
        fresh_preview.value(), prototype_id);
    if (!committed) {
        throw TestFailure("CONDITION vegetation commit failed: " +
            committed.error().message);
    }
    REQUIRE(committed.value().document_changed);
    const auto after_commit = ui.snapshot();
    REQUIRE(after_commit.objects.size() == before_rejection.objects.size() +
        fresh_preview.value().instances.size());
    REQUIRE(after_commit.operations.back().action ==
        "Commit CONDITION Vegetation Placements");
    REQUIRE(!ui.commit_condition_vegetation_preview(
        fresh_preview.value(), prototype_id));

    REQUIRE(ui.dispatch(carto::application::SaveProjectAction{project_path}));
    auto committed_project = carto::project::ProjectDocument::load(project_path);
    REQUIRE(committed_project && committed_project.value().validate());
    REQUIRE(committed_project.value().condition_vegetation_placement_receipts().size() ==
        fresh_preview.value().instances.size());
    for (const auto& placement : fresh_preview.value().instances) {
        const auto receipt = std::find_if(
            committed_project.value().condition_vegetation_placement_receipts().begin(),
            committed_project.value().condition_vegetation_placement_receipts().end(),
            [&placement](const auto& item) {
                return item.second.stable_placement_id == placement.stable_id;
            });
        REQUIRE(receipt != committed_project.value().condition_vegetation_placement_receipts().end());
        REQUIRE(receipt->second.recipe_id == recipe.id);
        REQUIRE(receipt->second.species_asset_id == recipe.species_asset_id);
        REQUIRE(receipt->second.mesh_asset == mesh_asset);
        REQUIRE(receipt->second.source_project_revision == fresh_preview.value().application_revision);
        REQUIRE(receipt->second.authoring_revision == fresh_preview.value().project_revision);
        REQUIRE(receipt->second.authoring_digest == fresh_preview.value().authoring_digest);
        REQUIRE(receipt->second.preview_digest == fresh_preview.value().preview_digest);
        REQUIRE(receipt->second.plant_source_identity == plant_source->identity);
        REQUIRE(receipt->second.plant_source_generation == plant_source->source_generation);
        REQUIRE(receipt->second.plant_source_digest == plant_source->source_digest);
        REQUIRE(receipt->second.fields.size() == preview.value().fields.size());
        REQUIRE(receipt->second.position.x == placement.position.x &&
            receipt->second.position.y == placement.position.y &&
            receipt->second.position.z == placement.position.z);
        REQUIRE(receipt->second.yaw_radians == placement.yaw_radians);
        REQUIRE(receipt->second.uniform_scale == placement.uniform_scale);
        REQUIRE(receipt->second.suitability == placement.suitability);
    }

    REQUIRE(ui.dispatch(carto::application::UndoAction{}));
    REQUIRE(ui.snapshot().objects.size() == before_rejection.objects.size());
    REQUIRE(ui.dispatch(carto::application::SaveProjectAction{project_path}));
    const auto undone_project = carto::project::ProjectDocument::load(project_path);
    REQUIRE(undone_project &&
        undone_project.value().condition_vegetation_placement_receipts().empty());
    REQUIRE(ui.dispatch(carto::application::RedoAction{}));
    REQUIRE(ui.snapshot().objects.size() == after_commit.objects.size());
    REQUIRE(ui.dispatch(carto::application::SaveProjectAction{project_path}));
    const auto redone_project = carto::project::ProjectDocument::load(project_path);
    REQUIRE(redone_project &&
        redone_project.value().condition_vegetation_placement_receipts().size() ==
            fresh_preview.value().instances.size());

    const auto duplicate_preview = ui.preview_condition_vegetation(recipe.id);
    REQUIRE(duplicate_preview && duplicate_preview.value().preview_digest !=
        fresh_preview.value().preview_digest);
    const auto before_duplicate = ui.snapshot();
    REQUIRE(!ui.commit_condition_vegetation_preview(
        duplicate_preview.value(), prototype_id));
    REQUIRE(ui.snapshot().project_revision == before_duplicate.project_revision);
    REQUIRE(ui.snapshot().objects.size() == before_duplicate.objects.size());

    carto::application::ApplicationSession final_session;
    carto::ui::UiController final_ui(final_session);
    REQUIRE(final_ui.dispatch(carto::application::OpenProjectAction{project_path}));
    const auto final_preview = final_ui.preview_condition_vegetation(recipe.id);
    REQUIRE(final_preview && final_preview.value().plant_source.has_value());
    REQUIRE(final_preview.value().application_revision == final_ui.snapshot().project_revision);
    REQUIRE(final_preview.value().instances.size() == fresh_preview.value().instances.size());
    for (std::size_t index = 0U; index < fresh_preview.value().instances.size(); ++index) {
        REQUIRE(final_preview.value().instances[index].stable_id ==
            fresh_preview.value().instances[index].stable_id);
    }

    constexpr std::array<std::string_view, 7U> private_donor_markers{
        "VANTA", "STRATA", "TrueForm", "REMENENCE", "MENAGERIE", "OWH",
        "HiddenCanopy"};
    const auto current_serialized_project = redone_project.value().serialize();
    REQUIRE(current_serialized_project.find(root.string()) == std::string::npos);
    REQUIRE(current_serialized_project.find(root.generic_string()) == std::string::npos);
    for (const std::string_view marker : private_donor_markers) {
        REQUIRE(current_serialized_project.find(marker) == std::string::npos);
    }

    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
}

void condition_gas_price_board_preserves_sign_source_and_material_lineage() {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto root = std::filesystem::current_path() / "build" / "test-temp" /
        ("cartographer-condition-gas-board-" + std::to_string(stamp));
    std::filesystem::create_directories(root);

    carto::application::ApplicationSession session;
    carto::ui::UiController ui(session);
    REQUIRE(ui.dispatch(carto::application::NewProjectAction{
        "CONDITION Pine Ridge gas board"}));

    carto::geometry::SignTextSource sign;
    sign.source_identity = "condition.pine-ridge.gas-board";
    sign.source_revision = carto::core::Revision(1U);
    sign.text = "PINE RIDGE\nREGULAR $4.29\nDIESEL $4.59";
    sign.board_width = 4.8;
    sign.board_height = 1.6;
    sign.border = 0.1;
    sign.glyph_height = 0.28;
    sign.line_spacing = 0.05;
    REQUIRE(sign.validate());
    REQUIRE(ui.dispatch(carto::application::SetSignTextSourceAction{
        ui.snapshot().project_revision, sign}));

    const auto project_revision_before_stale_create = ui.snapshot().project_revision;
    const auto objects_before_stale_create = ui.snapshot().objects.size();
    const auto stale_source_create = ui.dispatch(
        carto::application::CreateSignTextObjectAction{
            project_revision_before_stale_create, sign.source_identity,
            carto::core::Revision(2U)});
    REQUIRE(!stale_source_create &&
        stale_source_create.error().code == carto::core::ErrorCode::stale_data &&
        ui.snapshot().project_revision == project_revision_before_stale_create &&
        ui.snapshot().objects.size() == objects_before_stale_create);

    REQUIRE(ui.dispatch(carto::application::CreateSignTextObjectAction{
        ui.snapshot().project_revision, sign.source_identity, sign.source_revision}));
    REQUIRE(ui.snapshot().objects.size() == 1U);
    REQUIRE(ui.dispatch(carto::application::UndoAction{}));
    REQUIRE(ui.snapshot().objects.empty());
    REQUIRE(ui.dispatch(carto::application::RedoAction{}));
    REQUIRE(ui.snapshot().objects.size() == 1U);

    const auto project_path = root / "pine-ridge-gas-board.carto";
    REQUIRE(ui.dispatch(carto::application::SaveProjectAction{project_path}));
    const auto saved = carto::project::ProjectDocument::load(project_path);
    REQUIRE(saved && saved.value().validate());
    REQUIRE(saved.value().sign_text_instance_receipts().size() == 1U);
    const auto& receipt = saved.value().sign_text_instance_receipts().begin()->second;
    REQUIRE(receipt.source_identity == sign.source_identity &&
        receipt.source_revision == sign.source_revision &&
        receipt.source_snapshot == sign.serialize() &&
        receipt.source_digest == digest(sign.serialize()) && receipt.validate());
    const auto& mesh = saved.value().meshes().at(receipt.mesh_asset);
    const auto* slots = mesh.attributes().find("cartographer.material_slot");
    REQUIRE(slots != nullptr &&
        slots->descriptor.domain == carto::attributes::AttributeDomain::face &&
        slots->descriptor.type == carto::attributes::AttributeType::uint32 &&
        slots->values.size() == mesh.face_count());
    std::size_t panel_faces = 0U;
    std::size_t lettering_faces = 0U;
    for (const auto& value : slots->values) {
        const auto* slot = std::get_if<std::uint32_t>(&value);
        REQUIRE(slot != nullptr);
        if (*slot == carto::geometry::kSignTextPanelMaterialSlot) ++panel_faces;
        else if (*slot == carto::geometry::kSignTextLetteringMaterialSlot) {
            ++lettering_faces;
        } else {
            REQUIRE(false);
        }
    }
    REQUIRE(panel_faces > 0U && lettering_faces > 0U &&
        panel_faces + lettering_faces == mesh.face_count());

    const carto::project::MaterialRegionKey panel_region{
        receipt.mesh_asset, carto::geometry::kSignTextPanelMaterialSlot};
    const carto::project::MaterialRegionKey lettering_region{
        receipt.mesh_asset, carto::geometry::kSignTextLetteringMaterialSlot};
    REQUIRE(ui.dispatch(carto::application::CreateMaterialAction{
        ui.snapshot().project_revision, "Pine Ridge sign panel", {}}));
    const auto panel_material = ui.snapshot().materials.back().id;
    REQUIRE(ui.dispatch(carto::application::CreateMaterialAction{
        ui.snapshot().project_revision, "Pine Ridge lettering", {}}));
    const auto lettering_material = ui.snapshot().materials.back().id;
    REQUIRE(ui.dispatch(carto::application::SetMaterialRegionAssignmentAction{
        ui.snapshot().project_revision, panel_region, panel_material, std::nullopt}));
    REQUIRE(ui.dispatch(carto::application::SetMaterialRegionAssignmentAction{
        ui.snapshot().project_revision, lettering_region, lettering_material,
        std::nullopt}));
    REQUIRE(ui.dispatch(carto::application::UndoAction{}));
    REQUIRE(ui.dispatch(carto::application::UndoAction{}));
    REQUIRE(ui.snapshot().material_assignments.empty());
    REQUIRE(ui.dispatch(carto::application::RedoAction{}));
    REQUIRE(ui.dispatch(carto::application::RedoAction{}));
    REQUIRE(ui.snapshot().material_assignments.size() == 2U);

    auto alternate_wording = sign;
    alternate_wording.source_revision = carto::core::Revision(2U);
    alternate_wording.text = "PINE RIDGE\nREGULAR $4.19\nDIESEL $4.49";
    REQUIRE(alternate_wording.validate());
    REQUIRE(ui.dispatch(carto::application::SetSignTextSourceAction{
        ui.snapshot().project_revision, alternate_wording}));
    const auto stale_old_revision_create = ui.dispatch(
        carto::application::CreateSignTextObjectAction{
            ui.snapshot().project_revision, sign.source_identity, sign.source_revision});
    REQUIRE(!stale_old_revision_create &&
        stale_old_revision_create.error().code == carto::core::ErrorCode::stale_data &&
        ui.snapshot().objects.size() == 1U);

    REQUIRE(ui.dispatch(carto::application::SaveProjectAction{project_path}));
    const auto updated = carto::project::ProjectDocument::load(project_path);
    REQUIRE(updated && updated.value().validate());
    const auto& active_source = updated.value().sign_text_sources().at(sign.source_identity);
    REQUIRE(active_source.source_revision == alternate_wording.source_revision &&
        active_source.serialize() == alternate_wording.serialize());
    const auto& historical_receipt =
        updated.value().sign_text_instance_receipts().begin()->second;
    REQUIRE(historical_receipt.source_revision == sign.source_revision &&
        historical_receipt.source_snapshot == sign.serialize() &&
        historical_receipt.source_digest == digest(sign.serialize()));
    const auto& assignments = updated.value().material_catalog().assignments();
    REQUIRE(assignments.at(panel_region).material == panel_material &&
        assignments.at(lettering_region).material == lettering_material);

    carto::application::ApplicationSession reopened_session;
    carto::ui::UiController reopened_ui(reopened_session);
    REQUIRE(reopened_ui.dispatch(carto::application::OpenProjectAction{project_path}));
    const auto reopened = reopened_ui.snapshot();
    REQUIRE(reopened.sign_text_sources.size() == 1U &&
        reopened.sign_text_sources.front().source.source_revision ==
            alternate_wording.source_revision &&
        reopened.sign_text_sources.front().source.text == alternate_wording.text);
    REQUIRE(reopened.material_assignments.size() == 2U);

    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
}

void ui_safe_place_branch_is_journaled_comparable_and_recoverable() {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto root = std::filesystem::current_path() / "build" / "test-temp" /
        ("cartographer-condition-state-branch-ui-" + std::to_string(stamp));
    std::filesystem::create_directories(root);

    carto::application::ApplicationSession session;
    carto::ui::UiController ui(session);
    REQUIRE(ui.dispatch(carto::application::NewProjectAction{"CONDITION Safe Place branches"}));
    REQUIRE(ui.import_condition_fixture());
    const auto before = session.condition_authoring();
    REQUIRE(before);
    carto::condition::AuthoredStateDelta delta;
    delta.id = 1U;
    delta.target_record_id = before.value().safe_places.front().id;
    delta.target_asset_id = before.value().safe_places.front().asset_id;
    delta.target_kind = carto::condition::StateTargetKind::safe_place_profile;
    delta.property = carto::condition::StateProperty::privacy;
    delta.before.numeric_value = before.value().safe_places.front().privacy;
    delta.after.numeric_value = 0.55;
    carto::condition::AuthoredStateBranch branch;
    branch.id = 1U;
    branch.name = "storm-night-shelter";
    branch.applicability = "after-storm-night";
    branch.deltas.push_back(delta);

    const auto project_revision_before = ui.snapshot().project_revision;
    REQUIRE(ui.add_condition_state_branch(branch));
    REQUIRE(ui.snapshot().condition.state_branch_count == 1U);
    REQUIRE(ui.snapshot().condition.state_branches.front().name == branch.name);
    REQUIRE(ui.snapshot().condition.surface_states.size() == before.value().surface_states.size());
    REQUIRE(ui.snapshot().condition.surface_states.front().wetness ==
        before.value().surface_states.front().wetness);
    REQUIRE(ui.snapshot().operations.back().category == carto::ui::OperationCategory::provider);
    const auto comparison = ui.compare_condition_state_branches(0U, 1U);
    REQUIRE(comparison);
    REQUIRE(comparison.value().differences.size() == 1U);
    REQUIRE(comparison.value().differences.front().right_value.numeric_value == 0.55);
    REQUIRE(ui.snapshot().project_revision > project_revision_before);
    const auto revision_after_add = ui.snapshot().project_revision;
    REQUIRE(ui.compare_condition_state_branches(0U, 1U));
    REQUIRE(ui.snapshot().project_revision == revision_after_add);

    carto::condition::AuthoredStateDelta wetness_delta;
    wetness_delta.id = 1U;
    wetness_delta.target_record_id = before.value().surface_states.front().id;
    wetness_delta.target_asset_id = before.value().surface_states.front().asset_id;
    wetness_delta.target_kind = carto::condition::StateTargetKind::surface_state;
    wetness_delta.property = carto::condition::StateProperty::wetness;
    wetness_delta.before.numeric_value = before.value().surface_states.front().wetness;
    wetness_delta.after.numeric_value = 0.9;
    carto::condition::AuthoredStateBranch wetness_branch;
    wetness_branch.id = 2U;
    wetness_branch.parent_branch_id = branch.id;
    wetness_branch.name = "rain-on-sign";
    wetness_branch.applicability = "after-heavy-rain";
    wetness_branch.deltas.push_back(wetness_delta);
    REQUIRE(ui.add_condition_state_branch(wetness_branch));
    REQUIRE(ui.snapshot().condition.state_branch_count == 2U);
    const auto wetness_comparison = ui.compare_condition_state_branches(1U, 2U);
    REQUIRE(wetness_comparison);
    REQUIRE(wetness_comparison.value().differences.size() == 1U);
    REQUIRE(wetness_comparison.value().differences.front().property ==
        carto::condition::StateProperty::wetness);
    REQUIRE(wetness_comparison.value().differences.front().right_value.numeric_value == 0.9);
    const auto final_privacy_comparison = ui.compare_condition_state_branches(0U, 1U);
    REQUIRE(final_privacy_comparison);

    const std::vector<carto::condition::StateMergeChoice> wetness_choice{{
        0U, carto::condition::StateMergeSide::right}};
    const auto merge_revision_before = ui.snapshot().project_revision;
    const auto merged = ui.resolve_condition_state_branch_merge(
        wetness_comparison.value(), wetness_choice,
        "resolved-rain-state", "explicitly selected right-side wetness");
    REQUIRE(merged);
    REQUIRE(ui.snapshot().project_revision > merge_revision_before);
    REQUIRE(ui.snapshot().condition.state_branch_count == 3U);
    REQUIRE(ui.snapshot().operations.back().category == carto::ui::OperationCategory::provider);
    const auto merged_comparison = ui.compare_condition_state_branches(1U, 3U);
    REQUIRE(merged_comparison);
    REQUIRE(merged_comparison.value().differences.size() == 1U);
    REQUIRE(merged_comparison.value().differences.front().right_value.numeric_value == 0.9);
    const auto revision_after_merge = ui.snapshot().project_revision;
    REQUIRE(!ui.resolve_condition_state_branch_merge(
        wetness_comparison.value(), wetness_choice, "stale-merge"));
    REQUIRE(ui.snapshot().project_revision == revision_after_merge);

    const auto path = root / "safe-place-states.carto";
    REQUIRE(ui.dispatch(carto::application::SaveProjectAction{path}));
    carto::application::ApplicationSession reopened_session;
    carto::ui::UiController reopened_ui(reopened_session);
    REQUIRE(reopened_ui.dispatch(carto::application::OpenProjectAction{path}));
    REQUIRE(reopened_ui.snapshot().condition.state_branch_count == 3U);
    const auto reopened_comparison = reopened_ui.compare_condition_state_branches(1U, 3U);
    REQUIRE(reopened_comparison);
    REQUIRE(reopened_comparison.value().comparison_digest == merged_comparison.value().comparison_digest);

    REQUIRE(ui.dispatch(carto::application::UndoAction{}));
    REQUIRE(ui.snapshot().condition.state_branch_count == 2U);
    REQUIRE(!ui.compare_condition_state_branches(1U, 3U));
    REQUIRE(ui.dispatch(carto::application::UndoAction{}));
    REQUIRE(ui.snapshot().condition.state_branch_count == 1U);
    REQUIRE(ui.compare_condition_state_branches(0U, 1U));
    REQUIRE(ui.dispatch(carto::application::UndoAction{}));
    REQUIRE(ui.snapshot().condition.state_branch_count == 0U);
    REQUIRE(!ui.compare_condition_state_branches(0U, 1U));
    REQUIRE(ui.dispatch(carto::application::RedoAction{}));
    REQUIRE(ui.snapshot().condition.state_branch_count == 1U);
    REQUIRE(ui.dispatch(carto::application::RedoAction{}));
    REQUIRE(ui.snapshot().condition.state_branch_count == 2U);
    REQUIRE(ui.dispatch(carto::application::RedoAction{}));
    REQUIRE(ui.snapshot().condition.state_branch_count == 3U);

    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
}

void ui_audio_authoring_uses_project_transaction_and_undo_history() {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto root = std::filesystem::current_path() / "build" / "test-temp" /
        ("cartographer-condition-audio-tests-" + std::to_string(stamp));
    std::filesystem::create_directories(root);

    carto::application::ApplicationSession session;
    carto::ui::UiController ui(session);
    REQUIRE(ui.dispatch(carto::application::NewProjectAction{"CONDITION audio authoring"}));
    REQUIRE(ui.dispatch(carto::application::CreateBoxAction{
        "Audio fixture room", {4.0, 3.0, 6.0}}));
    REQUIRE(ui.import_condition_fixture());
    auto with_clip = fixture_with_general_store_audio_evidence();
    with_clip.project_revision = ui.snapshot().condition.authored_revision.next();
    const auto source_path = root / "fixture.carto-condition-authoring";
    {
        std::ofstream stream(source_path, std::ios::binary);
        REQUIRE(stream.good());
        stream << with_clip.serialize();
        REQUIRE(stream.good());
    }
    REQUIRE(ui.import_condition_authoring(source_path));

    std::vector<carto::condition::AuthoredAudioSource> authored_sources;
    for (std::size_t index = 0U; index < 3U; ++index) {
        auto source = carto::condition::AuthoredAudioSource{};
        source.asset_id = static_cast<carto::condition::Id>(4U + index);
        source.clip_source_id = static_cast<carto::condition::Id>(2U + index);
        source.local_transform.translation.x = static_cast<double>(index) + 0.25;
        source.gain = 0.45;
        source.pitch = 1.0;
        source.loop = true;
        source.minimum_distance = 1.0;
        source.maximum_distance = 14.0 - static_cast<double>(index) * 2.0;
        source.bus = carto::condition::AudioBusClass::machinery;
        source.familiarity_role = index == 0U ? "store.refrigerator"
            : index == 1U ? "store.freezer" : "store.fluorescent";
        REQUIRE(ui.add_condition_audio_source(source));
        authored_sources.push_back(std::move(source));
    }

    auto store_zone = carto::condition::AuthoredAcousticZone{};
    store_zone.asset_id = 4U;
    store_zone.half_extents = {8.0, 3.0, 6.0};
    store_zone.acoustic_class = "commercial-interior";
    store_zone.reverb_intent = "general-store-room";
    store_zone.semantic_region = "general-store.main";
    REQUIRE(ui.add_condition_acoustic_zone(store_zone));
    auto cooler_zone = carto::condition::AuthoredAcousticZone{};
    cooler_zone.asset_id = 5U;
    cooler_zone.shape = carto::condition::AcousticZoneShape::sphere;
    cooler_zone.half_extents = {};
    cooler_zone.radius = 3.0;
    cooler_zone.local_transform.translation.z = -1.5;
    cooler_zone.acoustic_class = "cooler";
    cooler_zone.reverb_intent = "cooler-short";
    cooler_zone.priority = 20U;
    cooler_zone.semantic_region = "general-store.cooler";
    REQUIRE(ui.add_condition_acoustic_zone(cooler_zone));
    REQUIRE(ui.snapshot().condition.audio_source_count == 3U);
    REQUIRE(ui.snapshot().condition.acoustic_zone_count == 2U);
    REQUIRE(ui.snapshot().operations.back().category == carto::ui::OperationCategory::provider);
    const auto authored_view = ui.snapshot().condition;
    REQUIRE(authored_view.audio_sources.size() == 3U);
    REQUIRE(authored_view.audio_sources[2].local_transform.translation.x == 2.25);
    REQUIRE(authored_view.audio_sources[2].maximum_distance == 10.0);
    REQUIRE(authored_view.acoustic_zones.size() == 2U);
    REQUIRE(authored_view.acoustic_zones[1].shape == "sphere");
    REQUIRE(authored_view.acoustic_zones[1].radius == 3.0);
    REQUIRE(authored_view.acoustic_zones[1].local_transform.translation.z == -1.5);

    const auto project_path = root / "audio-authoring.carto";
    const auto save_project = ui.dispatch(carto::application::SaveProjectAction{project_path});
    if (!save_project) throw TestFailure("save audio-authoring project: " + save_project.error().message);
    carto::application::ApplicationSession reopened;
    REQUIRE(carto::application::HumanApplicationAccess::dispatch(
        reopened, carto::application::OpenProjectAction{project_path}));
    REQUIRE(reopened.snapshot().condition.audio_source_count == 3U);
    REQUIRE(reopened.snapshot().condition.acoustic_zone_count == 2U);

    const auto undo_zone = ui.dispatch(carto::application::UndoAction{});
    if (!undo_zone) throw TestFailure("undo cooler acoustic zone: " + undo_zone.error().message);
    REQUIRE(ui.snapshot().condition.acoustic_zone_count == 1U);
    const auto undo_store_zone = ui.dispatch(carto::application::UndoAction{});
    if (!undo_store_zone) throw TestFailure("undo store acoustic zone: " + undo_store_zone.error().message);
    REQUIRE(ui.snapshot().condition.acoustic_zone_count == 0U);
    for (int index = 2; index >= 0; --index) {
        const auto undone = ui.dispatch(carto::application::UndoAction{});
        if (!undone) throw TestFailure("undo General Store audio source: " + undone.error().message);
        REQUIRE(ui.snapshot().condition.audio_source_count == static_cast<std::size_t>(index));
    }
    REQUIRE(ui.snapshot().objects.size() == 1U);

    const auto undo_clip_import = ui.dispatch(carto::application::UndoAction{});
    if (!undo_clip_import) throw TestFailure("undo clip-evidence import: " + undo_clip_import.error().message);
    REQUIRE(ui.snapshot().condition.source_count == 1U);
    const auto undo_fixture = ui.dispatch(carto::application::UndoAction{});
    if (!undo_fixture) throw TestFailure("undo CONDITION fixture: " + undo_fixture.error().message);
    REQUIRE(!ui.snapshot().condition.present);
    REQUIRE(ui.snapshot().objects.size() == 1U);

    const auto redo_fixture = ui.dispatch(carto::application::RedoAction{});
    if (!redo_fixture) throw TestFailure("redo CONDITION fixture: " + redo_fixture.error().message);
    REQUIRE(ui.snapshot().condition.present);
    const auto redo_clip_import = ui.dispatch(carto::application::RedoAction{});
    if (!redo_clip_import) throw TestFailure("redo clip-evidence import: " + redo_clip_import.error().message);
    REQUIRE(ui.snapshot().condition.source_count == 4U);
    for (std::size_t index = 1U; index <= 3U; ++index) {
        const auto redone = ui.dispatch(carto::application::RedoAction{});
        if (!redone) throw TestFailure("redo General Store audio source: " + redone.error().message);
        REQUIRE(ui.snapshot().condition.audio_source_count == index);
    }
    const auto redo_store_zone = ui.dispatch(carto::application::RedoAction{});
    if (!redo_store_zone) throw TestFailure("redo store acoustic zone: " + redo_store_zone.error().message);
    REQUIRE(ui.snapshot().condition.acoustic_zone_count == 1U);
    const auto redo_cooler_zone = ui.dispatch(carto::application::RedoAction{});
    if (!redo_cooler_zone) throw TestFailure("redo cooler acoustic zone: " + redo_cooler_zone.error().message);
    REQUIRE(ui.snapshot().condition.acoustic_zone_count == 2U);

    const auto project_revision = ui.snapshot().project_revision;
    auto duplicate = authored_sources.front();
    duplicate.id = 1U;
    REQUIRE(!ui.add_condition_audio_source(duplicate));
    REQUIRE(ui.snapshot().project_revision == project_revision);
    auto bad_clip = authored_sources.front();
    bad_clip.id = 0U;
    bad_clip.clip_source_id = 999U;
    REQUIRE(!ui.add_condition_audio_source(bad_clip));
    REQUIRE(ui.snapshot().project_revision == project_revision);

    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
}

void ui_wave_clip_import_is_review_gated_pathless_and_reversible() {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto root = std::filesystem::current_path() / "build" / "test-temp" /
        ("cartographer-condition-wave-import-" + std::to_string(stamp));
    std::filesystem::create_directories(root);
    const auto wav_bytes = make_wave(1U, 1U, 44100U, 16U,
        std::string{"\x01\x00\xff\x7f", 4U});
    const auto wav_path = root / "private-source-name.wav";
    {
        std::ofstream stream(wav_path, std::ios::binary);
        REQUIRE(stream.good());
        stream.write(wav_bytes.data(), static_cast<std::streamsize>(wav_bytes.size()));
        REQUIRE(stream.good());
    }

    carto::application::ApplicationSession session;
    carto::ui::UiController ui(session);
    REQUIRE(ui.dispatch(carto::application::NewProjectAction{"WAVE evidence import"}));
    REQUIRE(ui.import_condition_fixture());
    const auto before_import = ui.snapshot().project_revision;
    REQUIRE(ui.import_condition_audio_clip(wav_path));
    const auto imported = ui.snapshot();
    REQUIRE(imported.project_revision > before_import);
    REQUIRE(imported.condition.source_count == 2U);
    REQUIRE(imported.condition.sources.back().kind == "imported-asset");
    REQUIRE(!imported.condition.sources.back().reviewed);
    REQUIRE(imported.condition.sources.back().current);
    REQUIRE(imported.condition.sources.back().digest == digest(wav_bytes));
    REQUIRE(imported.condition.sources.back().locator.find(root.string()) == std::string::npos);
    REQUIRE(imported.condition.sources.back().diagnostics.size() == 1U);
    REQUIRE(imported.condition.sources.back().diagnostics.front().find("44100 Hz") !=
        std::string::npos);
    REQUIRE(!ui.build_condition_package());

    const auto authored = session.condition_authoring();
    REQUIRE(authored);
    const auto serialized = authored.value().serialize();
    REQUIRE(serialized.find(root.string()) == std::string::npos);
    REQUIRE(serialized.find("RIFF") == std::string::npos);
    REQUIRE(serialized.find(wav_bytes) == std::string::npos);

    REQUIRE(ui.set_condition_source_reviewed(2U, true));
    REQUIRE(ui.build_condition_package());
    const auto admission = ui.admit_condition_package();
    REQUIRE(admission);
    REQUIRE(admission.value().status == carto::condition::PackageAdmissionStatus::admit);
    const auto undo_review = ui.dispatch(carto::application::UndoAction{});
    if (!undo_review) throw TestFailure("undo WAVE source review: " + undo_review.error().message);
    REQUIRE(!ui.snapshot().condition.sources.back().reviewed);
    REQUIRE(!ui.build_condition_package());
    const auto redo_review = ui.dispatch(carto::application::RedoAction{});
    if (!redo_review) throw TestFailure("redo WAVE source review: " + redo_review.error().message);
    REQUIRE(ui.snapshot().condition.sources.back().reviewed);

    REQUIRE(ui.invalidate_condition_source(2U, "replacement", digest("replacement WAV")));
    REQUIRE(!ui.set_condition_source_reviewed(2U, true));
    REQUIRE(!ui.snapshot().condition.sources.back().current);

    const auto malformed_path = root / "malformed.wav";
    {
        std::ofstream stream(malformed_path, std::ios::binary);
        stream << "not a wave";
    }
    const auto before_rejection = ui.snapshot().project_revision;
    REQUIRE(!ui.import_condition_audio_clip(malformed_path));
    REQUIRE(ui.snapshot().project_revision == before_rejection);

    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
}

void ui_condition_anchors_and_prefabs_are_revision_bound_and_reversible() {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto root = std::filesystem::current_path() / "build" / "test-temp" /
        ("cartographer-condition-anchor-prefab-" + std::to_string(stamp));
    std::filesystem::create_directories(root);

    carto::application::ApplicationSession session;
    carto::ui::UiController ui(session);
    REQUIRE(ui.dispatch(carto::application::NewProjectAction{
        "CONDITION anchor and prefab authoring"}));
    REQUIRE(ui.import_condition_fixture());

    const auto initial = ui.snapshot();
    REQUIRE(initial.condition.interaction_anchors.size() ==
        initial.condition.interaction_anchor_count);
    REQUIRE(initial.condition.prefabs.size() == initial.condition.prefab_count);
    REQUIRE(initial.condition.variants.size() == initial.condition.variant_count);
    carto::condition::InteractionAnchor anchor;
    anchor.asset_id = initial.condition.assets.front().id;
    anchor.kind = carto::condition::AnchorKind::seat;
    anchor.interaction_class = "motel.room.seat";
    anchor.transform.translation = {1.25, 0.0, -0.75};
    const auto created_anchor = ui.add_condition_interaction_anchor(
        anchor, initial.project_revision, initial.condition.authored_revision);
    REQUIRE(created_anchor);

    const auto after_anchor = ui.snapshot();
    REQUIRE(after_anchor.condition.interaction_anchors.size() ==
        initial.condition.interaction_anchors.size() + 1U);
    const auto& projected_anchor = after_anchor.condition.interaction_anchors.back();
    REQUIRE(projected_anchor.id > initial.condition.interaction_anchors.back().id);
    REQUIRE(projected_anchor.asset_id == anchor.asset_id);
    REQUIRE(projected_anchor.kind == "seat");
    REQUIRE(projected_anchor.interaction_class == anchor.interaction_class);
    REQUIRE(projected_anchor.transform.translation.x == 1.25);
    REQUIRE(after_anchor.condition.authored_revision ==
        initial.condition.authored_revision.next());
    REQUIRE(!after_anchor.operations.empty());
    REQUIRE(after_anchor.operations.back().action ==
        "Add CONDITION Interaction Anchor");

    const auto stable_project_revision = after_anchor.project_revision;
    auto invalid_anchor = anchor;
    invalid_anchor.asset_id = 999999U;
    REQUIRE(!ui.add_condition_interaction_anchor(
        invalid_anchor, stable_project_revision,
        after_anchor.condition.authored_revision));
    invalid_anchor = anchor;
    invalid_anchor.approach_direction = {};
    REQUIRE(!ui.add_condition_interaction_anchor(
        invalid_anchor, stable_project_revision,
        after_anchor.condition.authored_revision));
    invalid_anchor = anchor;
    invalid_anchor.id = 77U;
    REQUIRE(!ui.add_condition_interaction_anchor(
        invalid_anchor, stable_project_revision,
        after_anchor.condition.authored_revision));
    REQUIRE(!ui.add_condition_interaction_anchor(
        anchor, initial.project_revision, initial.condition.authored_revision));
    REQUIRE(!ui.add_condition_interaction_anchor(
        anchor, stable_project_revision, initial.condition.authored_revision));
    REQUIRE(ui.snapshot().project_revision == stable_project_revision);

    carto::condition::PrefabRecord prefab;
    prefab.name = "Motel room assembly";
    prefab.asset_ids = {initial.condition.assets[0].id,
                        initial.condition.assets[2].id};
    prefab.sockets = {"entry.door", "sleeping.bed"};
    const auto prefab_base_revision = after_anchor.condition.authored_revision;
    REQUIRE(ui.add_condition_prefab(prefab, stable_project_revision,
                                    prefab_base_revision));
    const auto after_prefab = ui.snapshot();
    REQUIRE(after_prefab.condition.prefabs.size() ==
        initial.condition.prefabs.size() + 1U);
    const auto& projected_prefab = after_prefab.condition.prefabs.back();
    REQUIRE(projected_prefab.name == prefab.name);
    REQUIRE(projected_prefab.asset_ids == prefab.asset_ids);
    REQUIRE(projected_prefab.sockets == prefab.sockets);
    REQUIRE(projected_prefab.source_revision == prefab_base_revision);
    REQUIRE(projected_prefab.id > initial.condition.prefabs.back().id);
    REQUIRE(after_prefab.condition.authored_revision ==
        prefab_base_revision.next());
    REQUIRE(after_prefab.operations.back().action == "Add CONDITION Prefab");

    auto invalid_prefab = prefab;
    invalid_prefab.asset_ids = {999999U};
    REQUIRE(!ui.add_condition_prefab(
        invalid_prefab, after_prefab.project_revision,
        after_prefab.condition.authored_revision));
    invalid_prefab = prefab;
    invalid_prefab.sockets = {"duplicate", "duplicate"};
    REQUIRE(!ui.add_condition_prefab(
        invalid_prefab, after_prefab.project_revision,
        after_prefab.condition.authored_revision));
    invalid_prefab = prefab;
    invalid_prefab.asset_ids.clear();
    REQUIRE(!ui.add_condition_prefab(
        invalid_prefab, after_prefab.project_revision,
        after_prefab.condition.authored_revision));
    REQUIRE(!ui.add_condition_prefab(
        prefab, stable_project_revision, after_prefab.condition.authored_revision));
    REQUIRE(ui.snapshot().project_revision == after_prefab.project_revision);

    carto::condition::CollectionRecord collection;
    collection.name = "Motel room furnishings";
    collection.asset_ids = {initial.condition.assets[0].id,
                            initial.condition.assets[2].id};
    collection.semantic_class = "motel.room.furnishings";
    REQUIRE(ui.add_condition_collection(
        collection, after_prefab.project_revision,
        after_prefab.condition.authored_revision));
    const auto after_collection = ui.snapshot();
    REQUIRE(after_collection.condition.collections.size() ==
        initial.condition.collections.size() + 1U);
    const auto& projected_collection = after_collection.condition.collections.back();
    REQUIRE(projected_collection.name == collection.name);
    REQUIRE(projected_collection.asset_ids == collection.asset_ids);
    REQUIRE(projected_collection.semantic_class == collection.semantic_class);
    REQUIRE(after_collection.operations.back().action == "Add CONDITION Collection");
    auto invalid_collection = collection;
    invalid_collection.asset_ids = {999999U};
    REQUIRE(!ui.add_condition_collection(
        invalid_collection, after_collection.project_revision,
        after_collection.condition.authored_revision));
    invalid_collection = collection;
    invalid_collection.child_collection_ids = {999999U};
    REQUIRE(!ui.add_condition_collection(
        invalid_collection, after_collection.project_revision,
        after_collection.condition.authored_revision));
    invalid_collection = collection;
    invalid_collection.asset_ids.clear();
    invalid_collection.child_collection_ids.clear();
    REQUIRE(!ui.add_condition_collection(
        invalid_collection, after_collection.project_revision,
        after_collection.condition.authored_revision));
    invalid_collection = collection;
    invalid_collection.id = 700U;
    REQUIRE(!ui.add_condition_collection(
        invalid_collection, after_collection.project_revision,
        after_collection.condition.authored_revision));
    REQUIRE(ui.snapshot().project_revision == after_collection.project_revision);

    carto::condition::RegionKitManifest kit;
    kit.name = "Motel room kit";
    kit.asset_ids = {initial.condition.assets[0].id};
    kit.prefab_ids = {projected_prefab.id};
    kit.required_classes = {carto::condition::AssetClass::prop};
    kit.interaction_coverage = 0.8;
    kit.familiarity_coverage = 0.7;
    REQUIRE(ui.add_condition_region_kit(
        kit, after_collection.project_revision,
        after_collection.condition.authored_revision));
    const auto after_records = ui.snapshot();
    REQUIRE(after_records.condition.region_kits.size() ==
        initial.condition.region_kits.size() + 1U);
    const auto& projected_kit = after_records.condition.region_kits.back();
    REQUIRE(projected_kit.name == kit.name);
    REQUIRE(projected_kit.asset_ids == kit.asset_ids);
    REQUIRE(projected_kit.prefab_ids == kit.prefab_ids);
    REQUIRE(projected_kit.required_classes == std::vector<std::string>{"prop"});
    REQUIRE(projected_kit.interaction_coverage == kit.interaction_coverage);
    REQUIRE(projected_kit.familiarity_coverage == kit.familiarity_coverage);
    REQUIRE(after_records.operations.back().action == "Add CONDITION Region Kit");
    auto invalid_kit = kit;
    invalid_kit.prefab_ids = {999999U};
    REQUIRE(!ui.add_condition_region_kit(
        invalid_kit, after_records.project_revision,
        after_records.condition.authored_revision));
    invalid_kit = kit;
    invalid_kit.asset_ids.clear();
    invalid_kit.prefab_ids.clear();
    REQUIRE(!ui.add_condition_region_kit(
        invalid_kit, after_records.project_revision,
        after_records.condition.authored_revision));
    invalid_kit = kit;
    invalid_kit.interaction_coverage = 2.0;
    REQUIRE(!ui.add_condition_region_kit(
        invalid_kit, after_records.project_revision,
        after_records.condition.authored_revision));
    REQUIRE(!ui.add_condition_region_kit(
        kit, after_collection.project_revision,
        after_collection.condition.authored_revision));
    REQUIRE(ui.snapshot().project_revision == after_records.project_revision);

    const auto project_path = root / "anchors-and-prefabs.carto";
    REQUIRE(ui.dispatch(carto::application::SaveProjectAction{project_path}));
    carto::application::ApplicationSession reopened;
    REQUIRE(carto::application::HumanApplicationAccess::dispatch(
        reopened, carto::application::OpenProjectAction{project_path}));
    carto::ui::UiController reopened_ui(reopened);
    const auto reopened_snapshot = reopened_ui.snapshot();
    REQUIRE(reopened_snapshot.condition.interaction_anchors.size() ==
        after_records.condition.interaction_anchors.size());
    REQUIRE(reopened_snapshot.condition.prefabs.size() ==
        after_records.condition.prefabs.size());
    REQUIRE(reopened_snapshot.condition.collections.size() ==
        after_records.condition.collections.size());
    REQUIRE(reopened_snapshot.condition.region_kits.size() ==
        after_records.condition.region_kits.size());
    REQUIRE(reopened_snapshot.condition.digest == after_records.condition.digest);
    REQUIRE(reopened_snapshot.condition.interaction_anchors.back().id ==
        projected_anchor.id);
    REQUIRE(reopened_snapshot.condition.prefabs.back().source_revision ==
        prefab_base_revision);

    REQUIRE(ui.dispatch(carto::application::UndoAction{}));
    REQUIRE(ui.snapshot().condition.region_kits.size() ==
        initial.condition.region_kits.size());
    REQUIRE(ui.dispatch(carto::application::RedoAction{}));
    REQUIRE(ui.snapshot().condition.digest == after_records.condition.digest);
    REQUIRE(ui.dispatch(carto::application::UndoAction{}));
    REQUIRE(ui.dispatch(carto::application::UndoAction{}));
    REQUIRE(ui.snapshot().condition.collections.size() ==
        initial.condition.collections.size());
    REQUIRE(ui.dispatch(carto::application::UndoAction{}));
    REQUIRE(ui.snapshot().condition.prefabs.size() + 1U ==
        after_records.condition.prefabs.size());
    REQUIRE(ui.dispatch(carto::application::RedoAction{}));
    REQUIRE(ui.snapshot().condition.prefabs.size() ==
        after_records.condition.prefabs.size());
    REQUIRE(ui.dispatch(carto::application::UndoAction{}));
    auto new_prefab = prefab;
    new_prefab.name = "Motel room assembly revision two";
    const auto after_undo = ui.snapshot();
    REQUIRE(ui.add_condition_prefab(
        new_prefab, after_undo.project_revision,
        after_undo.condition.authored_revision));
    const auto after_replacement = ui.snapshot();
    REQUIRE(after_replacement.condition.prefabs.back().id > projected_prefab.id);
    REQUIRE(after_replacement.condition.prefabs.back().name == new_prefab.name);

    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
}

void ui_condition_prefab_instances_are_atomic_revision_bound_and_fail_closed() {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto root = std::filesystem::current_path() / "build" / "test-temp" /
        ("cartographer-condition-prefab-instance-" + std::to_string(stamp));
    std::filesystem::create_directories(root);
    const auto project_path = root / "prefab-instance.carto";

    carto::application::ApplicationSession session;
    carto::ui::UiController ui(session);
    REQUIRE(ui.dispatch(carto::application::NewProjectAction{
        "CONDITION prefab instance acceptance"}));
    REQUIRE(ui.dispatch(carto::application::CreateBoxAction{
        "Motel source room", {8.0, 3.0, 6.0}}));
    REQUIRE(ui.dispatch(carto::application::CreateBoxAction{
        "Motel source door", {0.9, 2.0, 0.12}}));
    auto state = ui.snapshot();
    REQUIRE(state.objects.size() == 2U);
    const carto::scene::ObjectId source_room = state.objects[0].object.id;
    const carto::scene::ObjectId source_door = state.objects[1].object.id;
    carto::core::Transform room_transform;
    room_transform.translation = {10.0, 0.0, 20.0};
    room_transform.scale = {2.0, 1.0, 2.0};
    REQUIRE(ui.dispatch(carto::application::SetObjectTransformAction{
        source_room, room_transform, state.project_revision}));
    state = ui.snapshot();
    carto::core::Transform door_transform;
    door_transform.translation = {14.0, 0.0, 22.0};
    REQUIRE(ui.dispatch(carto::application::SetObjectTransformAction{
        source_door, door_transform, state.project_revision}));

    auto authoring = carto::condition::make_condition_fixture();
    REQUIRE(authoring);
    for (auto& asset : authoring.value().assets) asset.authored_object_id = 0U;
    REQUIRE(ui.dispatch(carto::application::ImportConditionAuthoringAction{
        authoring.value().serialize()}));
    state = ui.snapshot();
    REQUIRE(ui.bind_condition_asset_to_object(
        state.condition.assets[0].id, source_room, state.project_revision,
        state.condition.authored_revision));
    state = ui.snapshot();
    REQUIRE(ui.bind_condition_asset_to_object(
        state.condition.assets[2].id, source_door, state.project_revision,
        state.condition.authored_revision));
    state = ui.snapshot();

    carto::condition::PrefabRecord prefab;
    prefab.name = "Motel room plus door";
    prefab.asset_ids = {state.condition.assets[0].id, state.condition.assets[2].id};
    prefab.sockets = {"room.entry"};
    REQUIRE(ui.add_condition_prefab(
        prefab, state.project_revision, state.condition.authored_revision));
    state = ui.snapshot();
    const auto prefab_id = state.condition.prefabs.back().id;
    REQUIRE(ui.build_condition_package());

    const auto unchanged_revision = state.project_revision;
    const auto unchanged_objects = state.objects.size();
    const auto unchanged_operations = state.operations.size();
    auto stale_project_action = carto::application::InstantiateConditionPrefabAction{
        carto::core::Revision{unchanged_revision.value() - 1U},
        state.condition.authored_revision, prefab_id,
        "Stale placement", {1.0, 0.0, 2.0}};
    REQUIRE(!ui.dispatch(stale_project_action));
    auto stale_authoring_action = stale_project_action;
    stale_authoring_action.expected_project_revision = unchanged_revision;
    stale_authoring_action.expected_authoring_revision = carto::core::Revision{
        state.condition.authored_revision.value() - 1U};
    REQUIRE(!ui.dispatch(stale_authoring_action));
    auto missing_prefab_action = carto::application::InstantiateConditionPrefabAction{
        unchanged_revision, state.condition.authored_revision, 999999U,
        "Missing prefab", {1.0, 0.0, 2.0}};
    REQUIRE(!ui.dispatch(missing_prefab_action));
    auto non_finite_position = carto::application::InstantiateConditionPrefabAction{
        unchanged_revision, state.condition.authored_revision, prefab_id,
        "Invalid placement", {std::numeric_limits<double>::infinity(), 0.0, 0.0}};
    REQUIRE(!ui.dispatch(non_finite_position));
    auto ai_attempt = carto::application::InstantiateConditionPrefabAction{
        unchanged_revision, state.condition.authored_revision, prefab_id,
        "AI placement", {1.0, 0.0, 2.0}};
    REQUIRE(!ui.submit_ai_proposal(ai_attempt));
    state = ui.snapshot();
    REQUIRE(state.project_revision == unchanged_revision);
    REQUIRE(state.objects.size() == unchanged_objects);
    REQUIRE(state.operations.size() == unchanged_operations);

    const carto::core::Vec3d placement{100.0, 0.0, 200.0};
    const auto before_instance = state;
    const auto created = ui.instantiate_condition_prefab(
        prefab_id, "Motel room instance A", placement,
        state.project_revision, state.condition.authored_revision);
    REQUIRE(created);
    REQUIRE(created.value().document_changed);
    REQUIRE(created.value().revision_before == before_instance.project_revision);
    REQUIRE(created.value().revision_after == before_instance.project_revision.next());
    REQUIRE(created.value().affected.objects.size() == 3U);
    state = ui.snapshot();
    REQUIRE(state.objects.size() == unchanged_objects + 3U);
    REQUIRE(state.condition.prefab_instances.size() == 1U);
    REQUIRE(state.condition.prefab_instance_count == 1U);
    REQUIRE(state.condition.prefab_instances.front().current);
    REQUIRE(state.condition.prefab_instances.front().member_count == 2U);
    REQUIRE(!state.condition.stale);
    REQUIRE(state.condition.publication_ready);
    REQUIRE(ui.build_condition_package());
    REQUIRE(state.operations.back().action == "Instantiate CONDITION Prefab");
    REQUIRE(state.condition.authored_revision ==
        before_instance.condition.authored_revision.next());

    const auto& placed = state.condition.prefab_instances.front();
    const auto root_object = std::find_if(state.objects.begin(), state.objects.end(),
        [&placed](const auto& item) {
            return item.object.id.value == placed.root_object_id;
        });
    REQUIRE(root_object != state.objects.end());
    REQUIRE(root_object->world_transform.translation.x == placement.x);
    REQUIRE(root_object->world_transform.translation.y == placement.y);
    REQUIRE(root_object->world_transform.translation.z == placement.z);
    REQUIRE(root_object->world_transform.scale.x == room_transform.scale.x);
    const auto authoring_after_instance = session.condition_authoring();
    REQUIRE(authoring_after_instance);
    REQUIRE(authoring_after_instance.value().prefab_instances.size() == 1U);
    const auto& record = authoring_after_instance.value().prefab_instances.front();
    REQUIRE(record.prefab_id == prefab_id);
    REQUIRE(record.captured_project_revision == before_instance.project_revision);
    REQUIRE(record.members.size() == 2U);
    for (const auto& member : record.members) {
        const auto source_object = std::find_if(state.objects.begin(), state.objects.end(),
            [&member](const auto& item) {
                return item.object.id.value == member.source_object_id;
            });
        const auto instance_object = std::find_if(state.objects.begin(), state.objects.end(),
            [&member](const auto& item) {
                return item.object.id.value == member.instance_object_id;
            });
        REQUIRE(source_object != state.objects.end());
        REQUIRE(instance_object != state.objects.end());
        REQUIRE(instance_object->object.parent.has_value());
        REQUIRE(instance_object->object.parent->value == placed.root_object_id);
        REQUIRE(instance_object->object.mesh_asset == member.mesh_asset_id);
        REQUIRE(instance_object->mesh_revision == member.mesh_revision);
        const auto source_asset = std::find_if(
            authoring_after_instance.value().assets.begin(),
            authoring_after_instance.value().assets.end(), [&member](const auto& item) {
                return item.id == member.asset_id;
            });
        REQUIRE(source_asset != authoring_after_instance.value().assets.end());
        REQUIRE(member.source_revision == source_asset->source_revision);
        REQUIRE(member.source_digest == source_asset->source_digest);
    }
    const auto serialized_authoring = authoring_after_instance.value().serialize();
    const auto round_trip = carto::condition::AuthoringDocument::deserialize(
        serialized_authoring);
    REQUIRE(round_trip);
    REQUIRE(round_trip.value().serialize() == serialized_authoring);

    REQUIRE(ui.dispatch(carto::application::UndoAction{}));
    state = ui.snapshot();
    REQUIRE(state.objects.size() == unchanged_objects);
    REQUIRE(state.condition.prefab_instances.empty());
    REQUIRE(state.project_revision > created.value().revision_after);
    REQUIRE(ui.dispatch(carto::application::RedoAction{}));
    state = ui.snapshot();
    REQUIRE(state.objects.size() == unchanged_objects + 3U);
    REQUIRE(state.condition.prefab_instances.size() == 1U);
    REQUIRE(state.condition.prefab_instances.front().current);

    REQUIRE(ui.dispatch(carto::application::SaveProjectAction{project_path}));
    carto::application::ApplicationSession reopened_session;
    carto::ui::UiController reopened_ui(reopened_session);
    REQUIRE(reopened_ui.dispatch(carto::application::OpenProjectAction{project_path}));
    auto reopened = reopened_ui.snapshot();
    REQUIRE(reopened.objects.size() == unchanged_objects + 3U);
    REQUIRE(reopened.condition.prefab_instances.size() == 1U);
    REQUIRE(reopened.condition.prefab_instances.front().current);
    REQUIRE(reopened_ui.build_condition_package());

    const auto source_view = std::find_if(reopened.objects.begin(), reopened.objects.end(),
        [source_room](const auto& item) { return item.object.id == source_room; });
    REQUIRE(source_view != reopened.objects.end());
    auto moved_source = source_view->world_transform;
    moved_source.translation.x += 1.0;
    REQUIRE(reopened_ui.dispatch(carto::application::SetObjectTransformAction{
        source_room, moved_source, reopened.project_revision}));
    reopened = reopened_ui.snapshot();
    REQUIRE(reopened.condition.sources.front().current);
    REQUIRE(reopened.condition.prefab_instances.size() == 1U);
    REQUIRE(!reopened.condition.prefab_instances.front().current);
    REQUIRE(reopened.condition.stale);
    REQUIRE(!reopened.condition.publication_ready);
    REQUIRE(!reopened_ui.build_condition_package());
    REQUIRE(reopened_ui.dispatch(carto::application::UndoAction{}));
    reopened = reopened_ui.snapshot();
    REQUIRE(reopened.condition.prefab_instances.front().current);
    REQUIRE(!reopened.condition.stale);
    REQUIRE(reopened_ui.build_condition_package());

    auto current_authoring = reopened_session.condition_authoring();
    REQUIRE(current_authoring);
    auto changed_prefab = current_authoring.value().prefabs.back();
    changed_prefab.name = "Motel room plus door · revised";
    REQUIRE(reopened_ui.replace_condition_authored_record(
        changed_prefab, reopened.project_revision,
        reopened.condition.authored_revision));
    reopened = reopened_ui.snapshot();
    REQUIRE(!reopened.condition.prefab_instances.front().current);
    REQUIRE(reopened.condition.stale);
    REQUIRE(!reopened_ui.build_condition_package());
    REQUIRE(reopened_ui.dispatch(carto::application::UndoAction{}));
    reopened = reopened_ui.snapshot();
    REQUIRE(reopened.condition.prefab_instances.front().current);
    REQUIRE(!reopened.condition.stale);
    REQUIRE(reopened_ui.build_condition_package());

    carto::condition::PrefabRecord unbound_prefab;
    unbound_prefab.name = "Unbound source fixture";
    unbound_prefab.asset_ids = {reopened.condition.assets[1].id};
    REQUIRE(reopened_ui.add_condition_prefab(
        unbound_prefab, reopened.project_revision,
        reopened.condition.authored_revision));
    reopened = reopened_ui.snapshot();
    const auto count_before_unbound = reopened.objects.size();
    REQUIRE(!reopened_ui.instantiate_condition_prefab(
        reopened.condition.prefabs.back().id, "Must not place", {0.0, 0.0, 0.0},
        reopened.project_revision, reopened.condition.authored_revision));
    REQUIRE(reopened_ui.snapshot().objects.size() == count_before_unbound);

    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
}

void ui_condition_authored_records_support_safe_edit_and_delete() {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto root = std::filesystem::current_path() / "build" / "test-temp" /
        ("cartographer-condition-record-edit-tests-" + std::to_string(stamp));
    std::filesystem::create_directories(root);

    carto::application::ApplicationSession session;
    carto::ui::UiController ui(session);
    REQUIRE(ui.dispatch(carto::application::NewProjectAction{
        "CONDITION record lifecycle"}));
    REQUIRE(ui.import_condition_fixture());
    const auto initial = ui.snapshot();

    carto::condition::InteractionAnchor anchor;
    anchor.asset_id = initial.condition.assets.front().id;
    anchor.kind = carto::condition::AnchorKind::interaction;
    anchor.interaction_class = "motel.room.entry";
    REQUIRE(ui.add_condition_interaction_anchor(
        anchor, initial.project_revision, initial.condition.authored_revision));
    auto state = ui.snapshot();
    const auto anchor_id = state.condition.interaction_anchors.back().id;
    auto edited_anchor = anchor;
    edited_anchor.id = anchor_id;
    edited_anchor.interaction_class = "motel.room.accessible-entry";
    edited_anchor.transform.translation = {0.25, 0.0, 0.5};
    REQUIRE(ui.replace_condition_authored_record(
        edited_anchor, state.project_revision, state.condition.authored_revision));
    auto after_anchor_edit = ui.snapshot();
    REQUIRE(after_anchor_edit.condition.interaction_anchors.back().id == anchor_id);
    REQUIRE(after_anchor_edit.condition.interaction_anchors.back().interaction_class ==
        edited_anchor.interaction_class);
    REQUIRE(after_anchor_edit.condition.interaction_anchors.back().transform.translation.x ==
        0.25);
    if (after_anchor_edit.operations.back().action !=
        "Update CONDITION Interaction Anchor") {
        std::fprintf(stderr, "CONDITION anchor edit action was: %s\n",
            after_anchor_edit.operations.back().action.c_str());
    }
    REQUIRE(after_anchor_edit.operations.back().action ==
        "Update CONDITION Interaction Anchor");
    REQUIRE(!ui.replace_condition_authored_record(
        edited_anchor, state.project_revision, state.condition.authored_revision));
    auto missing_anchor = edited_anchor;
    missing_anchor.id = std::numeric_limits<carto::condition::Id>::max();
    REQUIRE(!ui.replace_condition_authored_record(
        missing_anchor, after_anchor_edit.project_revision,
        after_anchor_edit.condition.authored_revision));
    REQUIRE(ui.snapshot().project_revision == after_anchor_edit.project_revision);

    carto::condition::PrefabRecord prefab;
    prefab.name = "Motel room entry";
    prefab.asset_ids = {initial.condition.assets.front().id};
    prefab.sockets = {"entry"};
    REQUIRE(ui.add_condition_prefab(
        prefab, after_anchor_edit.project_revision,
        after_anchor_edit.condition.authored_revision));
    auto after_prefab = ui.snapshot();
    const auto prefab_id = after_prefab.condition.prefabs.back().id;
    auto edited_prefab = prefab;
    edited_prefab.id = prefab_id;
    edited_prefab.name = "Motel room entry, revised";
    edited_prefab.sockets.push_back("threshold.center");
    REQUIRE(ui.replace_condition_authored_record(
        edited_prefab, after_prefab.project_revision,
        after_prefab.condition.authored_revision));
    auto after_prefab_edit = ui.snapshot();
    REQUIRE(after_prefab_edit.condition.prefabs.back().id == prefab_id);
    REQUIRE(after_prefab_edit.condition.prefabs.back().name == edited_prefab.name);
    REQUIRE(after_prefab_edit.condition.prefabs.back().source_revision ==
        after_prefab.condition.authored_revision);

    carto::condition::CollectionRecord collection;
    collection.name = "Motel entry props";
    collection.asset_ids = {initial.condition.assets.front().id};
    collection.semantic_class = "motel.entry";
    REQUIRE(ui.add_condition_collection(
        collection, after_prefab_edit.project_revision,
        after_prefab_edit.condition.authored_revision));
    auto after_collection = ui.snapshot();
    const auto collection_id = after_collection.condition.collections.back().id;
    carto::condition::CollectionRecord parent_collection;
    parent_collection.name = "Motel room kit collection";
    parent_collection.child_collection_ids = {collection_id};
    parent_collection.semantic_class = "motel.room";
    REQUIRE(ui.add_condition_collection(
        parent_collection, after_collection.project_revision,
        after_collection.condition.authored_revision));
    auto after_parent_collection = ui.snapshot();
    const auto parent_collection_id =
        after_parent_collection.condition.collections.back().id;
    auto edited_collection = collection;
    edited_collection.id = collection_id;
    edited_collection.semantic_class = "motel.entry.accessibility-reviewed";
    REQUIRE(ui.replace_condition_authored_record(
        edited_collection, after_parent_collection.project_revision,
        after_parent_collection.condition.authored_revision));
    auto after_collection_edit = ui.snapshot();
    const auto collection_projection = std::find_if(
        after_collection_edit.condition.collections.begin(),
        after_collection_edit.condition.collections.end(),
        [collection_id](const auto& value) { return value.id == collection_id; });
    REQUIRE(collection_projection != after_collection_edit.condition.collections.end());
    REQUIRE(collection_projection->semantic_class == edited_collection.semantic_class);
    REQUIRE(!ui.delete_condition_authored_record(
        carto::application::ConditionAuthoredRecordKind::collection,
        collection_id, after_collection_edit.project_revision,
        after_collection_edit.condition.authored_revision));
    REQUIRE(ui.snapshot().project_revision == after_collection_edit.project_revision);

    carto::condition::RegionKitManifest kit;
    kit.name = "Motel room entry kit";
    kit.asset_ids = {initial.condition.assets.front().id};
    kit.prefab_ids = {prefab_id};
    kit.interaction_coverage = 0.5;
    REQUIRE(ui.add_condition_region_kit(
        kit, after_collection_edit.project_revision,
        after_collection_edit.condition.authored_revision));
    auto after_kit = ui.snapshot();
    const auto kit_id = after_kit.condition.region_kits.back().id;
    auto edited_kit = kit;
    edited_kit.id = kit_id;
    edited_kit.name = "Motel room entry kit reviewed";
    edited_kit.interaction_coverage = 0.9;
    REQUIRE(ui.replace_condition_authored_record(
        edited_kit, after_kit.project_revision,
        after_kit.condition.authored_revision));
    auto after_kit_edit = ui.snapshot();
    REQUIRE(after_kit_edit.condition.region_kits.back().id == kit_id);
    REQUIRE(after_kit_edit.condition.region_kits.back().name == edited_kit.name);
    REQUIRE(after_kit_edit.condition.region_kits.back().interaction_coverage == 0.9);
    REQUIRE(!ui.delete_condition_authored_record(
        carto::application::ConditionAuthoredRecordKind::prefab,
        prefab_id, after_kit_edit.project_revision,
        after_kit_edit.condition.authored_revision));
    REQUIRE(ui.snapshot().project_revision == after_kit_edit.project_revision);

    REQUIRE(ui.delete_condition_authored_record(
        carto::application::ConditionAuthoredRecordKind::region_kit,
        kit_id, after_kit_edit.project_revision,
        after_kit_edit.condition.authored_revision));
    auto after_kit_delete = ui.snapshot();
    REQUIRE(std::none_of(after_kit_delete.condition.region_kits.begin(),
        after_kit_delete.condition.region_kits.end(),
        [kit_id](const auto& value) { return value.id == kit_id; }));
    REQUIRE(after_kit_delete.operations.back().action ==
        "Delete CONDITION Region Kit");
    REQUIRE(ui.dispatch(carto::application::UndoAction{}));
    const auto after_kit_undo = ui.snapshot();
    REQUIRE(std::any_of(after_kit_undo.condition.region_kits.begin(),
        after_kit_undo.condition.region_kits.end(),
        [kit_id](const auto& value) { return value.id == kit_id; }));
    REQUIRE(ui.dispatch(carto::application::RedoAction{}));
    const auto after_kit_redo = ui.snapshot();
    REQUIRE(std::none_of(after_kit_redo.condition.region_kits.begin(),
        after_kit_redo.condition.region_kits.end(),
        [kit_id](const auto& value) { return value.id == kit_id; }));

    auto current = ui.snapshot();
    REQUIRE(ui.delete_condition_authored_record(
        carto::application::ConditionAuthoredRecordKind::collection,
        parent_collection_id, current.project_revision,
        current.condition.authored_revision));
    current = ui.snapshot();
    REQUIRE(ui.delete_condition_authored_record(
        carto::application::ConditionAuthoredRecordKind::collection,
        collection_id, current.project_revision,
        current.condition.authored_revision));
    current = ui.snapshot();
    REQUIRE(ui.delete_condition_authored_record(
        carto::application::ConditionAuthoredRecordKind::prefab,
        prefab_id, current.project_revision, current.condition.authored_revision));
    current = ui.snapshot();
    REQUIRE(ui.delete_condition_authored_record(
        carto::application::ConditionAuthoredRecordKind::interaction_anchor,
        anchor_id, current.project_revision, current.condition.authored_revision));
    current = ui.snapshot();
    REQUIRE(ui.add_condition_interaction_anchor(
        anchor, current.project_revision, current.condition.authored_revision));
    const auto new_anchor_id = ui.snapshot().condition.interaction_anchors.back().id;
    REQUIRE(new_anchor_id > anchor_id);
    const auto project_path = root / "edited-condition-records.carto";
    REQUIRE(ui.dispatch(carto::application::SaveProjectAction{project_path}));
    carto::application::ApplicationSession reopened;
    REQUIRE(carto::application::HumanApplicationAccess::dispatch(
        reopened, carto::application::OpenProjectAction{project_path}));
    const auto reopened_snapshot = reopened.snapshot();
    REQUIRE(std::none_of(reopened_snapshot.condition.region_kits.begin(),
        reopened_snapshot.condition.region_kits.end(),
        [kit_id](const auto& value) { return value.id == kit_id; }));
    REQUIRE(std::none_of(reopened_snapshot.condition.prefabs.begin(),
        reopened_snapshot.condition.prefabs.end(),
        [prefab_id](const auto& value) { return value.id == prefab_id; }));
    REQUIRE(std::any_of(reopened_snapshot.condition.interaction_anchors.begin(),
        reopened_snapshot.condition.interaction_anchors.end(),
        [new_anchor_id](const auto& value) { return value.id == new_anchor_id; }));

    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
}

void application_persists_and_undoes_condition_authoring() {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto root = std::filesystem::current_path() / "build" / "test-temp" /
        ("cartographer-condition-tests-" + std::to_string(stamp));
    std::filesystem::create_directories(root);

    carto::application::ApplicationSession session;
    REQUIRE(carto::application::HumanApplicationAccess::dispatch(
        session, carto::application::NewProjectAction{"CONDITION Source Authoring"}));
    const auto imported = carto::application::HumanApplicationAccess::dispatch(
        session, carto::application::ImportConditionFixtureAction{});
    REQUIRE(imported);
    REQUIRE(session.snapshot().condition.present);
    REQUIRE(session.snapshot().condition.publication_ready);
    REQUIRE(session.snapshot().condition.consumer_admissible);
    REQUIRE(session.snapshot().condition.asset_count == 3U);

    const auto project_path = root / "condition.carto";
    const auto save_project = carto::application::HumanApplicationAccess::dispatch(
        session, carto::application::SaveProjectAction{project_path});
    if (!save_project) throw TestFailure("save CONDITION authoring project: " + save_project.error().message);
    carto::application::ApplicationSession reopened;
    REQUIRE(carto::application::HumanApplicationAccess::dispatch(
        reopened, carto::application::OpenProjectAction{project_path}));
    REQUIRE(reopened.snapshot().condition.digest == session.snapshot().condition.digest);

    REQUIRE(carto::application::HumanApplicationAccess::dispatch(
        reopened,
        carto::application::InvalidateConditionSourceAction{
            1U, "fixture-2", digest("condition-source-replacement")}));
    REQUIRE(reopened.snapshot().condition.stale);
    REQUIRE(reopened.snapshot().condition.authored_revision == carto::core::Revision(1U));
    REQUIRE(!reopened.snapshot().condition.publication_ready);
    REQUIRE(carto::application::HumanApplicationAccess::dispatch(
        reopened, carto::application::UndoAction{}));
    REQUIRE(reopened.snapshot().condition.authored_revision == carto::core::Revision(0U));
    REQUIRE(reopened.snapshot().condition.publication_ready);
    REQUIRE(carto::application::HumanApplicationAccess::dispatch(
        reopened, carto::application::RedoAction{}));
    REQUIRE(reopened.snapshot().condition.authored_revision == carto::core::Revision(1U));
    REQUIRE(!reopened.snapshot().condition.publication_ready);

    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
}

void ui_publishes_only_admitted_source_packages() {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto root = std::filesystem::current_path() / "build" / "test-temp" /
        ("cartographer-condition-ui-tests-" + std::to_string(stamp));
    std::filesystem::create_directories(root);

    carto::application::ApplicationSession session;
    carto::ui::UiController ui(session);
    REQUIRE(ui.dispatch(carto::application::NewProjectAction{"CONDITION UI"}));
    REQUIRE(ui.set_workspace(carto::ui::Workspace::condition));
    REQUIRE(ui.import_condition_fixture());
    REQUIRE(ui.snapshot().workspace_kind == carto::ui::Workspace::condition);
    REQUIRE(ui.snapshot().condition.consumer_admissible);
    const auto package = ui.build_condition_package();
    REQUIRE(package);
    const auto path = root / "condition.carto-condition";
    REQUIRE(ui.publish_condition_package(path));
    std::ifstream stream(path, std::ios::binary);
    REQUIRE(stream.good());
    const std::string bytes((std::istreambuf_iterator<char>(stream)), {});
    REQUIRE(carto::condition::Package::deserialize(bytes));

    REQUIRE(ui.invalidate_condition_source(1U, "fixture-rev-2", digest("condition-rev-2")));
    REQUIRE(ui.snapshot().condition.stale);
    REQUIRE(!ui.snapshot().condition.publication_ready);
    REQUIRE(!ui.build_condition_package());
    REQUIRE(!ui.publish_condition_package(path));
    std::ifstream preserved_stream(path, std::ios::binary);
    REQUIRE(preserved_stream.good());
    const std::string preserved_bytes((std::istreambuf_iterator<char>(preserved_stream)), {});
    REQUIRE(preserved_bytes == bytes);

    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
}

void ui_imports_exports_and_reconciles_condition_authoring_files() {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto root = std::filesystem::current_path() / "build" / "test-temp" /
        ("cartographer-condition-authoring-io-" + std::to_string(stamp));
    std::filesystem::create_directories(root);

    auto fixture = carto::condition::make_condition_fixture();
    REQUIRE(fixture);
    const auto source_path = root / "source.carto-condition-authoring";
    {
        std::ofstream stream(source_path, std::ios::binary);
        REQUIRE(stream.good());
        stream << fixture.value().serialize();
        REQUIRE(stream.good());
    }

    carto::application::ApplicationSession session;
    carto::ui::UiController ui(session);
    REQUIRE(ui.dispatch(carto::application::NewProjectAction{"CONDITION file import"}));
    const auto imported = ui.import_condition_authoring(source_path);
    REQUIRE(imported);
    REQUIRE(imported.value().document_changed);
    REQUIRE(ui.snapshot().condition.digest == fixture.value().canonical_digest().value());
    REQUIRE(ui.snapshot().condition.sources.size() == fixture.value().sources.size());
    REQUIRE(ui.snapshot().operations.back().category == carto::ui::OperationCategory::provider);
    REQUIRE(ui.snapshot().undo_count > 0U);

    REQUIRE(ui.dispatch(carto::application::UndoAction{}));
    REQUIRE(!ui.snapshot().condition.present);
    REQUIRE(ui.dispatch(carto::application::RedoAction{}));
    REQUIRE(ui.snapshot().condition.present);

    const auto exported_path = root / "exported.carto-condition-authoring";
    REQUIRE(ui.export_condition_authoring(exported_path));
    std::ifstream exported_stream(exported_path, std::ios::binary);
    REQUIRE(exported_stream.good());
    const std::string exported_bytes((std::istreambuf_iterator<char>(exported_stream)), {});
    REQUIRE(carto::condition::AuthoringDocument::deserialize(exported_bytes));
    REQUIRE(exported_bytes == fixture.value().serialize());

    const auto invalid_path = root / "invalid.carto-condition-authoring";
    {
        std::ofstream stream(invalid_path, std::ios::binary);
        REQUIRE(stream.good());
        stream << "not a CONDITION authoring document";
    }
    const auto revision_before_invalid_import = ui.snapshot().project_revision;
    const auto digest_before_invalid_import = ui.snapshot().condition.digest;
    REQUIRE(!ui.import_condition_authoring(invalid_path));
    REQUIRE(ui.snapshot().project_revision == revision_before_invalid_import);
    REQUIRE(ui.snapshot().condition.digest == digest_before_invalid_import);
    REQUIRE(!ui.submit_ai_proposal(carto::application::ImportConditionAuthoringAction{
        fixture.value().serialize()}));
    REQUIRE(ui.snapshot().project_revision == revision_before_invalid_import);

    const auto oversized_path = root / "oversized.carto-condition-authoring";
    {
        std::ofstream stream(oversized_path, std::ios::binary);
        REQUIRE(stream.good());
    }
    std::filesystem::resize_file(
        oversized_path, carto::condition::AuthoringDocument::kMaxSerializedBytes + 1U);
    REQUIRE(!ui.import_condition_authoring(oversized_path));
    REQUIRE(ui.snapshot().project_revision == revision_before_invalid_import);

    const auto source = ui.snapshot().condition.sources.front();
    REQUIRE(ui.invalidate_condition_source(
        source.id, "fixture-revision-2", digest("condition-revision-2-bytes")));
    REQUIRE(ui.snapshot().condition.stale);
    REQUIRE(!ui.snapshot().condition.sources.front().current);
    REQUIRE(!ui.snapshot().condition.publication_ready);
    REQUIRE(!ui.publish_condition_package(root / "blocked.carto-condition"));

    const auto stale_project_revision = ui.snapshot().project_revision;
    const auto stale_authoring_digest = ui.snapshot().condition.digest;
    REQUIRE(!ui.import_condition_authoring(source_path));
    REQUIRE(ui.snapshot().project_revision == stale_project_revision);
    REQUIRE(ui.snapshot().condition.digest == stale_authoring_digest);

    auto reconciled = fixture.value();
    reconciled.project_revision = ui.snapshot().condition.authored_revision.next();
    {
        std::ofstream stream(source_path, std::ios::binary | std::ios::trunc);
        REQUIRE(stream.good());
        stream << reconciled.serialize();
        REQUIRE(stream.good());
    }
    REQUIRE(ui.import_condition_authoring(source_path));
    REQUIRE(!ui.snapshot().condition.stale);
    REQUIRE(ui.snapshot().condition.sources.front().current);
    REQUIRE(ui.snapshot().condition.consumer_admissible);

    auto same_revision_change = reconciled;
    same_revision_change.safe_places.front().privacy -= 0.1;
    {
        std::ofstream stream(source_path, std::ios::binary | std::ios::trunc);
        REQUIRE(stream.good());
        stream << same_revision_change.serialize();
        REQUIRE(stream.good());
    }
    const auto current_project_revision = ui.snapshot().project_revision;
    const auto current_authoring_digest = ui.snapshot().condition.digest;
    REQUIRE(!ui.import_condition_authoring(source_path));
    REQUIRE(ui.snapshot().project_revision == current_project_revision);
    REQUIRE(ui.snapshot().condition.digest == current_authoring_digest);

    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
}

} // namespace

int main() {
    try {
        fixture_round_trips_and_publishes();
        physical_boundary_rejects_presentation_mutation();
        invalidation_preserves_source_identity_and_blocks_publication();
        invalidation_refuses_revision_exhaustion_without_partial_change();
        authored_audio_sources_and_acoustic_zones_round_trip_and_fail_closed();
        wave_clip_inspection_is_bounded_and_fail_closed();
        general_store_audio_fixture_covers_native_source_contract();
        condition_authoring_v1_reopens_with_empty_audio_sections();
        condition_authoring_v2_reopens_with_empty_effect_sections();
        condition_authoring_v3_reopens_with_empty_vegetation_sections();
        condition_authoring_v4_reopens_with_empty_state_branches();
        condition_authoring_v8_reopens_with_empty_prefab_instances();
        authored_state_branches_compare_without_overwriting_source_and_reject_conflicts();
        vegetation_scatter_round_trips_and_derives_deterministic_read_only_preview();
        vegetation_scatter_preview_rejects_stale_missing_wrong_semantic_and_unbounded_inputs();
        vegetation_scatter_binds_generic_plant_source_with_revision_and_digest();
        effect_source_emitter_field_binding_round_trip_and_adversarial_validation();
        condition_kinematic_assembly_round_trips_and_rejects_stale_or_unbounded_sources();
        effect_preview_is_deterministic_state_bound_and_read_only();
        effect_preview_rejects_stale_revision_and_out_of_coverage_field();
        ui_acoustic_geometry_edits_are_revision_bound_and_undoable();
        ui_condition_asset_binding_is_revision_checked_and_undoable();
        ui_condition_anchors_and_prefabs_are_revision_bound_and_reversible();
        ui_condition_prefab_instances_are_atomic_revision_bound_and_fail_closed();
        ui_condition_authored_records_support_safe_edit_and_delete();
        ui_effect_preview_uses_project_fields_and_is_non_mutating_across_history();
        condition_rural_road_forest_workflow_is_revision_bound_and_portable();
        ui_vegetation_recipe_commits_reopens_undoes_and_rejects_stale_sources();
        condition_gas_price_board_preserves_sign_source_and_material_lineage();
        ui_safe_place_branch_is_journaled_comparable_and_recoverable();
        application_persists_and_undoes_condition_authoring();
        ui_audio_authoring_uses_project_transaction_and_undo_history();
        ui_wave_clip_import_is_review_gated_pathless_and_reversible();
        ui_publishes_only_admitted_source_packages();
        ui_imports_exports_and_reconciles_condition_authoring_files();
        return 0;
    } catch (const std::exception& error) {
        return (static_cast<void>(std::fprintf(stderr, "%s\n", error.what())), 1);
    }
}
