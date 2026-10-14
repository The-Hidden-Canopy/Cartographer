#include <carto/application/application.hpp>
#include <carto/condition/authoring.hpp>
#include <carto/condition/effect_preview.hpp>
#include <carto/condition/vegetation_preview.hpp>
#include <carto/condition/state_branch.hpp>
#include <carto/condition/wave_import.hpp>
#include <carto/ui/ui.hpp>

#include <array>
#include <algorithm>
#include <bit>
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
    const auto header_end = legacy.find('\n');
    REQUIRE(header_end != std::string::npos);
    legacy.replace(0U, header_end, "CARTOGRAPHER_CONDITION_AUTHORING 4");
    const auto migrated = carto::condition::AuthoringDocument::deserialize(legacy);
    REQUIRE(migrated);
    REQUIRE(migrated.value().state_branches.empty());
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

void ui_vegetation_recipe_commits_reopens_undoes_and_rejects_stale_fields() {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto root = std::filesystem::current_path() / "build" / "test-temp" /
        ("cartographer-condition-vegetation-ui-" + std::to_string(stamp));
    std::filesystem::create_directories(root);

    carto::application::ApplicationSession session;
    carto::ui::UiController ui(session);
    REQUIRE(ui.dispatch(carto::application::NewProjectAction{"CONDITION forest placement"}));
    REQUIRE(ui.import_condition_fixture());
    auto authoring = carto::condition::make_condition_fixture();
    REQUIRE(authoring);
    auto fields = make_forest_fields(authoring.value());
    auto recipe = authoring.value().vegetation_scatters.front();
    authoring.value().vegetation_scatters.clear();
    for (const auto& [identity, field] : fields) {
        REQUIRE(ui.dispatch(carto::application::CreateSpatialFieldAction{
            identity, field.semantic(), field.origin_uv(), field.cell_size(), field.width(),
            field.height(), field.minimum_value(), field.maximum_value(),
            field.values().front()}));
    }
    authoring.value().project_revision = ui.snapshot().condition.authored_revision.next();
    const auto source_path = root / "forest-authoring.carto-condition-authoring";
    {
        std::ofstream stream(source_path, std::ios::binary);
        REQUIRE(stream.good());
        stream << authoring.value().serialize();
        REQUIRE(stream.good());
    }
    REQUIRE(ui.import_condition_authoring(source_path));
    REQUIRE(ui.snapshot().condition.assets.front().asset_class == "vegetation");
    const auto project_revision_before_preview = ui.snapshot().project_revision;
    REQUIRE(ui.add_condition_vegetation_scatter(recipe));
    REQUIRE(ui.snapshot().condition.vegetation_scatter_count == 1U);
    REQUIRE(ui.snapshot().condition.vegetation_scatter_ids.front() == recipe.id);
    REQUIRE(ui.snapshot().operations.back().category == carto::ui::OperationCategory::provider);
    const auto preview = ui.preview_condition_vegetation(recipe.id);
    REQUIRE(preview);
    REQUIRE(preview.value().instances.size() == 16U);
    REQUIRE(ui.snapshot().project_revision > project_revision_before_preview);
    const auto revision_after_authoring = ui.snapshot().project_revision;
    REQUIRE(ui.preview_condition_vegetation(recipe.id));
    REQUIRE(ui.snapshot().project_revision == revision_after_authoring);

    const auto project_path = root / "forest-placement.carto";
    REQUIRE(ui.dispatch(carto::application::SaveProjectAction{project_path}));
    carto::application::ApplicationSession reopened_session;
    carto::ui::UiController reopened_ui(reopened_session);
    REQUIRE(reopened_ui.dispatch(carto::application::OpenProjectAction{project_path}));
    REQUIRE(reopened_ui.snapshot().condition.vegetation_scatter_count == 1U);
    const auto reopened_preview = reopened_ui.preview_condition_vegetation(recipe.id);
    REQUIRE(reopened_preview);
    REQUIRE(reopened_preview.value().preview_digest == preview.value().preview_digest);

    REQUIRE(ui.dispatch(carto::application::UndoAction{}));
    REQUIRE(ui.snapshot().condition.vegetation_scatter_count == 0U);
    REQUIRE(!ui.preview_condition_vegetation(recipe.id));
    REQUIRE(ui.dispatch(carto::application::RedoAction{}));
    REQUIRE(ui.snapshot().condition.vegetation_scatter_count == 1U);
    REQUIRE(ui.preview_condition_vegetation(recipe.id));

    const auto moisture = fields.at("forest.moisture");
    REQUIRE(ui.dispatch(carto::application::PaintSpatialFieldAction{
        "forest.moisture",
        carto::spatial_field::FieldBrush{moisture.source_revision(),
            {0.0, 0.0}, 0.5, 0.5, 1.0,
            carto::spatial_field::BrushMode::replace}}));
    REQUIRE(!ui.preview_condition_vegetation(recipe.id));

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
    REQUIRE(ui.snapshot().operations.back().category == carto::ui::OperationCategory::provider);
    const auto comparison = ui.compare_condition_state_branches(0U, 1U);
    REQUIRE(comparison);
    REQUIRE(comparison.value().differences.size() == 1U);
    REQUIRE(comparison.value().differences.front().right_value.numeric_value == 0.55);
    REQUIRE(ui.snapshot().project_revision > project_revision_before);
    const auto revision_after_add = ui.snapshot().project_revision;
    REQUIRE(ui.compare_condition_state_branches(0U, 1U));
    REQUIRE(ui.snapshot().project_revision == revision_after_add);

    const auto path = root / "safe-place-states.carto";
    REQUIRE(ui.dispatch(carto::application::SaveProjectAction{path}));
    carto::application::ApplicationSession reopened_session;
    carto::ui::UiController reopened_ui(reopened_session);
    REQUIRE(reopened_ui.dispatch(carto::application::OpenProjectAction{path}));
    REQUIRE(reopened_ui.snapshot().condition.state_branch_count == 1U);
    const auto reopened_comparison = reopened_ui.compare_condition_state_branches(0U, 1U);
    REQUIRE(reopened_comparison);
    REQUIRE(reopened_comparison.value().comparison_digest ==
        comparison.value().comparison_digest);

    REQUIRE(ui.dispatch(carto::application::UndoAction{}));
    REQUIRE(ui.snapshot().condition.state_branch_count == 0U);
    REQUIRE(!ui.compare_condition_state_branches(0U, 1U));
    REQUIRE(ui.dispatch(carto::application::RedoAction{}));
    REQUIRE(ui.snapshot().condition.state_branch_count == 1U);

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
        authored_state_branches_compare_without_overwriting_source_and_reject_conflicts();
        vegetation_scatter_round_trips_and_derives_deterministic_read_only_preview();
        vegetation_scatter_preview_rejects_stale_missing_wrong_semantic_and_unbounded_inputs();
        effect_source_emitter_field_binding_round_trip_and_adversarial_validation();
        effect_preview_is_deterministic_state_bound_and_read_only();
        effect_preview_rejects_stale_revision_and_out_of_coverage_field();
        ui_acoustic_geometry_edits_are_revision_bound_and_undoable();
        ui_effect_preview_uses_project_fields_and_is_non_mutating_across_history();
        ui_vegetation_recipe_commits_reopens_undoes_and_rejects_stale_fields();
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
