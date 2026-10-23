#include <carto/web_geometry/package.hpp>
#include <carto/web_geometry/page_payload.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <limits>
#include <span>
#include <sstream>
#include <string>
#include <utility>

namespace carto::web_geometry {

namespace {

constexpr std::size_t kMaxPackageBytes = 256U * 1024U * 1024U;
constexpr std::uint64_t kMaxClusters = 1'000'000U;
constexpr std::uint64_t kMaxSourceTriangles = 10'000'000U;
constexpr std::uint64_t kMaxTriangleReferences =
    WebGeometryImportBudget::kMaxTriangles;
constexpr std::uint64_t kMaxProvenance = 1'000'000U;
constexpr std::uint64_t kMaxPages = 1'000'000U;
constexpr std::uint64_t kMaxPayload = 256U * 1024U * 1024U;

core::Diagnostic invalid(std::string message) {
    return core::Diagnostic(core::ErrorCode::invalid_argument, std::move(message));
}

core::Diagnostic validation(std::string message) {
    return core::Diagnostic(core::ErrorCode::validation_failed, std::move(message));
}

bool valid_runtime_label(const std::string& value) {
    return !value.empty() && value.size() <= 256U &&
        std::all_of(value.begin(), value.end(), [](const char character) {
            const auto code = static_cast<unsigned char>(character);
            return code >= 0x20U && code != 0x7fU;
        });
}

assets::Sha256Digest digest_text(const std::string& text) {
    return assets::sha256(std::span<const std::uint8_t>{
        reinterpret_cast<const std::uint8_t*>(text.data()), text.size()});
}

std::string runtime_budget_canonical(const WebGeometryRuntimeBudget& budget) {
    std::ostringstream canonical;
    canonical << "web-geometry-runtime-budget-v1\n"
              << "device=" << std::quoted(budget.device_name) << '\n'
              << "driver=" << std::quoted(budget.driver) << '\n'
              << "viewport=" << budget.viewport_width << 'x' << budget.viewport_height << '\n'
              << "max_clusters=" << budget.max_clusters << '\n'
              << "max_triangles=" << budget.max_triangles << '\n'
              << "max_pages=" << budget.max_pages << '\n'
              << "max_resident_payload_bytes=" << budget.max_resident_payload_bytes << '\n';
    return canonical.str();
}

std::string runtime_receipt_canonical(
    const WebGeometryRuntimeAdmissionReceipt& receipt) {
    std::ostringstream canonical;
    canonical << "web-geometry-runtime-admission-v1\n"
              << "package=" << receipt.package_digest.hex() << '\n'
              << "source_revision=" << receipt.source_revision.value() << '\n'
              << "options=" << receipt.options_digest.hex() << '\n'
              << "consumer_admission=" << receipt.consumer_admission_digest.hex() << '\n'
              << "runtime_budget=" << receipt.runtime_budget_digest.hex() << '\n'
              << "device=" << std::quoted(receipt.device_name) << '\n'
              << "driver=" << std::quoted(receipt.driver) << '\n'
              << "viewport=" << receipt.viewport_width << 'x' << receipt.viewport_height << '\n'
              << "clusters=" << receipt.cluster_count << '\n'
              << "triangles=" << receipt.triangle_count << '\n'
              << "pages=" << receipt.page_count << '\n'
              << "resident_payload_bytes=" << receipt.resident_payload_bytes << '\n'
              << "max_clusters=" << receipt.max_clusters << '\n'
              << "max_triangles=" << receipt.max_triangles << '\n'
              << "max_pages=" << receipt.max_pages << '\n'
              << "max_resident_payload_bytes=" << receipt.max_resident_payload_bytes << '\n';
    return canonical.str();
}

template <typename T>
core::Result<void> read_label(std::istream& input, const char* expected, T& value) {
    std::string label;
    if (!(input >> label) || label != expected || !(input >> value)) {
        return core::Result<void>::failure(invalid(
            std::string("web geometry package expected ") + expected));
    }
    return core::Result<void>::success();
}

core::Result<void> read_count(
    std::istream& input,
    const char* label,
    std::uint64_t maximum,
    std::size_t& value) {
    std::uint64_t parsed = 0U;
    if (auto result = read_label(input, label, parsed); !result) return result;
    if (parsed > maximum || parsed > std::numeric_limits<std::size_t>::max()) {
        return core::Result<void>::failure(validation(
            std::string("web geometry package count exceeds bound: ") + label));
    }
    value = static_cast<std::size_t>(parsed);
    return core::Result<void>::success();
}

core::Result<assets::Sha256Digest> read_digest(std::istream& input, const char* label) {
    std::string actual_label;
    std::string encoded;
    if (!(input >> actual_label >> encoded) || actual_label != label) {
        return core::Result<assets::Sha256Digest>::failure(invalid(
            std::string("web geometry package expected ") + label));
    }
    const auto digest = assets::Sha256Digest::from_hex(encoded);
    if (!digest) return digest;
    return digest;
}

void write_bounds(std::ostream& output, const ClusterBounds& bounds) {
    output << std::setprecision(17)
           << bounds.aabb.minimum.x << ' ' << bounds.aabb.minimum.y << ' '
           << bounds.aabb.minimum.z << ' ' << bounds.aabb.maximum.x << ' '
           << bounds.aabb.maximum.y << ' ' << bounds.aabb.maximum.z << ' '
           << bounds.sphere_center.x << ' ' << bounds.sphere_center.y << ' '
           << bounds.sphere_center.z << ' ' << bounds.sphere_radius;
}

void write_aabb(std::ostream& output, const core::Bounds3d& bounds) {
    output << std::setprecision(17)
           << bounds.minimum.x << ' ' << bounds.minimum.y << ' '
           << bounds.minimum.z << ' ' << bounds.maximum.x << ' '
           << bounds.maximum.y << ' ' << bounds.maximum.z;
}

core::Result<ClusterBounds> read_bounds(std::istream& input) {
    ClusterBounds bounds;
    if (!(input >> bounds.aabb.minimum.x >> bounds.aabb.minimum.y >> bounds.aabb.minimum.z
          >> bounds.aabb.maximum.x >> bounds.aabb.maximum.y >> bounds.aabb.maximum.z
          >> bounds.sphere_center.x >> bounds.sphere_center.y >> bounds.sphere_center.z
          >> bounds.sphere_radius)) {
        return core::Result<ClusterBounds>::failure(
            invalid("web geometry package cluster bounds are truncated"));
    }
    return core::Result<ClusterBounds>::success(bounds);
}

core::Result<core::Bounds3d> read_aabb(std::istream& input) {
    core::Bounds3d bounds;
    if (!(input >> bounds.minimum.x >> bounds.minimum.y >> bounds.minimum.z
          >> bounds.maximum.x >> bounds.maximum.y >> bounds.maximum.z)) {
        return core::Result<core::Bounds3d>::failure(
            invalid("web geometry package page bounds are truncated"));
    }
    return core::Result<core::Bounds3d>::success(bounds);
}

core::Result<void> validate_page_payloads(const WebGeometryPackage& package) {
    for (const auto& page : package.pages) {
        const auto decoded = decode_runtime_page_payload(
            std::span<const std::uint8_t>{package.payload}.subspan(
                static_cast<std::size_t>(page.payload_offset),
                static_cast<std::size_t>(page.payload_bytes)));
        if (!decoded) return core::Result<void>::failure(decoded.error());
        if (decoded.value().clusters.size() != page.cluster_count) {
            return core::Result<void>::failure(validation(
                "web geometry runtime page cluster count disagrees with the page table"));
        }
        for (std::size_t page_offset = 0U; page_offset < decoded.value().clusters.size();
             ++page_offset) {
            const auto cluster_id = static_cast<ClusterId>(page.first_cluster + page_offset);
            const auto& decoded_cluster = decoded.value().clusters[page_offset];
            if (decoded_cluster.cluster != cluster_id) {
                return core::Result<void>::failure(validation(
                    "web geometry runtime page cluster ordering is inconsistent"));
            }
            const auto& cluster = package.hierarchy.clusters[cluster_id];
            if (decoded_cluster.triangles.size() != cluster.triangle_count) {
                return core::Result<void>::failure(validation(
                    "web geometry runtime page triangle count disagrees with the hierarchy"));
            }
            for (std::size_t triangle_offset = 0U;
                 triangle_offset < decoded_cluster.triangles.size(); ++triangle_offset) {
                const auto& decoded_triangle = decoded_cluster.triangles[triangle_offset];
                const auto source_triangle =
                    package.hierarchy.triangle_indices[cluster.first_triangle + triangle_offset];
                if (decoded_triangle.source_triangle != source_triangle ||
                    decoded_triangle.source_face !=
                        package.hierarchy.source_faces[source_triangle]) {
                    return core::Result<void>::failure(validation(
                        "web geometry runtime page triangle identity is inconsistent"));
                }
                for (const auto& point : decoded_triangle.positions) {
                    ClusterId enclosing_id = cluster_id;
                    for (std::size_t depth = 0U;
                         depth < package.hierarchy.clusters.size(); ++depth) {
                        const auto& enclosing = package.hierarchy.clusters[enclosing_id];
                        const double distance = (point - enclosing.bounds.sphere_center).length();
                        const double tolerance = 32.0 * std::numeric_limits<double>::epsilon() *
                            std::max(1.0, enclosing.bounds.sphere_radius);
                        if (!std::isfinite(distance) ||
                            distance > enclosing.bounds.sphere_radius + tolerance) {
                            return core::Result<void>::failure(validation(
                                "web geometry page triangle escapes its cluster or ancestor sphere"));
                        }
                        if (!enclosing.parent.has_value()) break;
                        enclosing_id = *enclosing.parent;
                        if (depth + 1U == package.hierarchy.clusters.size()) {
                            return core::Result<void>::failure(validation(
                                "web geometry cluster ancestry exceeds the hierarchy bound"));
                        }
                    }
                }
            }
        }
    }
    return core::Result<void>::success();
}

} // namespace

core::Result<std::string> serialize_package(const WebGeometryPackage& package) {
    if (auto result = package.validate(); !result) {
        return core::Result<std::string>::failure(result.error());
    }
    if (auto result = validate_page_payloads(package); !result) {
        return core::Result<std::string>::failure(result.error());
    }
    std::ostringstream output;
    output << "CARTOGRAPHER_WEB_GEOMETRY 2\n"
           << "SCHEMA " << package.schema_version << '\n'
           << "COMPILER " << package.compiler_version << '\n'
           << "SOURCE_REVISION " << package.source_revision.value() << '\n'
           << "OPTIONS " << package.options.target_triangles_per_leaf << ' '
           << package.options.hard_max_triangles_per_leaf << ' '
           << package.options.max_children_per_node << ' '
           << package.options.target_page_payload_bytes << ' '
           << package.options.hard_max_page_payload_bytes << ' '
           << package.options.preserve_material_boundaries << ' '
           << package.options.preserve_semantic_boundaries << ' '
           << package.options.preserve_hard_normal_boundaries << ' '
           << package.options.preserve_authored_break_boundaries << '\n'
           << "HIERARCHY_REVISION " << package.hierarchy.source_revision.value() << '\n'
           << "HIERARCHY_DIGEST " << package.hierarchy.digest.hex() << '\n'
           << "CLUSTERS " << package.hierarchy.clusters.size() << '\n';
    for (const auto& cluster : package.hierarchy.clusters) {
        output << "C " << cluster.id << ' '
               << (cluster.parent.has_value() ? static_cast<std::int64_t>(*cluster.parent) : -1)
               << ' ' << cluster.first_child << ' ' << cluster.child_count << ' '
               << cluster.first_triangle << ' ' << cluster.triangle_count << ' ';
        write_bounds(output, cluster.bounds);
        output << ' ' << std::setprecision(17) << cluster.geometric_error << ' '
               << cluster.material_group << ' ' << cluster.semantic_group << ' '
               << cluster.page << ' ' << cluster.content_digest << '\n';
    }
    output << "CHILDREN " << package.hierarchy.child_ids.size();
    for (const auto child : package.hierarchy.child_ids) output << ' ' << child;
    output << "\nTRIANGLES " << package.hierarchy.triangle_indices.size();
    for (const auto triangle : package.hierarchy.triangle_indices) output << ' ' << triangle;
    output << "\nFACES " << package.hierarchy.source_faces.size();
    for (const auto face : package.hierarchy.source_faces) output << ' ' << face.value;
    output << "\nPROVENANCE " << package.provenance.size() << '\n';
    for (const auto& trace : package.provenance) {
        output << "P " << trace.cluster << ' ' << trace.source_revision.value() << ' '
               << trace.compiler_options_digest << ' ' << trace.content_digest << ' '
               << trace.source_faces.size();
        for (const auto face : trace.source_faces) output << ' ' << face.value;
        output << '\n';
    }
    output << "PAGES " << package.pages.size() << '\n';
    for (const auto& page : package.pages) {
        output << "R " << page.id << ' ' << page.first_cluster << ' ' << page.cluster_count
               << ' ' << page.payload_offset << ' ' << page.payload_bytes << ' ';
        write_aabb(output, page.bounds);
        output << " PAGE_DIGEST " << page.digest.hex() << '\n';
    }
    output << "PAYLOAD " << package.payload.size();
    for (const auto byte : package.payload) output << ' ' << static_cast<unsigned int>(byte);
    output << "\nPACKAGE_DIGEST " << package.digest.hex() << "\nEND\n";
    const std::string text = output.str();
    if (text.size() > kMaxPackageBytes) {
        return core::Result<std::string>::failure(validation(
            "web geometry serialized package exceeds the bounded size"));
    }
    return core::Result<std::string>::success(text);
}

core::Result<WebGeometryPackage> deserialize_package(std::string_view text) {
    if (text.empty() || text.size() > kMaxPackageBytes) {
        return core::Result<WebGeometryPackage>::failure(validation(
            "web geometry package is empty or exceeds the bounded size"));
    }
    std::istringstream input{std::string{text}};
    std::string magic;
    std::uint32_t format_version = 0U;
    if (!(input >> magic >> format_version) || magic != "CARTOGRAPHER_WEB_GEOMETRY" ||
        format_version != 2U) {
        return core::Result<WebGeometryPackage>::failure(
            invalid("web geometry package header is unsupported"));
    }

    WebGeometryPackage package;
    if (auto result = read_label(input, "SCHEMA", package.schema_version); !result) {
        return core::Result<WebGeometryPackage>::failure(result.error());
    }
    if (auto result = read_label(input, "COMPILER", package.compiler_version); !result) {
        return core::Result<WebGeometryPackage>::failure(result.error());
    }
    std::uint64_t source_revision = 0U;
    if (auto result = read_label(input, "SOURCE_REVISION", source_revision); !result) {
        return core::Result<WebGeometryPackage>::failure(result.error());
    }
    package.source_revision = core::Revision{source_revision};
    if (auto result = read_label(input, "OPTIONS", package.options.target_triangles_per_leaf);
        !result || !(input >> package.options.hard_max_triangles_per_leaf
                     >> package.options.max_children_per_node
                     >> package.options.target_page_payload_bytes
                     >> package.options.hard_max_page_payload_bytes
                     >> package.options.preserve_material_boundaries
                     >> package.options.preserve_semantic_boundaries
                     >> package.options.preserve_hard_normal_boundaries
                     >> package.options.preserve_authored_break_boundaries)) {
        return core::Result<WebGeometryPackage>::failure(invalid(
            "web geometry package options are truncated"));
    }
    std::uint64_t hierarchy_revision = 0U;
    if (auto result = read_label(input, "HIERARCHY_REVISION", hierarchy_revision); !result) {
        return core::Result<WebGeometryPackage>::failure(result.error());
    }
    package.hierarchy.source_revision = core::Revision{hierarchy_revision};
    const auto hierarchy_digest = read_digest(input, "HIERARCHY_DIGEST");
    if (!hierarchy_digest) return core::Result<WebGeometryPackage>::failure(hierarchy_digest.error());
    package.hierarchy.digest = hierarchy_digest.value();

    std::size_t count = 0U;
    if (auto result = read_count(input, "CLUSTERS", kMaxClusters, count); !result) {
        return core::Result<WebGeometryPackage>::failure(result.error());
    }
    package.hierarchy.clusters.reserve(count);
    for (std::size_t index = 0U; index < count; ++index) {
        std::string label;
        ClusterRecord cluster;
        std::int64_t parent = -1;
        if (!(input >> label >> cluster.id >> parent >> cluster.first_child >> cluster.child_count
              >> cluster.first_triangle >> cluster.triangle_count) || label != "C") {
            return core::Result<WebGeometryPackage>::failure(invalid(
                "web geometry package cluster record is truncated"));
        }
        if (parent < -1 || parent > std::numeric_limits<ClusterId>::max()) {
            return core::Result<WebGeometryPackage>::failure(invalid(
                "web geometry package cluster parent is invalid"));
        }
        if (parent >= 0) cluster.parent = static_cast<ClusterId>(parent);
        const auto bounds = read_bounds(input);
        if (!bounds) return core::Result<WebGeometryPackage>::failure(bounds.error());
        cluster.bounds = bounds.value();
        if (!(input >> cluster.geometric_error >> cluster.material_group >> cluster.semantic_group
              >> cluster.page >> cluster.content_digest)) {
            return core::Result<WebGeometryPackage>::failure(invalid(
                "web geometry package cluster record is truncated"));
        }
        package.hierarchy.clusters.push_back(std::move(cluster));
    }
    if (auto result = read_count(input, "CHILDREN", kMaxClusters, count); !result) {
        return core::Result<WebGeometryPackage>::failure(result.error());
    }
    package.hierarchy.child_ids.resize(count);
    for (auto& child : package.hierarchy.child_ids) {
        if (!(input >> child)) return core::Result<WebGeometryPackage>::failure(
            invalid("web geometry package child table is truncated"));
    }
    if (auto result = read_count(input, "TRIANGLES", kMaxTriangleReferences, count); !result) {
        return core::Result<WebGeometryPackage>::failure(result.error());
    }
    package.hierarchy.triangle_indices.resize(count);
    for (auto& triangle : package.hierarchy.triangle_indices) {
        if (!(input >> triangle)) return core::Result<WebGeometryPackage>::failure(
            invalid("web geometry package triangle table is truncated"));
    }
    if (auto result = read_count(input, "FACES", kMaxSourceTriangles, count); !result) {
        return core::Result<WebGeometryPackage>::failure(result.error());
    }
    package.hierarchy.source_faces.resize(count);
    for (auto& face : package.hierarchy.source_faces) {
        if (!(input >> face.value)) return core::Result<WebGeometryPackage>::failure(
            invalid("web geometry package source-face table is truncated"));
    }
    if (auto result = read_count(input, "PROVENANCE", kMaxProvenance, count); !result) {
        return core::Result<WebGeometryPackage>::failure(result.error());
    }
    package.provenance.reserve(count);
    for (std::size_t index = 0U; index < count; ++index) {
        ClusterProvenance trace;
        std::size_t face_count = 0U;
        std::uint64_t trace_revision = 0U;
        std::string label;
        if (!(input >> label >> trace.cluster >> trace_revision
              >> trace.compiler_options_digest >> trace.content_digest >> face_count) ||
            label != "P" || face_count > kMaxSourceTriangles) {
            return core::Result<WebGeometryPackage>::failure(invalid(
                "web geometry package provenance record is invalid"));
        }
        trace.source_revision = core::Revision{trace_revision};
        trace.source_faces.resize(face_count);
        for (auto& face : trace.source_faces) {
            if (!(input >> face.value)) return core::Result<WebGeometryPackage>::failure(
                invalid("web geometry package provenance faces are truncated"));
        }
        package.provenance.push_back(std::move(trace));
    }
    if (auto result = read_count(input, "PAGES", kMaxPages, count); !result) {
        return core::Result<WebGeometryPackage>::failure(result.error());
    }
    package.pages.reserve(count);
    for (std::size_t index = 0U; index < count; ++index) {
        RuntimeGeometryPage page;
        std::string label;
        if (!(input >> label >> page.id >> page.first_cluster >> page.cluster_count
              >> page.payload_offset >> page.payload_bytes) || label != "R") {
            return core::Result<WebGeometryPackage>::failure(invalid(
                "web geometry package page record is truncated"));
        }
        const auto bounds = read_aabb(input);
        if (!bounds) return core::Result<WebGeometryPackage>::failure(bounds.error());
        page.bounds = bounds.value();
        const auto digest = read_digest(input, "PAGE_DIGEST");
        if (!digest) return core::Result<WebGeometryPackage>::failure(digest.error());
        page.digest = digest.value();
        package.pages.push_back(std::move(page));
    }
    if (auto result = read_count(input, "PAYLOAD", kMaxPayload, count); !result) {
        return core::Result<WebGeometryPackage>::failure(result.error());
    }
    package.payload.resize(count);
    for (auto& byte : package.payload) {
        unsigned int parsed = 0U;
        if (!(input >> parsed) || parsed > 255U) {
            return core::Result<WebGeometryPackage>::failure(invalid(
                "web geometry package payload byte is invalid"));
        }
        byte = static_cast<std::uint8_t>(parsed);
    }
    const auto package_digest = read_digest(input, "PACKAGE_DIGEST");
    if (!package_digest) return core::Result<WebGeometryPackage>::failure(package_digest.error());
    package.digest = package_digest.value();
    std::string end;
    if (!(input >> end) || end != "END") {
        return core::Result<WebGeometryPackage>::failure(
            invalid("web geometry package end marker is missing"));
    }
    std::string trailing;
    if (input >> trailing) {
        return core::Result<WebGeometryPackage>::failure(validation(
            "web geometry package contains trailing data"));
    }
    if (auto result = package.validate(); !result) {
        return core::Result<WebGeometryPackage>::failure(result.error());
    }
    if (auto result = validate_page_payloads(package); !result) {
        return core::Result<WebGeometryPackage>::failure(result.error());
    }
    return core::Result<WebGeometryPackage>::success(std::move(package));
}

core::Result<void> WebGeometryImportBudget::validate() const {
    if (expected_source_revision.value() == 0U || expected_source_revision.exhausted() ||
        max_serialized_bytes == 0U || max_serialized_bytes > kMaxSerializedBytes ||
        max_clusters == 0U || max_clusters > kMaxClusters ||
        max_triangles == 0U || max_triangles > kMaxTriangleReferences ||
        max_pages == 0U || max_pages > kMaxPages ||
        max_payload_bytes == 0U || max_payload_bytes > kMaxPayloadBytes) {
        return core::Result<void>::failure(invalid(
            "web geometry consumer import budget is invalid"));
    }
    return core::Result<void>::success();
}

core::Result<AdmittedWebGeometryPackage> admit_package_for_consumer(
    std::string_view text,
    const WebGeometryImportBudget& budget) {
    if (auto valid = budget.validate(); !valid) {
        return core::Result<AdmittedWebGeometryPackage>::failure(valid.error());
    }
    if (text.empty() || text.size() > budget.max_serialized_bytes) {
        return core::Result<AdmittedWebGeometryPackage>::failure(validation(
            "web geometry package exceeds the consumer serialized-size budget"));
    }
    auto decoded = deserialize_package(text);
    if (!decoded) {
        return core::Result<AdmittedWebGeometryPackage>::failure(
            decoded.error().with_context("web geometry consumer import"));
    }
    const auto canonical_text = serialize_package(decoded.value());
    if (!canonical_text) {
        return core::Result<AdmittedWebGeometryPackage>::failure(canonical_text.error());
    }
    if (canonical_text.value() != text) {
        return core::Result<AdmittedWebGeometryPackage>::failure(validation(
            "web geometry package encoding is not canonical"));
    }
    const auto& package = decoded.value();
    if (package.source_revision != budget.expected_source_revision) {
        return core::Result<AdmittedWebGeometryPackage>::failure(core::Diagnostic(
            core::ErrorCode::stale_data,
            "web geometry package source revision is stale"));
    }
    const assets::Sha256Digest options_digest = package.options.digest();
    if (budget.expected_options_digest.has_value() &&
        *budget.expected_options_digest != options_digest) {
        return core::Result<AdmittedWebGeometryPackage>::failure(core::Diagnostic(
            core::ErrorCode::stale_data,
            "web geometry package compiler options are stale"));
    }
    const std::uint64_t cluster_count = package.hierarchy.clusters.size();
    const std::uint64_t triangle_count = package.hierarchy.triangle_indices.size();
    const std::uint64_t page_count = package.pages.size();
    const std::uint64_t payload_bytes = package.payload.size();
    if (cluster_count > budget.max_clusters || triangle_count > budget.max_triangles ||
        page_count > budget.max_pages || payload_bytes > budget.max_payload_bytes) {
        return core::Result<AdmittedWebGeometryPackage>::failure(validation(
            "web geometry package exceeds the consumer object or payload budget"));
    }
    std::ostringstream admission_canonical;
    admission_canonical << "web-geometry-consumer-admission-v1\n"
                        << "package=" << package.digest.hex() << '\n'
                        << "source_revision=" << package.source_revision.value() << '\n'
                        << "options=" << options_digest.hex() << '\n'
                        << "clusters=" << cluster_count << '\n'
                        << "triangles=" << triangle_count << '\n'
                        << "pages=" << page_count << '\n'
                        << "payload_bytes=" << payload_bytes << '\n'
                        << "serialized_bytes=" << text.size() << '\n';
    const std::string admission_text = admission_canonical.str();
    const auto admission_digest = assets::sha256(std::span<const std::uint8_t>{
        reinterpret_cast<const std::uint8_t*>(admission_text.data()), admission_text.size()});
    WebGeometryImportReceipt receipt{
        package.digest,
        package.source_revision,
        options_digest,
        cluster_count,
        triangle_count,
        page_count,
        payload_bytes,
        text.size(),
        admission_digest};
    return core::Result<AdmittedWebGeometryPackage>::success(
        AdmittedWebGeometryPackage{std::move(decoded.value()), std::move(receipt)});
}

core::Result<void> WebGeometryRuntimeBudget::validate() const {
    if (!valid_runtime_label(device_name) || !valid_runtime_label(driver) ||
        viewport_width == 0U || viewport_height == 0U || max_clusters == 0U ||
        max_triangles == 0U || max_pages == 0U || max_resident_payload_bytes == 0U) {
        return core::Result<void>::failure(invalid(
            "web geometry runtime budget requires device, driver, viewport, and positive limits"));
    }
    return core::Result<void>::success();
}

core::Result<void> WebGeometryRuntimeAdmissionReceipt::validate() const {
    if (!valid_runtime_label(device_name) || !valid_runtime_label(driver) ||
        source_revision.exhausted() || viewport_width == 0U || viewport_height == 0U ||
        max_clusters == 0U || max_triangles == 0U || max_pages == 0U ||
        max_resident_payload_bytes == 0U || cluster_count > max_clusters ||
        triangle_count > max_triangles || page_count > max_pages ||
        resident_payload_bytes > max_resident_payload_bytes) {
        return core::Result<void>::failure(validation(
            "web geometry runtime admission receipt fields are inconsistent"));
    }
    const WebGeometryRuntimeBudget budget{
        device_name,
        driver,
        viewport_width,
        viewport_height,
        max_clusters,
        max_triangles,
        max_pages,
        max_resident_payload_bytes,
    };
    if (digest_text(runtime_budget_canonical(budget)) != runtime_budget_digest ||
        digest_text(runtime_receipt_canonical(*this)) != runtime_admission_digest) {
        return core::Result<void>::failure(validation(
            "web geometry runtime admission receipt digest does not match its fields"));
    }
    return core::Result<void>::success();
}

core::Result<RuntimeAdmittedWebGeometryPackage> admit_package_for_runtime(
    const std::string_view text,
    const WebGeometryImportBudget& import_budget,
    const WebGeometryRuntimeBudget& runtime_budget) {
    if (auto result = runtime_budget.validate(); !result) {
        return core::Result<RuntimeAdmittedWebGeometryPackage>::failure(result.error());
    }
    auto consumer_admission = admit_package_for_consumer(text, import_budget);
    if (!consumer_admission) {
        return core::Result<RuntimeAdmittedWebGeometryPackage>::failure(
            consumer_admission.error());
    }
    const auto& package = consumer_admission.value().package;
    const std::uint64_t cluster_count = package.hierarchy.clusters.size();
    const std::uint64_t triangle_count = package.hierarchy.triangle_indices.size();
    const std::uint64_t page_count = package.pages.size();
    const std::uint64_t resident_payload_bytes = package.payload.size();
    if (cluster_count > runtime_budget.max_clusters ||
        triangle_count > runtime_budget.max_triangles ||
        page_count > runtime_budget.max_pages ||
        resident_payload_bytes > runtime_budget.max_resident_payload_bytes) {
        return core::Result<RuntimeAdmittedWebGeometryPackage>::failure(validation(
            "web geometry package exceeds the declared runtime device budget"));
    }

    const assets::Sha256Digest runtime_budget_digest =
        digest_text(runtime_budget_canonical(runtime_budget));
    WebGeometryRuntimeAdmissionReceipt receipt;
    receipt.package_digest = package.digest;
    receipt.source_revision = package.source_revision;
    receipt.options_digest = package.options.digest();
    receipt.consumer_admission_digest = consumer_admission.value().receipt.admission_digest;
    receipt.runtime_budget_digest = runtime_budget_digest;
    receipt.device_name = runtime_budget.device_name;
    receipt.driver = runtime_budget.driver;
    receipt.viewport_width = runtime_budget.viewport_width;
    receipt.viewport_height = runtime_budget.viewport_height;
    receipt.cluster_count = cluster_count;
    receipt.triangle_count = triangle_count;
    receipt.page_count = page_count;
    receipt.resident_payload_bytes = resident_payload_bytes;
    receipt.max_clusters = runtime_budget.max_clusters;
    receipt.max_triangles = runtime_budget.max_triangles;
    receipt.max_pages = runtime_budget.max_pages;
    receipt.max_resident_payload_bytes = runtime_budget.max_resident_payload_bytes;
    receipt.runtime_admission_digest = digest_text(runtime_receipt_canonical(receipt));
    if (auto result = receipt.validate(); !result) {
        return core::Result<RuntimeAdmittedWebGeometryPackage>::failure(result.error());
    }
    return core::Result<RuntimeAdmittedWebGeometryPackage>::success(
        RuntimeAdmittedWebGeometryPackage{
            std::move(consumer_admission.value()), std::move(receipt)});
}

} // namespace carto::web_geometry
