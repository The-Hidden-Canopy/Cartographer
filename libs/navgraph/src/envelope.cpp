#include <carto/navgraph/envelope.hpp>

#include "detail.hpp"

#include <algorithm>
#include <iomanip>
#include <set>
#include <span>
#include <sstream>
#include <utility>

namespace carto::navgraph {
namespace {

constexpr std::string_view kMagic = "CARTOGRAPHER_NAVIGATION_ENVELOPE";
constexpr std::size_t kMaxNamespaceBytes = 256U;

void write_optional_text(std::ostream& output, const std::optional<std::string>& value) {
    if (value.has_value()) {
        output << std::quoted(value.value());
    } else {
        output << '-';
    }
}

bool read_optional_text(
    std::istream& input,
    std::optional<std::string>& value,
    std::size_t maximum) {
    input >> std::ws;
    if (input.peek() == '-') {
        static_cast<void>(input.get());
        value = std::nullopt;
        return true;
    }
    std::string text;
    if (!(input >> std::quoted(text)) || !detail::safe_text(text, maximum, true)) {
        return false;
    }
    value = std::move(text);
    return true;
}

template <typename Record, typename Key>
bool unique_key(const std::vector<Record>& records, Key&& key_of) {
    std::set<std::remove_cvref_t<std::invoke_result_t<Key, const Record&>>> seen;
    for (const auto& record : records) {
        if (!seen.insert(key_of(record)).second) return false;
    }
    return true;
}

template <typename Id>
bool read_id(std::istream& input, Id& value) {
    std::uint64_t encoded = 0U;
    if (!detail::read_uint64(input, encoded)) return false;
    value = Id{encoded};
    return true;
}

} // namespace

core::Result<void> PreparedNavigationEnvelope::validate() const {
    if (!detail::safe_text(source_namespace, kMaxNamespaceBytes) ||
        source_digest.is_zero()) {
        return core::Result<void>::failure(detail::invalid(
            "navigation envelope namespace or source digest binding is invalid"));
    }
    if (regions.size() > kMaxRecordsPerKind || routes.size() > kMaxRecordsPerKind ||
        corridors.size() > kMaxRecordsPerKind ||
        access_points.size() > kMaxRecordsPerKind || anchors.size() > kMaxRecordsPerKind ||
        links.size() > kMaxRecordsPerKind ||
        medium_transitions.size() > kMaxRecordsPerKind ||
        agent_profiles.size() > kMaxRecordsPerKind ||
        area_bindings.size() > kMaxRecordsPerKind ||
        path_bindings.size() > kMaxRecordsPerKind ||
        footprints.size() > kMaxRecordsPerKind) {
        return core::Result<void>::failure(detail::validation(
            "navigation envelope exceeds its bounded record capacity"));
    }

    for (const auto& region : regions) {
        if (auto result = region.validate(); !result) return result;
    }
    for (const auto& route : routes) {
        if (auto result = route.validate(); !result) return result;
    }
    for (const auto& corridor : corridors) {
        if (auto result = corridor.validate(); !result) return result;
    }
    for (const auto& access : access_points) {
        if (auto result = access.validate(); !result) return result;
    }
    for (const auto& anchor : anchors) {
        if (auto result = anchor.validate(); !result) return result;
    }
    for (const auto& link : links) {
        if (auto result = link.validate(); !result) return result;
    }
    for (const auto& transition : medium_transitions) {
        if (auto result = transition.validate(); !result) return result;
    }
    for (const auto& profile : agent_profiles) {
        if (auto result = profile.validate(); !result) return result;
    }
    for (const auto& binding : area_bindings) {
        if (auto result = binding.validate(); !result) return result;
    }
    for (const auto& binding : path_bindings) {
        if (auto result = binding.validate(); !result) return result;
    }
    for (const auto& footprint : footprints) {
        if (auto result = footprint.validate(); !result) return result;
    }

    if (!unique_key(regions, [](const SemanticAreaRegion& r) { return r.id; }) ||
        !unique_key(routes, [](const SemanticRouteProduct& r) { return r.id; }) ||
        !unique_key(corridors, [](const SemanticCorridorProduct& c) { return c.id; }) ||
        !unique_key(access_points, [](const StructureAccessProduct& a) { return a.id; }) ||
        !unique_key(anchors, [](const TraversalAnchor& a) { return a.id; }) ||
        !unique_key(links, [](const OffMeshLinkCandidate& l) { return l.id; }) ||
        !unique_key(agent_profiles, [](const AgentProfile& p) { return p.profile_identity; }) ||
        !unique_key(footprints, [](const NavigationTileFootprint& f) { return f.canonical(); })) {
        return core::Result<void>::failure(detail::validation(
            "navigation envelope contains duplicate compiled identities"));
    }

    std::set<SourceAreaId> source_areas;
    for (const auto& region : regions) {
        source_areas.insert(region.source_area);
    }
    std::set<RegionId> region_ids;
    for (const auto& region : regions) {
        region_ids.insert(region.id);
        if (region.parent_area.has_value() && !source_areas.contains(region.parent_area.value())) {
            return core::Result<void>::failure(detail::validation(
                "navigation region references a parent area outside the envelope scope"));
        }
    }

    std::set<RouteId> route_ids;
    for (const auto& route : routes) {
        route_ids.insert(route.id);
        if ((route.start_area.has_value() && !source_areas.contains(route.start_area.value())) ||
            (route.end_area.has_value() && !source_areas.contains(route.end_area.value()))) {
            return core::Result<void>::failure(detail::validation(
                "navigation route references a terminal area outside the envelope scope"));
        }
    }

    for (const auto& corridor : corridors) {
        const auto route = std::find_if(
            routes.begin(), routes.end(),
            [&corridor](const SemanticRouteProduct& candidate) {
                return candidate.id == corridor.route;
            });
        if (route == routes.end()) {
            return core::Result<void>::failure(detail::validation(
                "navigation corridor references a missing route"));
        }
        if (route->source_path != corridor.source_path) {
            return core::Result<void>::failure(detail::validation(
                "navigation corridor source path does not match its route"));
        }
    }

    std::set<AccessId> access_ids;
    for (const auto& access : access_points) {
        access_ids.insert(access.id);
    }
    std::set<AnchorId> anchor_ids;
    for (const auto& anchor : anchors) {
        anchor_ids.insert(anchor.id);
        if (anchor.region.has_value() && !region_ids.contains(anchor.region.value())) {
            return core::Result<void>::failure(detail::validation(
                "navigation anchor references a missing region"));
        }
        if (anchor.access.has_value() && !access_ids.contains(anchor.access.value())) {
            return core::Result<void>::failure(detail::validation(
                "navigation anchor references a missing access point"));
        }
    }

    for (const auto& link : links) {
        if (!anchor_ids.contains(link.start_anchor) ||
            !anchor_ids.contains(link.end_anchor)) {
            return core::Result<void>::failure(detail::validation(
                "navigation link references a missing anchor"));
        }
    }
    for (const auto& transition : medium_transitions) {
        const auto link = std::find_if(
            links.begin(), links.end(),
            [&transition](const OffMeshLinkCandidate& candidate) {
                return candidate.id == transition.link;
            });
        if (link == links.end()) {
            return core::Result<void>::failure(detail::validation(
                "navigation medium transition references a missing link"));
        }
        if (link->start_medium != transition.from_medium ||
            link->end_medium != transition.to_medium ||
            link->primitive != transition.primitive) {
            return core::Result<void>::failure(detail::validation(
                "navigation medium transition disagrees with its link"));
        }
    }

    std::set<std::string> profile_identities;
    for (const auto& profile : agent_profiles) {
        profile_identities.insert(profile.profile_identity);
    }
    for (const auto& binding : area_bindings) {
        if (!region_ids.contains(binding.region) ||
            !profile_identities.contains(binding.profile_identity)) {
            return core::Result<void>::failure(detail::validation(
                "navigation area binding references a missing region or profile"));
        }
    }
    for (const auto& binding : path_bindings) {
        if (!route_ids.contains(binding.route) ||
            !profile_identities.contains(binding.profile_identity)) {
            return core::Result<void>::failure(detail::validation(
                "navigation path binding references a missing route or profile"));
        }
    }
    return core::Result<void>::success();
}

std::string PreparedNavigationEnvelope::serialize() const {
    std::ostringstream output;
    output << kMagic << ' ' << kSchemaVersion << '\n';
    output << std::setprecision(17);

    output << "HEADER " << std::quoted(source_namespace) << ' '
           << source_revision.value() << ' ';
    detail::write_digest(output, source_digest);
    output << ' ' << (scene_root.has_value() ? scene_root->value : 0U) << '\n';

    output << "REGIONS " << regions.size() << '\n';
    for (const auto& region : regions) {
        output << "REGION " << region.id.value << ' ' << region.source_area.value << ' ';
        detail::write_optional_id(output, region.parent_area);
        output << ' ' << static_cast<unsigned>(region.plane) << ' '
               << region.minimum_clearance << ' ' << std::quoted(region.semantic_type)
               << ' ' << region.boundary.size();
        for (const auto& point : region.boundary) {
            output << ' ';
            detail::write_vec3(output, point);
        }
        output << ' ';
        detail::write_bounds(output, region.bounds);
        output << '\n';
    }

    output << "ROUTES " << routes.size() << '\n';
    for (const auto& route : routes) {
        output << "ROUTE " << route.id.value << ' ' << route.source_path.value << ' '
               << (route.closed ? 1 : 0) << ' ' << route.width << ' '
               << static_cast<unsigned>(route.spine.plane) << ' '
               << std::quoted(route.semantic_type) << ' '
               << route.spine.control_points.size();
        for (const auto& point : route.spine.control_points) {
            output << ' ';
            detail::write_vec3(output, point);
        }
        for (const auto& distance : route.spine.cumulative_distances) {
            output << ' ' << distance;
        }
        output << ' ';
        detail::write_optional_id(output, route.start_area);
        output << ' ';
        detail::write_optional_id(output, route.end_area);
        output << ' ';
        detail::write_bounds(output, route.bounds);
        output << '\n';
    }

    output << "CORRIDORS " << corridors.size() << '\n';
    for (const auto& corridor : corridors) {
        output << "CORRIDOR " << corridor.id.value << ' ' << corridor.source_path.value
               << ' ' << corridor.route.value << ' '
               << static_cast<unsigned>(corridor.plane) << ' ' << corridor.polygon.size();
        for (const auto& point : corridor.polygon) {
            output << ' ';
            detail::write_vec3(output, point);
        }
        output << ' ';
        detail::write_bounds(output, corridor.corridor_bounds);
        output << '\n';
    }

    output << "ACCESS " << access_points.size() << '\n';
    for (const auto& access : access_points) {
        output << "ACCESS_POINT " << access.id.value << ' ' << access.source_graph.value
               << ' ' << access.source_node << ' '
               << static_cast<unsigned>(access.primitive) << ' '
               << std::quoted(access.semantic_type) << ' ';
        detail::write_optional_id(output, access.area);
        output << ' ';
        detail::write_vec3(output, access.position);
        output << '\n';
    }

    output << "ANCHORS " << anchors.size() << '\n';
    for (const auto& anchor : anchors) {
        output << "ANCHOR " << anchor.id.value << ' '
               << static_cast<unsigned>(anchor.medium) << ' ';
        detail::write_vec3(output, anchor.position);
        output << ' ';
        detail::write_optional_id(output, anchor.region);
        output << ' ';
        detail::write_optional_id(output, anchor.access);
        output << ' ' << std::quoted(anchor.label) << '\n';
    }

    output << "LINKS " << links.size() << '\n';
    for (const auto& link : links) {
        output << "LINK " << link.id.value << ' '
               << static_cast<unsigned>(link.primitive) << ' ' << link.start_anchor.value
               << ' ' << link.end_anchor.value << ' '
               << static_cast<unsigned>(link.start_medium) << ' '
               << static_cast<unsigned>(link.end_medium) << ' ' << link.length << ' ';
        write_optional_text(output, link.start_port_identity);
        output << ' ';
        write_optional_text(output, link.end_port_identity);
        output << '\n';
    }

    output << "MEDIA " << medium_transitions.size() << '\n';
    for (const auto& transition : medium_transitions) {
        output << "MEDIA_TRANSITION " << transition.link.value << ' '
               << static_cast<unsigned>(transition.from_medium) << ' '
               << static_cast<unsigned>(transition.to_medium) << ' '
               << static_cast<unsigned>(transition.primitive) << ' ';
        write_optional_text(output, transition.start_port_identity);
        output << ' ';
        write_optional_text(output, transition.end_port_identity);
        output << '\n';
    }

    output << "PROFILES " << agent_profiles.size() << '\n';
    for (const auto& profile : agent_profiles) {
        output << "PROFILE " << std::quoted(profile.profile_identity) << ' '
               << static_cast<unsigned>(profile.locomotion) << ' ';
        detail::write_bounds(output, profile.clearances);
        output << ' ' << profile.primitives.size();
        for (const auto primitive : profile.primitives) {
            output << ' ' << static_cast<unsigned>(primitive);
        }
        output << '\n';
    }

    output << "AREA_BINDINGS " << area_bindings.size() << '\n';
    for (const auto& binding : area_bindings) {
        output << "AREA_BINDING " << binding.region.value << ' '
               << std::quoted(binding.profile_identity) << ' '
               << binding.required_primitives.size();
        for (const auto primitive : binding.required_primitives) {
            output << ' ' << static_cast<unsigned>(primitive);
        }
        output << '\n';
    }

    output << "PATH_BINDINGS " << path_bindings.size() << '\n';
    for (const auto& binding : path_bindings) {
        output << "PATH_BINDING " << binding.route.value << ' '
               << std::quoted(binding.profile_identity) << ' '
               << binding.required_primitives.size();
        for (const auto primitive : binding.required_primitives) {
            output << ' ' << static_cast<unsigned>(primitive);
        }
        output << '\n';
    }

    output << "FOOTPRINTS " << footprints.size() << '\n';
    for (const auto& footprint : footprints) {
        output << "FOOTPRINT " << std::quoted(footprint.grid_identity) << ' '
               << footprint.level << ' ' << footprint.minimum_x << ' '
               << footprint.minimum_y << ' ' << footprint.minimum_z << ' '
               << footprint.maximum_x << ' ' << footprint.maximum_y << ' '
               << footprint.maximum_z << '\n';
    }

    output << "END\n";
    return output.str();
}

core::Result<assets::Sha256Digest> PreparedNavigationEnvelope::canonical_digest() const {
    if (auto result = validate(); !result) {
        return core::Result<assets::Sha256Digest>::failure(result.error());
    }
    const std::string canonical = serialize();
    return core::Result<assets::Sha256Digest>::success(assets::sha256(
        std::span<const std::uint8_t>{
            reinterpret_cast<const std::uint8_t*>(canonical.data()), canonical.size()}));
}

core::Result<PreparedNavigationEnvelope> PreparedNavigationEnvelope::deserialize(
    std::string_view text) {
    if (text.empty() || text.size() > kMaxSerializedBytes) {
        return core::Result<PreparedNavigationEnvelope>::failure(detail::parse_error(
            "navigation envelope text is empty or exceeds its byte bound"));
    }
    std::istringstream input{std::string(text)};
    std::string magic;
    std::uint32_t version = 0U;
    if (!(input >> magic) || !detail::read_integer(input, version) ||
        magic != kMagic || version != kSchemaVersion) {
        return core::Result<PreparedNavigationEnvelope>::failure(detail::parse_error(
            "navigation envelope framing or schema version is invalid"));
    }

    PreparedNavigationEnvelope envelope;
    std::uint64_t source_revision = 0U;
    if (!detail::require_token(input, "HEADER") ||
        !(input >> std::quoted(envelope.source_namespace)) ||
        !detail::read_uint64(input, source_revision) ||
        !detail::read_digest(input, envelope.source_digest)) {
        return core::Result<PreparedNavigationEnvelope>::failure(detail::parse_error(
            "navigation envelope header is malformed"));
    }
    envelope.source_revision = core::Revision{source_revision};
    std::uint64_t scene_root = 0U;
    if (!detail::read_uint64(input, scene_root)) {
        return core::Result<PreparedNavigationEnvelope>::failure(detail::parse_error(
            "navigation envelope scene root is malformed"));
    }
    envelope.scene_root = scene_root == 0U
        ? std::nullopt
        : std::optional<scene::ObjectId>{scene::ObjectId{scene_root}};

    std::size_t count = 0U;
    if (!detail::require_token(input, "REGIONS") ||
        !detail::read_count(input, kMaxRecordsPerKind, count)) {
        return core::Result<PreparedNavigationEnvelope>::failure(detail::parse_error(
            "navigation envelope region header is invalid"));
    }
    for (std::size_t index = 0U; index < count; ++index) {
        SemanticAreaRegion region;
        std::size_t boundary_count = 0U;
        unsigned plane = 0U;
        if (!detail::require_token(input, "REGION") ||
            !read_id(input, region.id) ||
            !read_id(input, region.source_area) ||
            !detail::read_optional_id(input, region.parent_area) ||
            !detail::read_integer(input, plane) ||
            !detail::read_double(input, region.minimum_clearance) ||
            !(input >> std::quoted(region.semantic_type)) ||
            !detail::read_count(input, SemanticAreaRegion::kMaxBoundaryPoints, boundary_count)) {
            return core::Result<PreparedNavigationEnvelope>::failure(detail::parse_error(
                "navigation envelope region record is malformed"));
        }
        region.plane = static_cast<NavigationPlane>(plane);
        region.boundary.reserve(boundary_count);
        for (std::size_t point = 0U; point < boundary_count; ++point) {
            core::Vec3d value;
            if (!detail::read_vec3(input, value)) {
                return core::Result<PreparedNavigationEnvelope>::failure(detail::parse_error(
                    "navigation envelope region boundary point is malformed"));
            }
            region.boundary.push_back(value);
        }
        if (!detail::read_bounds(input, region.bounds)) {
            return core::Result<PreparedNavigationEnvelope>::failure(detail::parse_error(
                "navigation envelope region bounds are malformed"));
        }
        envelope.regions.push_back(std::move(region));
    }

    if (!detail::require_token(input, "ROUTES") ||
        !detail::read_count(input, kMaxRecordsPerKind, count)) {
        return core::Result<PreparedNavigationEnvelope>::failure(detail::parse_error(
            "navigation envelope route header is invalid"));
    }
    for (std::size_t index = 0U; index < count; ++index) {
        SemanticRouteProduct route;
        std::size_t point_count = 0U;
        unsigned plane = 0U;
        if (!detail::require_token(input, "ROUTE") ||
            !read_id(input, route.id) ||
            !read_id(input, route.source_path) ||
            !detail::read_bool(input, route.closed) ||
            !detail::read_double(input, route.width) ||
            !detail::read_integer(input, plane) ||
            !(input >> std::quoted(route.semantic_type)) ||
            !detail::read_count(input, SemanticRouteProduct::kMaxControlPoints, point_count)) {
            return core::Result<PreparedNavigationEnvelope>::failure(detail::parse_error(
                "navigation envelope route record is malformed"));
        }
        route.spine.plane = static_cast<NavigationPlane>(plane);
        route.spine.control_points.reserve(point_count);
        for (std::size_t point = 0U; point < point_count; ++point) {
            core::Vec3d value;
            if (!detail::read_vec3(input, value)) {
                return core::Result<PreparedNavigationEnvelope>::failure(detail::parse_error(
                    "navigation envelope route control point is malformed"));
            }
            route.spine.control_points.push_back(value);
        }
        route.spine.cumulative_distances.reserve(point_count);
        for (std::size_t point = 0U; point < point_count; ++point) {
            double value = 0.0;
            if (!detail::read_double(input, value)) {
                return core::Result<PreparedNavigationEnvelope>::failure(detail::parse_error(
                    "navigation envelope route cumulative distance is malformed"));
            }
            route.spine.cumulative_distances.push_back(value);
        }
        if (!detail::read_optional_id(input, route.start_area) ||
            !detail::read_optional_id(input, route.end_area) ||
            !detail::read_bounds(input, route.bounds)) {
            return core::Result<PreparedNavigationEnvelope>::failure(detail::parse_error(
                "navigation envelope route terminals or bounds are malformed"));
        }
        envelope.routes.push_back(std::move(route));
    }

    if (!detail::require_token(input, "CORRIDORS") ||
        !detail::read_count(input, kMaxRecordsPerKind, count)) {
        return core::Result<PreparedNavigationEnvelope>::failure(detail::parse_error(
            "navigation envelope corridor header is invalid"));
    }
    for (std::size_t index = 0U; index < count; ++index) {
        SemanticCorridorProduct corridor;
        std::size_t point_count = 0U;
        unsigned plane = 0U;
        if (!detail::require_token(input, "CORRIDOR") ||
            !read_id(input, corridor.id) ||
            !read_id(input, corridor.source_path) ||
            !read_id(input, corridor.route) ||
            !detail::read_integer(input, plane) ||
            !detail::read_count(input, SemanticRouteProduct::kMaxControlPoints, point_count)) {
            return core::Result<PreparedNavigationEnvelope>::failure(detail::parse_error(
                "navigation envelope corridor record is malformed"));
        }
        corridor.plane = static_cast<NavigationPlane>(plane);
        corridor.polygon.reserve(point_count);
        for (std::size_t point = 0U; point < point_count; ++point) {
            core::Vec3d value;
            if (!detail::read_vec3(input, value)) {
                return core::Result<PreparedNavigationEnvelope>::failure(detail::parse_error(
                    "navigation envelope corridor polygon point is malformed"));
            }
            corridor.polygon.push_back(value);
        }
        if (!detail::read_bounds(input, corridor.corridor_bounds)) {
            return core::Result<PreparedNavigationEnvelope>::failure(detail::parse_error(
                "navigation envelope corridor bounds are malformed"));
        }
        envelope.corridors.push_back(std::move(corridor));
    }

    if (!detail::require_token(input, "ACCESS") ||
        !detail::read_count(input, kMaxRecordsPerKind, count)) {
        return core::Result<PreparedNavigationEnvelope>::failure(detail::parse_error(
            "navigation envelope access header is invalid"));
    }
    for (std::size_t index = 0U; index < count; ++index) {
        StructureAccessProduct access;
        unsigned primitive = 0U;
        if (!detail::require_token(input, "ACCESS_POINT") ||
            !read_id(input, access.id) ||
            !read_id(input, access.source_graph) ||
            !detail::read_uint64(input, access.source_node) ||
            !detail::read_integer(input, primitive) ||
            !(input >> std::quoted(access.semantic_type)) ||
            !detail::read_optional_id(input, access.area) ||
            !detail::read_vec3(input, access.position)) {
            return core::Result<PreparedNavigationEnvelope>::failure(detail::parse_error(
                "navigation envelope access record is malformed"));
        }
        access.primitive = static_cast<AccessPrimitive>(primitive);
        envelope.access_points.push_back(std::move(access));
    }

    if (!detail::require_token(input, "ANCHORS") ||
        !detail::read_count(input, kMaxRecordsPerKind, count)) {
        return core::Result<PreparedNavigationEnvelope>::failure(detail::parse_error(
            "navigation envelope anchor header is invalid"));
    }
    for (std::size_t index = 0U; index < count; ++index) {
        TraversalAnchor anchor;
        unsigned medium = 0U;
        if (!detail::require_token(input, "ANCHOR") ||
            !read_id(input, anchor.id) ||
            !detail::read_integer(input, medium) ||
            !detail::read_vec3(input, anchor.position) ||
            !detail::read_optional_id(input, anchor.region) ||
            !detail::read_optional_id(input, anchor.access) ||
            !(input >> std::quoted(anchor.label))) {
            return core::Result<PreparedNavigationEnvelope>::failure(detail::parse_error(
                "navigation envelope anchor record is malformed"));
        }
        anchor.medium = static_cast<TraversalMedium>(medium);
        envelope.anchors.push_back(std::move(anchor));
    }

    if (!detail::require_token(input, "LINKS") ||
        !detail::read_count(input, kMaxRecordsPerKind, count)) {
        return core::Result<PreparedNavigationEnvelope>::failure(detail::parse_error(
            "navigation envelope link header is invalid"));
    }
    for (std::size_t index = 0U; index < count; ++index) {
        OffMeshLinkCandidate link;
        unsigned primitive = 0U;
        unsigned start_medium = 0U;
        unsigned end_medium = 0U;
        if (!detail::require_token(input, "LINK") ||
            !read_id(input, link.id) ||
            !detail::read_integer(input, primitive) ||
            !read_id(input, link.start_anchor) ||
            !read_id(input, link.end_anchor) ||
            !detail::read_integer(input, start_medium) ||
            !detail::read_integer(input, end_medium) ||
            !detail::read_double(input, link.length) ||
            !read_optional_text(input, link.start_port_identity,
                                detail::kMaxIdentityStringBytes) ||
            !read_optional_text(input, link.end_port_identity,
                                detail::kMaxIdentityStringBytes)) {
            return core::Result<PreparedNavigationEnvelope>::failure(detail::parse_error(
                "navigation envelope link record is malformed"));
        }
        link.primitive = static_cast<AccessPrimitive>(primitive);
        link.start_medium = static_cast<TraversalMedium>(start_medium);
        link.end_medium = static_cast<TraversalMedium>(end_medium);
        envelope.links.push_back(std::move(link));
    }

    if (!detail::require_token(input, "MEDIA") ||
        !detail::read_count(input, kMaxRecordsPerKind, count)) {
        return core::Result<PreparedNavigationEnvelope>::failure(detail::parse_error(
            "navigation envelope medium-transition header is invalid"));
    }
    for (std::size_t index = 0U; index < count; ++index) {
        MediumTransition transition;
        unsigned from_medium = 0U;
        unsigned to_medium = 0U;
        unsigned primitive = 0U;
        if (!detail::require_token(input, "MEDIA_TRANSITION") ||
            !read_id(input, transition.link) ||
            !detail::read_integer(input, from_medium) ||
            !detail::read_integer(input, to_medium) ||
            !detail::read_integer(input, primitive) ||
            !read_optional_text(input, transition.start_port_identity,
                                detail::kMaxIdentityStringBytes) ||
            !read_optional_text(input, transition.end_port_identity,
                                detail::kMaxIdentityStringBytes)) {
            return core::Result<PreparedNavigationEnvelope>::failure(detail::parse_error(
                "navigation envelope medium-transition record is malformed"));
        }
        transition.from_medium = static_cast<TraversalMedium>(from_medium);
        transition.to_medium = static_cast<TraversalMedium>(to_medium);
        transition.primitive = static_cast<AccessPrimitive>(primitive);
        envelope.medium_transitions.push_back(std::move(transition));
    }

    if (!detail::require_token(input, "PROFILES") ||
        !detail::read_count(input, kMaxRecordsPerKind, count)) {
        return core::Result<PreparedNavigationEnvelope>::failure(detail::parse_error(
            "navigation envelope profile header is invalid"));
    }
    for (std::size_t index = 0U; index < count; ++index) {
        AgentProfile profile;
        unsigned locomotion = 0U;
        std::size_t primitive_count = 0U;
        if (!detail::require_token(input, "PROFILE") ||
            !(input >> std::quoted(profile.profile_identity)) ||
            !detail::read_integer(input, locomotion) ||
            !detail::read_bounds(input, profile.clearances) ||
            !detail::read_count(input, kMaxPrimitivesPerProfile, primitive_count)) {
            return core::Result<PreparedNavigationEnvelope>::failure(detail::parse_error(
                "navigation envelope profile record is malformed"));
        }
        profile.locomotion = static_cast<LocomotionClass>(locomotion);
        profile.primitives.reserve(primitive_count);
        for (std::size_t primitive = 0U; primitive < primitive_count; ++primitive) {
            unsigned value = 0U;
            if (!detail::read_integer(input, value)) {
                return core::Result<PreparedNavigationEnvelope>::failure(detail::parse_error(
                    "navigation envelope profile primitive is malformed"));
            }
            profile.primitives.push_back(static_cast<TraversalPrimitive>(value));
        }
        envelope.agent_profiles.push_back(std::move(profile));
    }

    if (!detail::require_token(input, "AREA_BINDINGS") ||
        !detail::read_count(input, kMaxRecordsPerKind, count)) {
        return core::Result<PreparedNavigationEnvelope>::failure(detail::parse_error(
            "navigation envelope area-binding header is invalid"));
    }
    for (std::size_t index = 0U; index < count; ++index) {
        AgentAreaBinding binding;
        std::size_t primitive_count = 0U;
        if (!detail::require_token(input, "AREA_BINDING") ||
            !read_id(input, binding.region) ||
            !(input >> std::quoted(binding.profile_identity)) ||
            !detail::read_count(input, kMaxPrimitivesPerProfile, primitive_count)) {
            return core::Result<PreparedNavigationEnvelope>::failure(detail::parse_error(
                "navigation envelope area-binding record is malformed"));
        }
        binding.required_primitives.reserve(primitive_count);
        for (std::size_t primitive = 0U; primitive < primitive_count; ++primitive) {
            unsigned value = 0U;
            if (!detail::read_integer(input, value)) {
                return core::Result<PreparedNavigationEnvelope>::failure(detail::parse_error(
                    "navigation envelope area-binding primitive is malformed"));
            }
            binding.required_primitives.push_back(static_cast<TraversalPrimitive>(value));
        }
        envelope.area_bindings.push_back(std::move(binding));
    }

    if (!detail::require_token(input, "PATH_BINDINGS") ||
        !detail::read_count(input, kMaxRecordsPerKind, count)) {
        return core::Result<PreparedNavigationEnvelope>::failure(detail::parse_error(
            "navigation envelope path-binding header is invalid"));
    }
    for (std::size_t index = 0U; index < count; ++index) {
        AgentPathBinding binding;
        std::size_t primitive_count = 0U;
        if (!detail::require_token(input, "PATH_BINDING") ||
            !read_id(input, binding.route) ||
            !(input >> std::quoted(binding.profile_identity)) ||
            !detail::read_count(input, kMaxPrimitivesPerProfile, primitive_count)) {
            return core::Result<PreparedNavigationEnvelope>::failure(detail::parse_error(
                "navigation envelope path-binding record is malformed"));
        }
        binding.required_primitives.reserve(primitive_count);
        for (std::size_t primitive = 0U; primitive < primitive_count; ++primitive) {
            unsigned value = 0U;
            if (!detail::read_integer(input, value)) {
                return core::Result<PreparedNavigationEnvelope>::failure(detail::parse_error(
                    "navigation envelope path-binding primitive is malformed"));
            }
            binding.required_primitives.push_back(static_cast<TraversalPrimitive>(value));
        }
        envelope.path_bindings.push_back(std::move(binding));
    }

    if (!detail::require_token(input, "FOOTPRINTS") ||
        !detail::read_count(input, kMaxRecordsPerKind, count)) {
        return core::Result<PreparedNavigationEnvelope>::failure(detail::parse_error(
            "navigation envelope footprint header is invalid"));
    }
    for (std::size_t index = 0U; index < count; ++index) {
        NavigationTileFootprint footprint;
        if (!detail::require_token(input, "FOOTPRINT") ||
            !(input >> std::quoted(footprint.grid_identity)) ||
            !detail::read_integer(input, footprint.level) ||
            !detail::read_int64(input, footprint.minimum_x) ||
            !detail::read_int64(input, footprint.minimum_y) ||
            !detail::read_int64(input, footprint.minimum_z) ||
            !detail::read_int64(input, footprint.maximum_x) ||
            !detail::read_int64(input, footprint.maximum_y) ||
            !detail::read_int64(input, footprint.maximum_z)) {
            return core::Result<PreparedNavigationEnvelope>::failure(detail::parse_error(
                "navigation envelope footprint record is malformed"));
        }
        envelope.footprints.push_back(std::move(footprint));
    }

    if (!detail::require_token(input, "END")) {
        return core::Result<PreparedNavigationEnvelope>::failure(detail::parse_error(
            "navigation envelope is missing its END record"));
    }
    std::string trailing;
    if (input >> trailing) {
        return core::Result<PreparedNavigationEnvelope>::failure(detail::parse_error(
            "navigation envelope contains trailing unbound data"));
    }
    if (auto result = envelope.validate(); !result) {
        return core::Result<PreparedNavigationEnvelope>::failure(result.error());
    }
    return core::Result<PreparedNavigationEnvelope>::success(std::move(envelope));
}

} // namespace carto::navgraph
