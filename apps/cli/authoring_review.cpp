#include <carto/assets/hdr_environment.hpp>
#include <carto/d3d12/authoring_viewport.hpp>
#include <carto/project/project.hpp>
#include <carto/project/transaction.hpp>

#include <bit>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {
template <typename T> T take(carto::core::Result<T> result) {
    if (!result) throw std::runtime_error(result.error().message);
    return std::move(result).value();
}
void okay(carto::core::Result<void> result) {
    if (!result) throw std::runtime_error(result.error().message);
}
template <typename T> T number(std::string_view input) {
    T value{};
    const auto parsed = std::from_chars(input.data(), input.data() + input.size(), value);
    if (parsed.ec != std::errc{} || parsed.ptr != input.data() + input.size())
        throw std::runtime_error("invalid numeric review argument");
    return value;
}
std::vector<std::uint8_t> read_bounded(const std::filesystem::path& path, std::size_t maximum) {
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    const auto length = input ? input.tellg() : std::streampos(-1);
    if (length <= 0 || length > static_cast<std::streamoff>(maximum))
        throw std::runtime_error("review source is missing, empty or exceeds its byte limit");
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(length));
    input.seekg(0);
    input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!input) throw std::runtime_error("review source read failed");
    return bytes;
}
std::ofstream output_new(const std::filesystem::path& path) {
    std::ofstream output(path, std::ios::binary | std::ios::out | std::ios::noreplace);
    if (!output) throw std::runtime_error("review artifact already exists or cannot be created");
    return output;
}
void write_image(const std::filesystem::path& root,
                 const carto::production::AuthoringViewportImage& image, std::uint32_t frame_index) {
    const auto pixels = static_cast<std::size_t>(image.width) * image.height;
    if (image.display_rgba8.size() != pixels * 4U || image.linear_hdr.size() != pixels)
        throw std::runtime_error("authoring review returned incomplete image attachments");
    std::ostringstream name;
    name << "frame";
    if (frame_index != 0U) name << '-' << std::setw(4) << std::setfill('0') << frame_index;
    auto ppm = output_new(root / (name.str() + ".ppm"));
    ppm << "P6\n" << image.width << ' ' << image.height << "\n255\n";
    std::vector<char> row(static_cast<std::size_t>(image.width) * 3U);
    for (std::uint32_t y = 0; y < image.height; ++y) {
        for (std::uint32_t x = 0; x < image.width; ++x) {
            const auto source = (static_cast<std::size_t>(y) * image.width + x) * 4U;
            for (unsigned c = 0; c < 3U; ++c) row[x * 3U + c] = static_cast<char>(image.display_rgba8[source + c]);
        }
        ppm.write(row.data(), static_cast<std::streamsize>(row.size()));
    }
    ppm.close();
    if (!ppm) throw std::runtime_error("authoring review display write failed");
    if (frame_index != 0U) return;
    auto pfm = output_new(root / "frame.pfm");
    pfm << "PF\n" << image.width << ' ' << image.height << "\n-1.0\n";
    std::vector<char> linear_row(static_cast<std::size_t>(image.width) * 12U);
    for (std::uint32_t y = image.height; y-- > 0U;) {
        for (std::uint32_t x = 0; x < image.width; ++x) {
            const auto value = image.linear_hdr[static_cast<std::size_t>(y) * image.width + x];
            const double channels[]{value.x, value.y, value.z};
            for (unsigned c = 0; c < 3U; ++c) {
                if (!std::isfinite(channels[c]) || std::abs(channels[c]) > std::numeric_limits<float>::max())
                    throw std::runtime_error("authoring review HDR value is not representable");
                const auto bits = std::bit_cast<std::uint32_t>(static_cast<float>(channels[c]));
                for (unsigned byte = 0; byte < 4U; ++byte)
                    linear_row[x * 12U + c * 4U + byte] = static_cast<char>((bits >> (byte * 8U)) & 255U);
            }
        }
        pfm.write(linear_row.data(), static_cast<std::streamsize>(linear_row.size()));
    }
    pfm.close();
    if (!pfm) throw std::runtime_error("authoring review HDR write failed");
}
} // namespace

