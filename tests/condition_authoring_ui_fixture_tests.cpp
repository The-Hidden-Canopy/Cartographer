#include <carto/application/application.hpp>
#include <carto/assets/blob_store.hpp>
#include <carto/condition/authoring.hpp>
#include <carto/condition/wave_import.hpp>
#include <carto/project/project.hpp>
#include <carto/ui/ui.hpp>

#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
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

void run(const std::filesystem::path& output_root) {
    std::filesystem::create_directories(output_root);
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto fixture_root = output_root / ("run-" + std::to_string(stamp));
    std::filesystem::create_directories(fixture_root);
    const auto wave_path = fixture_root / "synthetic-acoustic-tone.wav";
    const auto project_path = fixture_root / "condition-acoustic-interaction.carto";

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
    authoring.project_namespace = "condition.fixture.acoustic-interaction";
    authoring.project_revision = carto::core::Revision{2U};
    require(!authoring.assets.empty(), "base CONDITION fixture has no owner asset");
    // Start unbound so this fixture exercises the native revision-bound bind command.
    authoring.assets.front().authored_object_id = 0U;

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
        true,
        true,
        {"WAVE inspected: PCM integer, 1 channel, 48000 Hz, 16 bit, 48000 frames"}});
    authoring.audio_sources.push_back(carto::condition::AuthoredAudioSource{
        1U, 1U, 2U, carto::core::Transform::identity(), 0.5, 1.0,
        true, true, 0.75, 12.0, 50U,
        carto::condition::AudioBusClass::ambient,
        carto::condition::AudioOcclusionIntent::none, 0.15,
        "fixture-active", "qa.synthetic-tone", true});
    authoring.acoustic_zones.push_back(carto::condition::AuthoredAcousticZone{
        1U, 1U, carto::condition::AcousticZoneShape::box,
        carto::core::Transform::identity(), {2.0, 1.5, 2.5}, 0.0,
        "synthetic-qa-room", 0.8, "qa-room", 50U,
        "cartographer.qa.acoustic-room", true});
    require(static_cast<bool>(authoring.validate()),
        "generated CONDITION acoustic authoring is invalid");

    carto::application::ApplicationSession session;
    carto::ui::UiController ui(session);
    require(static_cast<bool>(ui.dispatch(carto::application::NewProjectAction{
                    "CONDITION acoustic interaction QA"})),
        "could not create the temporary acoustic QA project");
    require(static_cast<bool>(ui.dispatch(carto::application::CreateBoxAction{
                    "Synthetic acoustic QA object", {2.0, 2.0, 2.0}})),
        "could not create the owner scene object for the acoustic QA fixture");
    require(static_cast<bool>(ui.dispatch(carto::application::ImportConditionAuthoringAction{
                    authoring.serialize()})),
        "could not bind the generated CONDITION authoring fixture");
    const auto before_binding = ui.snapshot();
    require(!before_binding.objects.empty(),
        "generated acoustic QA project has no scene object to bind");
    const auto asset_id = authoring.assets.front().id;
    const auto owner_object_id = before_binding.objects.front().object.id;
    const auto binding = ui.bind_condition_asset_to_object(
                asset_id, owner_object_id,
                before_binding.project_revision,
                before_binding.condition.authored_revision);
    require(binding.has_value() && binding.value().document_changed,
        "native UI command could not bind the acoustic asset to its scene object");
    const auto after_binding = ui.snapshot();
    require(after_binding.project_revision == before_binding.project_revision.next() &&
            after_binding.condition.authored_revision ==
                before_binding.condition.authored_revision.next(),
        "native UI binding did not advance project and CONDITION revisions once");
    require(static_cast<bool>(ui.dispatch(
                carto::application::SaveProjectAction{project_path})),
        "could not save the temporary acoustic QA project");

    const auto reopened = carto::project::ProjectDocument::load(project_path);
    require(reopened.has_value(), "generated acoustic QA project does not reopen");
    require(reopened.value().scene().objects_sorted().size() == 1U,
        "generated acoustic QA project does not contain exactly one owner object");
    require(reopened.value().condition_authoring().has_value(),
        "generated acoustic QA project is missing CONDITION authoring");
    const auto reopened_authoring = carto::condition::AuthoringDocument::deserialize(
        reopened.value().condition_authoring()->bytes);
    require(reopened_authoring.has_value(), "reopened acoustic QA authoring is invalid");
    require(reopened_authoring.value().audio_sources.size() == 1U &&
            reopened_authoring.value().acoustic_zones.size() == 1U &&
            reopened_authoring.value().assets.front().authored_object_id ==
                owner_object_id.value,
        "reopened acoustic QA authoring lost its scene bindings");

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
        "generated acoustic QA package failed consumer admission");

    std::cout << "fixture_project=" << project_path.string() << '\n'
              << "fixture_wave=" << wave_path.string() << '\n'
              << "wave_digest=" << wave_digest.hex() << '\n'
              << "scene_object=1 audio_source=1 acoustic_zone=1"
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