int main(int argc, char** argv) {
    using namespace carto;
    try {
        const bool high_detail_hdr = argc > 1 && std::string_view(argv[argc - 1]) == "--hdr-high";
        if (high_detail_hdr) --argc;
        if (argc != 13 && argc != 15 && argc != 16 && argc != 17) throw std::runtime_error(
            "usage: cartographer_authoring_review PROJECT HDR NEW_DIRECTORY WIDTH HEIGHT "
            "TARGET_X TARGET_Y TARGET_Z DISTANCE YAW PITCH VERTICAL_FOV [FRAMES off|on [environment|scene [DISPLACED_OBJECT_ID]]] [--hdr-high]");
        const std::filesystem::path project_path = argv[1], hdr_path = argv[2], root = argv[3];
        production::AuthoringViewportRequest request;
        request.width = number<std::uint32_t>(argv[4]);
        request.height = number<std::uint32_t>(argv[5]);
        const core::Vec3d target{number<double>(argv[6]), number<double>(argv[7]), number<double>(argv[8])};
        const double distance = number<double>(argv[9]), yaw = number<double>(argv[10]),
                     pitch = number<double>(argv[11]), fov = number<double>(argv[12]);
        const std::uint32_t frames = argc >= 15 ? number<std::uint32_t>(argv[13]) : 1U;
        const std::string_view temporal = argc >= 15 ? argv[14] : "off";
        const std::string_view refraction = argc >= 16 ? argv[15] : "environment";
        const scene::ObjectId displaced_object{argc == 17 ? number<std::uint64_t>(argv[16]) : 0U};
        const std::uint64_t pixels = std::uint64_t(request.width) * request.height;
        if (!production::valid_authoring_native_extent(request.width, request.height) || !target.finite() ||
            !std::isfinite(distance) || distance <= 0.01 || !std::isfinite(yaw) || !std::isfinite(pitch) ||
            std::abs(yaw) > 1e6 || !std::isfinite(fov) || fov <= 0.0 || fov >= 3.141592653589793 ||
            std::abs(pitch) >= 1.5707963267948966 || frames == 0U || frames > 120U ||
            (temporal != "off" && temporal != "on") || (refraction != "environment" && refraction != "scene") ||
            pixels * (3ULL * frames + 12ULL) + 8192ULL > 1024ULL * 1024ULL * 1024ULL)
            throw std::runtime_error("review extent or perspective camera is outside its bounds");
        const auto source_bytes = read_bounded(project_path, 128U * 1024U * 1024U);
        const auto hdr_bytes = read_bounded(hdr_path, assets::kMaxHdrEnvironmentSourceBytes);
        const auto source_digest = assets::sha256(source_bytes);
        const auto decoded = high_detail_hdr
            ? take(assets::decode_hdr_environment_with_quality(hdr_bytes, assets::HdrLightingQuality::high))
            : take(assets::decode_hdr_environment(hdr_bytes));
        auto document = take(project::ProjectDocument::deserialize(std::string_view(
            reinterpret_cast<const char*>(source_bytes.data()), source_bytes.size())));
        if (argc == 17) {
            const auto* object = document.scene().find(displaced_object);
            if (!displaced_object || !object || !object->mesh_asset || !object->visible)
                throw std::runtime_error("diagnostic displaced object must be a visible mesh object");
            auto transform = take(document.scene().world_transform(displaced_object));
            transform.translation.x += 10000.0;
            if (!transform.finite()) throw std::runtime_error("diagnostic object transform is invalid");
            request.transform_overrides[displaced_object] = transform;
        }
        project::AuthoredEnvironment environment;
        environment.identity = "hdr." + decoded.source_digest.hex();
        environment.width = decoded.width;
        environment.height = decoded.height;
        environment.linear_radiance = decoded.linear_radiance;
        project::PresentationCatalog review_presentation;
        okay(review_presentation.set_environment(std::move(environment)));
        std::error_code error;
        if (!std::filesystem::create_directory(root, error) || error)
            throw std::runtime_error("review output directory must be new and writable");
        journal::Journal journal(root / "review.journal");
        if (document.revision() != core::Revision{}) {
            const auto checkpoint = document.serialize();
            static_cast<void>(take(journal.append({{}, document.revision(), "authoring.review.checkpoint",
                std::span<const std::uint8_t>{reinterpret_cast<const std::uint8_t*>(checkpoint.data()), checkpoint.size()},
                std::nullopt})));
        }
        auto transaction = take(project::ProjectTransaction::begin(document, journal, "authoring-review"));
        okay(transaction.set_presentation_catalog(std::move(review_presentation)));
        static_cast<void>(take(transaction.commit("authoring.review.presentation")));
        okay(journal.verify());
        request.view.projection = project::CameraProjection::perspective;
        const core::Quaternion qx{std::sin(pitch / 2.0), 0.0, 0.0, std::cos(pitch / 2.0)};
        request.view.vertical_fov_radians = fov;
        request.view.near_plane = 0.01;
        request.view.far_plane = 1000.0;
        request.tone_mode = render::ToneMapMode::neutral;
        request.exposure_ev = 0.0;
        request.shadows.enabled = false;
        request.temporal.enabled = temporal == "on";
        request.refraction.enabled = refraction == "scene";
        request.temporal.session = 1U;
        request = take(d3d12::prepare_readback_snapshot_request(document, project_path, request));
        auto renderer = take(d3d12::AuthoringViewport::create());
        auto source = std::make_shared<const project::ProjectDocument>(std::move(document));
        production::AuthoringViewportImage reusable;
        std::ostringstream receipt;
        receipt << std::setprecision(17)
                << "source_sha256=" << source_digest.hex() << "\nhdr_source_sha256=" << decoded.source_digest.hex()
                << "\nhdr_source_extent=" << decoded.source_width << 'x' << decoded.source_height
                << "\nhdr_lighting_grid=" << decoded.width << 'x' << decoded.height
                << "\nhdr_lighting_quality=" << (high_detail_hdr ? "high" : "standard")
                << "\nhdr_decode=solid_angle_filtered_rec709\nreview_direct_lights=suppressed_in_memory"
                << "\nsource_project=unchanged\nreview_presentation=journaled_copy\nexposure_ev=0\ntone_map=reinhard_srgb"
                << "\ntransmission=" << (refraction == "environment" ? "environment_only" : "scene_requested")
                << "\nrequested_transmission=" << refraction << "\ntemporal_history=" << temporal
                << "\ndiagnostic_displaced_object=" << displaced_object.value
                << "\ndiagnostic_displacement_world=" << (displaced_object ? "10000,0,0" : "0,0,0")
                << "\ncontact_shadows=disabled\nextent=" << request.width << 'x' << request.height
                << "\nframes=" << frames << "\ntarget=" << target.x << ',' << target.y << ',' << target.z
                << "\ndistance=" << distance << "\ninitial_yaw=" << yaw << "\npitch=" << pitch
                << "\nvertical_fov=" << fov << "\nmeasurement=logical_allocations_not_peak_or_residency\n";
        std::string device_name;
        for (std::uint32_t i = 0U; i < frames; ++i) {
            const double angle = yaw + 2.0 * 3.14159265358979323846 * i / frames;
            request.view.position = {target.x + distance * std::cos(pitch) * std::sin(angle),
                                     target.y - distance * std::sin(pitch),
                                     target.z + distance * std::cos(pitch) * std::cos(angle)};
            const core::Quaternion qy{0.0, std::sin(angle / 2.0), 0.0, std::cos(angle / 2.0)};
            request.view.rotation = (qy * qx).normalized();
            request.temporal.camera_cut = i == 0U;
            auto frame = take(renderer->render(source, request, std::move(reusable)));
            write_image(root, frame.image, i);
            device_name = frame.device_name;
            receipt << "frame=" << i << " yaw=" << angle
                    << " display_rgba_sha256=" << assets::sha256(frame.image.display_rgba8).hex()
                    << " gpu_raster_ms=" << frame.gpu_milliseconds
                    << " gpu_all_stages_ms=" << frame.gpu_all_stages_milliseconds
                    << " logical_resident_bytes=" << frame.logical_resident_bytes
                    << " host_attachment_bytes=" << frame.image.attachment_bytes
                    << " material_resident_bytes=" << frame.material_resident_bytes
                    << " environment_resident_bytes=" << frame.environment_resident_bytes
                    << " refraction_snapshot_bytes=" << frame.refraction_resident_bytes
                    << " temporal_resident_bytes=" << frame.temporal_resident_bytes
                    << " temporal_history_eligible=" << frame.temporal_history_eligible
                    << " temporal_reset=" << frame.temporal_reset
                    << " draw_calls=" << frame.draw_calls << " triangles=" << frame.triangles << '\n';
            if (i == 0U)
                for (const auto& diagnostic : frame.image.diagnostics) receipt << "diagnostic=" << diagnostic << '\n';
            reusable = std::move(frame.image);
        }
        if (assets::sha256(read_bounded(project_path, 128U * 1024U * 1024U)) != source_digest ||
            assets::sha256(read_bounded(hdr_path, 32U * 1024U * 1024U)) != decoded.source_digest)
            throw std::runtime_error("review source changed during capture; incomplete evidence retained");
        receipt << "device=" << device_name << '\n';
        auto receipt_file = output_new(root / "receipt.txt");
        receipt_file << receipt.str();
        receipt_file.close();
        if (!receipt_file) throw std::runtime_error("authoring review receipt write failed");
        std::cout << "Authoring viewport review captured " << frames << " frames on " << device_name
                  << " at " << request.width << 'x' << request.height
                  << "; see receipt for actual refraction and fallbacks.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Authoring review refused: " << error.what() << '\n';
        return 1;
    }
}
