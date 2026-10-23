#include <carto/production_authoring/authoring.hpp>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <iomanip>
#include <istream>
#include <limits>
#include <map>
#include <sstream>
#include <span>
#include <string>
#include <system_error>
#include <type_traits>
#include <utility>

namespace carto::production_authoring {
namespace {

constexpr std::string_view kSculptMagic = "CARTOGRAPHER_SCULPT_STACK";
constexpr std::string_view kGarmentMagic = "CARTOGRAPHER_GARMENT_ASSET";
constexpr std::size_t kMaxTextBytes = 256U;

core::Diagnostic invalid(std::string message) {
    return core::Diagnostic(core::ErrorCode::invalid_argument, std::move(message));
}

core::Diagnostic validation(std::string message) {
    return core::Diagnostic(core::ErrorCode::validation_failed, std::move(message));
}

core::Diagnostic parse_error(std::string message) {
    return core::Diagnostic(core::ErrorCode::validation_failed, std::move(message));
}

core::Diagnostic version_mismatch(std::string message) {
    return core::Diagnostic(core::ErrorCode::version_mismatch, std::move(message));
}

core::Diagnostic stale(std::string message) {
    return core::Diagnostic(core::ErrorCode::stale_data, std::move(message));
}

bool safe_text(std::string_view value, std::size_t maximum, bool allow_empty = false) {
    if ((!allow_empty && value.empty()) || value.size() > maximum) return false;
    return std::all_of(value.begin(), value.end(), [](unsigned char byte) {
        return byte >= 0x20U && byte != 0x7fU && byte != '\t' && byte != '\r' &&
            byte != '\n';
    });
}

assets::Sha256Digest digest_text(std::string_view text) {
    return assets::sha256(std::span<const std::uint8_t>{
        reinterpret_cast<const std::uint8_t*>(text.data()), text.size()});
}

template <typename T>
bool read_unsigned(std::istream& input, T& value) {
    static_assert(std::is_integral_v<T> && std::is_unsigned_v<T>);
    std::string token;
    if (!(input >> token)) return false;
    T parsed = 0U;
    const auto result = std::from_chars(token.data(), token.data() + token.size(), parsed);
    if (result.ec != std::errc{} || result.ptr != token.data() + token.size()) return false;
    value = parsed;
    return true;
}

bool read_count(std::istream& input, std::size_t maximum, std::size_t& value) {
    std::uint64_t encoded = 0U;
    if (!read_unsigned(input, encoded) || encoded > maximum ||
        encoded > std::numeric_limits<std::size_t>::max()) {
        return false;
    }
    value = static_cast<std::size_t>(encoded);
    return true;
}

bool read_digest(std::istream& input, assets::Sha256Digest& value) {
    std::string token;
    if (!(input >> token)) return false;
    const auto parsed = assets::Sha256Digest::from_hex(token);
    if (!parsed) return false;
    value = parsed.value();
    return true;
}

bool require_token(std::istream& input, std::string_view expected) {
    std::string token;
    return static_cast<bool>(input >> token) && token == expected;
}

bool require_end(std::istream& input) {
    std::string token;
    std::string trailing;
    return static_cast<bool>(input >> token) && token == "END" && !(input >> trailing);
}

bool read_optional_digest(std::istream& input, std::optional<assets::Sha256Digest>& value) {
    std::uint64_t present = 0U;
    if (!read_unsigned(input, present) || present > 1U) return false;
    if (present == 0U) {
        value.reset();
        return true;
    }
    assets::Sha256Digest digest;
    if (!read_digest(input, digest)) return false;
    value = digest;
    return true;
}

void write_optional_digest(
    std::ostringstream& output,
    const std::optional<assets::Sha256Digest>& value) {
    output << (value.has_value() ? 1U : 0U);
    if (value.has_value()) output << ' ' << value->hex();
}

template <typename Tag>
bool read_optional_id(std::istream& input, std::optional<Id<Tag>>& value) {
    std::uint64_t encoded = 0U;
    if (!read_unsigned(input, encoded)) return false;
    if (encoded == 0U) {
        value.reset();
    } else {
        value = Id<Tag>{encoded};
    }
    return true;
}

template <typename Tag>
void write_optional_id(std::ostringstream& output, const std::optional<Id<Tag>>& value) {
    output << (value.has_value() ? value->value : 0U);
}

template <typename IdType>
bool sorted_unique_ids(const std::vector<IdType>& values) {
    return std::all_of(values.begin(), values.end(), [](const IdType& value) {
               return static_cast<bool>(value);
           }) &&
        std::is_sorted(values.begin(), values.end()) &&
        std::adjacent_find(values.begin(), values.end()) == values.end();
}

template <typename IdType>
bool parent_cycle(const std::map<IdType, std::optional<IdType>>& parents) {
    std::map<IdType, std::uint8_t> state;
    for (const auto& [start, ignored] : parents) {
        static_cast<void>(ignored);
        if (state[start] == 2U) continue;
        std::vector<IdType> path;
        IdType current = start;
        while (true) {
            if (state[current] == 1U) return true;
            if (state[current] == 2U) break;
            state[current] = 1U;
            path.push_back(current);
            const auto found = parents.find(current);
            if (found == parents.end() || !found->second.has_value()) break;
            current = *found->second;
        }
        for (const auto& id : path) state[id] = 2U;
    }
    return false;
}

bool finite_nonnegative(double value) {
    return std::isfinite(value) && value >= 0.0;
}

bool finite_positive(double value) {
    return std::isfinite(value) && value > 0.0;
}

bool valid_sculpt_layer_kind(SculptLayerKind value) {
    return sculpt_layer_kind_name(value) != "unknown";
}

bool valid_brush_kind(BrushKind value) {
    return brush_kind_name(value) != "unknown";
}

bool valid_symmetry_axis(SymmetryAxis value) {
    return symmetry_axis_name(value) != "unknown";
}

bool valid_cloth_representation_kind(ClothRepresentationKind value) {
    return cloth_representation_kind_name(value) != "unknown";
}

bool valid_cloth_constraint_kind(ClothConstraintKind value) {
    return cloth_constraint_kind_name(value) != "unknown";
}

bool valid_drape_evidence_source(DrapeEvidenceSource value) {
    return drape_evidence_source_name(value) != "unknown";
}

template <typename IdType>
void write_id_list(std::ostringstream& output, const std::vector<IdType>& values) {
    output << values.size();
    for (const auto& value : values) output << ' ' << value.value;
}

} // namespace

std::string_view sculpt_layer_kind_name(SculptLayerKind value) noexcept {
    switch (value) {
    case SculptLayerKind::form: return "form";
    case SculptLayerKind::secondary_form: return "secondary-form";
    case SculptLayerKind::damage: return "damage";
    case SculptLayerKind::weathering: return "weathering";
    case SculptLayerKind::microdetail: return "microdetail";
    }
    return "unknown";
}

core::Result<SculptLayerKind> sculpt_layer_kind_from_name(std::string_view value) {
    for (const auto candidate : {SculptLayerKind::form, SculptLayerKind::secondary_form,
             SculptLayerKind::damage, SculptLayerKind::weathering,
             SculptLayerKind::microdetail}) {
        if (sculpt_layer_kind_name(candidate) == value) {
            return core::Result<SculptLayerKind>::success(candidate);
        }
    }
    return core::Result<SculptLayerKind>::failure(invalid("sculpt layer kind is unsupported"));
}

std::string_view brush_kind_name(BrushKind value) noexcept {
    switch (value) {
    case BrushKind::grab: return "grab";
    case BrushKind::clay: return "clay";
    case BrushKind::smooth: return "smooth";
    case BrushKind::crease: return "crease";
    case BrushKind::flatten: return "flatten";
    case BrushKind::polish: return "polish";
    }
    return "unknown";
}

core::Result<BrushKind> brush_kind_from_name(std::string_view value) {
    for (const auto candidate : {BrushKind::grab, BrushKind::clay, BrushKind::smooth,
             BrushKind::crease, BrushKind::flatten, BrushKind::polish}) {
        if (brush_kind_name(candidate) == value) return core::Result<BrushKind>::success(candidate);
    }
    return core::Result<BrushKind>::failure(invalid("sculpt brush kind is unsupported"));
}

std::string_view symmetry_axis_name(SymmetryAxis value) noexcept {
    switch (value) {
    case SymmetryAxis::none: return "none";
    case SymmetryAxis::x: return "x";
    case SymmetryAxis::y: return "y";
    case SymmetryAxis::z: return "z";
    }
    return "unknown";
}

core::Result<SymmetryAxis> symmetry_axis_from_name(std::string_view value) {
    for (const auto candidate : {SymmetryAxis::none, SymmetryAxis::x,
             SymmetryAxis::y, SymmetryAxis::z}) {
        if (symmetry_axis_name(candidate) == value) return core::Result<SymmetryAxis>::success(candidate);
    }
    return core::Result<SymmetryAxis>::failure(invalid("symmetry axis is unsupported"));
}

std::string_view cloth_representation_kind_name(ClothRepresentationKind value) noexcept {
    switch (value) {
    case ClothRepresentationKind::baked: return "baked";
    case ClothRepresentationKind::skinned: return "skinned";
    case ClothRepresentationKind::live: return "live";
    }
    return "unknown";
}

core::Result<ClothRepresentationKind> cloth_representation_kind_from_name(
    std::string_view value) {
    for (const auto candidate : {ClothRepresentationKind::baked,
             ClothRepresentationKind::skinned, ClothRepresentationKind::live}) {
        if (cloth_representation_kind_name(candidate) == value) {
            return core::Result<ClothRepresentationKind>::success(candidate);
        }
    }
    return core::Result<ClothRepresentationKind>::failure(
        invalid("cloth representation kind is unsupported"));
}

std::string_view cloth_constraint_kind_name(ClothConstraintKind value) noexcept {
    switch (value) {
    case ClothConstraintKind::stretch: return "stretch";
    case ClothConstraintKind::shear: return "shear";
    case ClothConstraintKind::bend: return "bend";
    case ClothConstraintKind::pin: return "pin";
    case ClothConstraintKind::body_collision: return "body-collision";
    case ClothConstraintKind::self_collision: return "self-collision";
    }
    return "unknown";
}

core::Result<ClothConstraintKind> cloth_constraint_kind_from_name(std::string_view value) {
    for (const auto candidate : {ClothConstraintKind::stretch, ClothConstraintKind::shear,
             ClothConstraintKind::bend, ClothConstraintKind::pin,
             ClothConstraintKind::body_collision, ClothConstraintKind::self_collision}) {
        if (cloth_constraint_kind_name(candidate) == value) {
            return core::Result<ClothConstraintKind>::success(candidate);
        }
    }
    return core::Result<ClothConstraintKind>::failure(
        invalid("cloth constraint kind is unsupported"));
}

std::string_view drape_evidence_source_name(DrapeEvidenceSource value) noexcept {
    switch (value) {
    case DrapeEvidenceSource::cpu_reference: return "cpu-reference";
    case DrapeEvidenceSource::native_runtime: return "native-runtime";
    }
    return "unknown";
}

core::Result<DrapeEvidenceSource> drape_evidence_source_from_name(
    std::string_view value) {
    for (const auto candidate : {DrapeEvidenceSource::cpu_reference,
             DrapeEvidenceSource::native_runtime}) {
        if (drape_evidence_source_name(candidate) == value) {
            return core::Result<DrapeEvidenceSource>::success(candidate);
        }
    }
    return core::Result<DrapeEvidenceSource>::failure(
        invalid("drape evidence source is unsupported"));
}

core::Result<void> SculptSample::validate() const {
    if (!position.finite() || !finite_positive(radius) || !std::isfinite(strength) ||
        strength < -1.0 || strength > 1.0 || surface_element == 0U ||
        !safe_text(surface_domain, kMaxSemanticBytes)) {
        return core::Result<void>::failure(validation(
            "sculpt sample is non-finite or outside its bounded surface contract"));
    }
    return core::Result<void>::success();
}

core::Result<void> SculptStroke::validate() const {
    if (!id || !valid_brush_kind(brush) || !valid_symmetry_axis(symmetry) ||
        !std::isfinite(falloff) || falloff < 0.0 || falloff > 1.0 || samples.empty() ||
        samples.size() > kMaxSamples) {
        return core::Result<void>::failure(validation("sculpt stroke is malformed"));
    }
    if (mask_digest.has_value() && mask_digest->is_zero()) {
        return core::Result<void>::failure(validation("sculpt stroke mask digest is zero"));
    }
    for (const auto& sample : samples) {
        if (auto result = sample.validate(); !result) return result;
    }
    return core::Result<void>::success();
}

core::Result<void> SculptLayer::validate() const {
    if (!id || !valid_sculpt_layer_kind(kind) ||
        (parent.has_value() && (!*parent || *parent == id)) || settings_digest.is_zero() ||
        strokes.empty() || strokes.size() > kMaxStrokes) {
        return core::Result<void>::failure(validation("sculpt layer is malformed"));
    }
    if (displacement_digest.has_value() && displacement_digest->is_zero()) {
        return core::Result<void>::failure(validation("sculpt displacement digest is zero"));
    }
    std::vector<SculptStrokeId> ids;
    ids.reserve(strokes.size());
    for (const auto& stroke : strokes) {
        if (auto result = stroke.validate(); !result) return result;
        ids.push_back(stroke.id);
    }
    if (!sorted_unique_ids(ids)) {
        return core::Result<void>::failure(validation(
            "sculpt strokes must have sorted, unique identities"));
    }
    return core::Result<void>::success();
}

core::Result<void> SculptPublication::validate() const {
    if (auto result = source.validate(); !result) return result;
    if (mesh_asset == 0U || stack_digest.is_zero() || product_digest.is_zero() ||
        topology_lineage_digest.is_zero() || !safe_text(target_profile, kMaxTextBytes) ||
        receipt_digest.is_zero() || receipt_digest != digest_text(canonical_without_receipt())) {
        return core::Result<void>::failure(validation("sculpt publication is stale or incomplete"));
    }
    return core::Result<void>::success();
}

std::string SculptPublication::canonical_without_receipt() const {
    std::ostringstream output;
    output << "sculpt-publication-v1{namespace=" << std::quoted(source.source_namespace)
           << ";asset=" << source.asset_id << ";revision=" << source.revision.value()
           << ";source=" << source.digest.hex() << ";mesh=" << mesh_asset
           << ";stack=" << stack_digest.hex() << ";product=" << product_digest.hex()
           << ";lineage=" << topology_lineage_digest.hex() << ";target="
           << std::quoted(target_profile) << '}';
    return output.str();
}

core::Result<void> SculptPublication::refresh_receipt_digest() {
    if (!source.validate() || mesh_asset == 0U || stack_digest.is_zero() ||
        product_digest.is_zero() || topology_lineage_digest.is_zero() ||
        !safe_text(target_profile, kMaxTextBytes)) {
        return core::Result<void>::failure(validation(
            "sculpt publication cannot refresh an incomplete receipt"));
    }
    receipt_digest = digest_text(canonical_without_receipt());
    return core::Result<void>::success();
}

core::Result<void> SculptStack::insert_layer(SculptLayer value) {
    if (auto result = value.validate(); !result) return result;
    if (layers_.size() >= kMaxLayers || layers_.contains(value.id)) {
        return core::Result<void>::failure(validation(
            "sculpt layer capacity is exceeded or identity is duplicated"));
    }
    layers_.emplace(value.id, std::move(value));
    return core::Result<void>::success();
}

core::Result<void> SculptStack::validate() const {
    if (auto result = source.validate(); !result) return result;
    if (mesh_asset == 0U || base_mesh_digest.is_zero() || !valid_symmetry_axis(default_symmetry) ||
        layers_.empty() || layers_.size() > kMaxLayers) {
        return core::Result<void>::failure(validation("sculpt stack source or capacity is invalid"));
    }
    std::map<SculptLayerId, std::optional<SculptLayerId>> parents;
    std::size_t total_strokes = 0U;
    for (const auto& [id, layer] : layers_) {
        if (id != layer.id) {
            return core::Result<void>::failure(validation("sculpt layer map key does not match identity"));
        }
        if (auto result = layer.validate(); !result) return result;
        if (layer.parent.has_value() && !layers_.contains(*layer.parent)) {
            return core::Result<void>::failure(validation("sculpt layer parent is unresolved"));
        }
        parents.emplace(id, layer.parent);
        total_strokes += layer.strokes.size();
        if (total_strokes > kMaxStrokes) {
            return core::Result<void>::failure(validation("sculpt stack exceeds its stroke bound"));
        }
    }
    if (parent_cycle(parents)) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::cycle_detected, "sculpt layer hierarchy contains a cycle"));
    }
    return core::Result<void>::success();
}

std::string SculptStack::serialize() const {
    std::ostringstream output;
    output << kSculptMagic << ' ' << kSchemaVersion << '\n';
    output << "SOURCE " << std::quoted(source.source_namespace) << ' ' << source.asset_id << ' '
           << source.revision.value() << ' ' << source.digest.hex() << '\n';
    output << "MESH " << mesh_asset << ' ' << base_mesh_digest.hex() << '\n';
    output << "SYMMETRY " << symmetry_axis_name(default_symmetry) << '\n';
    output << "LAYERS " << layers_.size() << '\n';
    for (const auto& [ignored, layer] : layers_) {
        static_cast<void>(ignored);
        output << "LAYER " << layer.id.value << ' ' << sculpt_layer_kind_name(layer.kind) << ' ';
        write_optional_id(output, layer.parent);
        output << ' ' << layer.order << ' ' << layer.settings_digest.hex() << ' ';
        write_optional_digest(output, layer.displacement_digest);
        output << ' ' << layer.strokes.size() << '\n';
        for (const auto& stroke : layer.strokes) {
            output << "STROKE " << stroke.id.value << ' ' << brush_kind_name(stroke.brush) << ' '
                   << symmetry_axis_name(stroke.symmetry) << ' ' << std::setprecision(17)
                   << stroke.falloff << ' ';
            write_optional_digest(output, stroke.mask_digest);
            output << ' ' << stroke.samples.size() << '\n';
            for (const auto& sample : stroke.samples) {
                output << "SAMPLE " << std::setprecision(17) << sample.position.x << ' '
                       << sample.position.y << ' ' << sample.position.z << ' ' << sample.radius
                       << ' ' << sample.strength << ' ' << sample.surface_element << ' '
                       << std::quoted(sample.surface_domain) << '\n';
            }
        }
    }
    output << "END\n";
    return output.str();
}

core::Result<assets::Sha256Digest> SculptStack::canonical_digest() const {
    if (auto result = validate(); !result) {
        return core::Result<assets::Sha256Digest>::failure(result.error());
    }
    return core::Result<assets::Sha256Digest>::success(digest_text(serialize()));
}

core::Result<SculptStack> SculptStack::deserialize(std::string_view text) {
    if (text.empty() || text.size() > kMaxSerializedBytes) {
        return core::Result<SculptStack>::failure(parse_error(
            "sculpt stack text is empty or exceeds its byte bound"));
    }
    std::istringstream input{std::string(text)};
    std::string magic;
    std::uint32_t version = 0U;
    if (!(input >> magic) || magic != kSculptMagic || !read_unsigned(input, version)) {
        return core::Result<SculptStack>::failure(parse_error("sculpt stack framing is malformed"));
    }
    if (version != kSchemaVersion) {
        return core::Result<SculptStack>::failure(version_mismatch(
            "sculpt stack schema version is unsupported"));
    }
    SculptStack stack;
    std::uint64_t revision = 0U;
    if (!require_token(input, "SOURCE") || !(input >> std::quoted(stack.source.source_namespace)) ||
        !read_unsigned(input, stack.source.asset_id) || !read_unsigned(input, revision) ||
        !read_digest(input, stack.source.digest) || !require_token(input, "MESH") ||
        !read_unsigned(input, stack.mesh_asset) || !read_digest(input, stack.base_mesh_digest) ||
        !require_token(input, "SYMMETRY")) {
        return core::Result<SculptStack>::failure(parse_error("sculpt stack source is malformed"));
    }
    stack.source.revision = core::Revision{revision};
    std::string symmetry;
    if (!(input >> symmetry)) return core::Result<SculptStack>::failure(parse_error("sculpt symmetry is malformed"));
    const auto parsed_symmetry = symmetry_axis_from_name(symmetry);
    if (!parsed_symmetry) return core::Result<SculptStack>::failure(parsed_symmetry.error());
    stack.default_symmetry = parsed_symmetry.value();

    std::size_t layer_count = 0U;
    if (!require_token(input, "LAYERS") || !read_count(input, kMaxLayers, layer_count)) {
        return core::Result<SculptStack>::failure(parse_error("sculpt layer count is malformed"));
    }
    for (std::size_t layer_index = 0U; layer_index < layer_count; ++layer_index) {
        SculptLayer layer;
        std::string kind;
        std::size_t stroke_count = 0U;
        if (!require_token(input, "LAYER") || !read_unsigned(input, layer.id.value) ||
            !(input >> kind) || !read_optional_id(input, layer.parent) ||
            !read_unsigned(input, layer.order) || !read_digest(input, layer.settings_digest) ||
            !read_optional_digest(input, layer.displacement_digest) ||
            !read_count(input, SculptLayer::kMaxStrokes, stroke_count)) {
            return core::Result<SculptStack>::failure(parse_error("sculpt layer is malformed"));
        }
        const auto parsed_kind = sculpt_layer_kind_from_name(kind);
        if (!parsed_kind) return core::Result<SculptStack>::failure(parsed_kind.error());
        layer.kind = parsed_kind.value();
        layer.strokes.reserve(stroke_count);
        for (std::size_t stroke_index = 0U; stroke_index < stroke_count; ++stroke_index) {
            SculptStroke stroke;
            std::string brush;
            std::string stroke_symmetry;
            std::size_t sample_count = 0U;
            if (!require_token(input, "STROKE") || !read_unsigned(input, stroke.id.value) ||
                !(input >> brush >> stroke_symmetry) || !(input >> stroke.falloff) ||
                !read_optional_digest(input, stroke.mask_digest) ||
                !read_count(input, SculptStroke::kMaxSamples, sample_count)) {
                return core::Result<SculptStack>::failure(parse_error("sculpt stroke is malformed"));
            }
            const auto parsed_brush = brush_kind_from_name(brush);
            const auto parsed_stroke_symmetry = symmetry_axis_from_name(stroke_symmetry);
            if (!parsed_brush || !parsed_stroke_symmetry) {
                return core::Result<SculptStack>::failure(parse_error("sculpt stroke enum is malformed"));
            }
            stroke.brush = parsed_brush.value();
            stroke.symmetry = parsed_stroke_symmetry.value();
            stroke.samples.reserve(sample_count);
            for (std::size_t sample_index = 0U; sample_index < sample_count; ++sample_index) {
                SculptSample sample;
                if (!require_token(input, "SAMPLE") || !(input >> sample.position.x) ||
                    !(input >> sample.position.y) || !(input >> sample.position.z) ||
                    !(input >> sample.radius) || !(input >> sample.strength) ||
                    !read_unsigned(input, sample.surface_element) ||
                    !(input >> std::quoted(sample.surface_domain))) {
                    return core::Result<SculptStack>::failure(parse_error("sculpt sample is malformed"));
                }
                stroke.samples.push_back(std::move(sample));
            }
            layer.strokes.push_back(std::move(stroke));
        }
        if (auto result = stack.insert_layer(std::move(layer)); !result) {
            return core::Result<SculptStack>::failure(result.error());
        }
    }
    if (!require_end(input)) {
        return core::Result<SculptStack>::failure(parse_error(
            "sculpt stack is missing END or contains trailing data"));
    }
    if (auto result = stack.validate(); !result) {
        return core::Result<SculptStack>::failure(result.error());
    }
    return core::Result<SculptStack>::success(std::move(stack));
}

core::Result<void> FabricIntent::validate() const {
    if (!id || !safe_text(family, kMaxFamilyBytes) || weave_field_digest.is_zero() ||
        !finite_positive(areal_density) || !finite_positive(thickness) ||
        !finite_positive(stretch_stiffness) || !finite_positive(shear_stiffness) ||
        !finite_positive(bend_stiffness) || !finite_nonnegative(friction) ||
        !finite_nonnegative(damping)) {
        return core::Result<void>::failure(validation("fabric intent is malformed"));
    }
    return core::Result<void>::success();
}

core::Result<void> GarmentPanel::validate() const {
    if (!id || !safe_text(semantic_type, kMaxTextBytes) || outline_digest.is_zero() ||
        rest_shape_digest.is_zero() || !fabric || boundary_edges < 3U) {
        return core::Result<void>::failure(validation("garment panel is malformed"));
    }
    return core::Result<void>::success();
}

core::Result<void> GarmentSeam::validate() const {
    if (!id || !first_panel || !second_panel || first_panel == second_panel ||
        first_edge == 0U || second_edge == 0U || !finite_positive(rest_length) ||
        construction_digest.is_zero()) {
        return core::Result<void>::failure(validation("garment seam is malformed"));
    }
    return core::Result<void>::success();
}

core::Result<void> GarmentPin::validate() const {
    if (!id || !panel || !body_structure || panel_vertex == 0U ||
        !std::isfinite(weight) || weight < 0.0 || weight > 1.0 ||
        attachment_digest.is_zero()) {
        return core::Result<void>::failure(validation("garment pin is malformed"));
    }
    if (auto result = body_source.validate(); !result) return result;
    return core::Result<void>::success();
}

core::Result<void> ClothConstraint::validate() const {
    if (!id || !valid_cloth_constraint_kind(kind) || !finite_nonnegative(compliance) ||
        !finite_positive(limit) || !finite_nonnegative(thickness)) {
        return core::Result<void>::failure(validation("cloth constraint is malformed"));
    }
    return core::Result<void>::success();
}

core::Result<void> ClothRepresentation::validate() const {
    if (!valid_cloth_representation_kind(kind) || product_digest.is_zero() ||
        rest_pose_digest.is_zero() || lineage_digest.is_zero() || resident_bytes == 0U) {
        return core::Result<void>::failure(validation("cloth representation is malformed"));
    }
    return core::Result<void>::success();
}

core::Result<void> DrapeRequest::validate() const {
    if (auto result = source.validate(); !result) return result;
    if (garment_digest.is_zero() || iterations == 0U || iterations > 1'000'000U ||
        !finite_positive(time_step) || time_step > 1.0 ||
        !finite_positive(convergence_tolerance) || stable_seed == 0U) {
        return core::Result<void>::failure(validation("drape request is malformed"));
    }
    return core::Result<void>::success();
}

std::string DrapeRequest::canonical() const {
    std::ostringstream output;
    output << "drape-request-v1{namespace=" << std::quoted(source.source_namespace)
           << ";asset=" << source.asset_id << ";revision=" << source.revision.value()
           << ";source=" << source.digest.hex() << ";garment=" << garment_digest.hex()
           << ";iterations=" << iterations << ";dt=" << std::setprecision(17) << time_step
           << ";tolerance=" << convergence_tolerance << ";seed=" << stable_seed << '}';
    return output.str();
}

core::Result<assets::Sha256Digest> DrapeRequest::canonical_digest() const {
    if (auto result = validate(); !result) {
        return core::Result<assets::Sha256Digest>::failure(result.error());
    }
    return core::Result<assets::Sha256Digest>::success(digest_text(canonical()));
}

core::Result<void> DrapeReceipt::validate() const {
    if (auto result = source.validate(); !result) return result;
    if (request_digest.is_zero() || output_digest.is_zero() ||
        !valid_drape_evidence_source(evidence_source) || iterations == 0U ||
        !finite_nonnegative(maximum_strain) || !finite_nonnegative(maximum_shear) ||
        !finite_nonnegative(maximum_bend)) {
        return core::Result<void>::failure(validation("drape receipt is malformed"));
    }
    return core::Result<void>::success();
}

core::Result<void> GarmentAsset::insert_fabric(FabricIntent value) {
    if (auto result = value.validate(); !result) return result;
    if (fabrics_.size() >= kMaxFabrics || fabrics_.contains(value.id)) {
        return core::Result<void>::failure(validation("fabric intent capacity or identity is invalid"));
    }
    fabrics_.emplace(value.id, std::move(value));
    return core::Result<void>::success();
}

core::Result<void> GarmentAsset::insert_panel(GarmentPanel value) {
    if (auto result = value.validate(); !result) return result;
    if (panels_.size() >= kMaxPanels || panels_.contains(value.id)) {
        return core::Result<void>::failure(validation("garment panel capacity or identity is invalid"));
    }
    panels_.emplace(value.id, std::move(value));
    return core::Result<void>::success();
}

core::Result<void> GarmentAsset::insert_seam(GarmentSeam value) {
    if (auto result = value.validate(); !result) return result;
    if (seams_.size() >= kMaxSeams || seams_.contains(value.id)) {
        return core::Result<void>::failure(validation("garment seam capacity or identity is invalid"));
    }
    seams_.emplace(value.id, std::move(value));
    return core::Result<void>::success();
}

core::Result<void> GarmentAsset::insert_pin(GarmentPin value) {
    if (auto result = value.validate(); !result) return result;
    if (pins_.size() >= kMaxPins || pins_.contains(value.id)) {
        return core::Result<void>::failure(validation("garment pin capacity or identity is invalid"));
    }
    pins_.emplace(value.id, std::move(value));
    return core::Result<void>::success();
}

core::Result<void> GarmentAsset::insert_constraint(ClothConstraint value) {
    if (auto result = value.validate(); !result) return result;
    if (constraints_.size() >= kMaxConstraints || constraints_.contains(value.id)) {
        return core::Result<void>::failure(validation("cloth constraint capacity or identity is invalid"));
    }
    constraints_.emplace(value.id, std::move(value));
    return core::Result<void>::success();
}

core::Result<void> GarmentAsset::add_representation(ClothRepresentation value) {
    if (auto result = value.validate(); !result) return result;
    if (representations_.size() >= kMaxRepresentations ||
        std::any_of(representations_.begin(), representations_.end(),
                    [&](const auto& entry) { return entry.kind == value.kind; })) {
        return core::Result<void>::failure(validation(
            "cloth representation capacity or kind is duplicated"));
    }
    representations_.push_back(std::move(value));
    return core::Result<void>::success();
}

core::Result<void> GarmentAsset::validate() const {
    if (auto result = source.validate(); !result) return result;
    if (!safe_text(semantic_name, kMaxTextBytes) || fabrics_.empty() || panels_.empty() ||
        fabrics_.size() > kMaxFabrics || panels_.size() > kMaxPanels ||
        seams_.size() > kMaxSeams || pins_.size() > kMaxPins ||
        constraints_.size() > kMaxConstraints || representations_.empty() ||
        representations_.size() > kMaxRepresentations) {
        return core::Result<void>::failure(validation("garment asset source or capacity is invalid"));
    }
    for (const auto& [id, fabric] : fabrics_) {
        if (id != fabric.id) return core::Result<void>::failure(validation("fabric map key mismatch"));
        if (auto result = fabric.validate(); !result) return result;
    }
    for (const auto& [id, panel] : panels_) {
        if (id != panel.id) return core::Result<void>::failure(validation("panel map key mismatch"));
        if (auto result = panel.validate(); !result) return result;
        if (!fabrics_.contains(panel.fabric)) {
            return core::Result<void>::failure(validation("panel fabric reference is unresolved"));
        }
    }
    for (const auto& [id, seam] : seams_) {
        if (id != seam.id) return core::Result<void>::failure(validation("seam map key mismatch"));
        if (auto result = seam.validate(); !result) return result;
        if (!panels_.contains(seam.first_panel) || !panels_.contains(seam.second_panel)) {
            return core::Result<void>::failure(validation("seam panel reference is unresolved"));
        }
        if (seam.first_edge > panels_.at(seam.first_panel).boundary_edges ||
            seam.second_edge > panels_.at(seam.second_panel).boundary_edges) {
            return core::Result<void>::failure(validation("seam edge exceeds panel boundary"));
        }
    }
    std::optional<biological::AuthoringSourceIdentity> body_source;
    for (const auto& [id, pin] : pins_) {
        if (id != pin.id) return core::Result<void>::failure(validation("pin map key mismatch"));
        if (auto result = pin.validate(); !result) return result;
        if (!panels_.contains(pin.panel)) {
            return core::Result<void>::failure(validation("pin panel reference is unresolved"));
        }
        if (body_source.has_value() && *body_source != pin.body_source) {
            return core::Result<void>::failure(validation(
                "garment pins must bind one exact body source identity"));
        }
        body_source = pin.body_source;
    }
    for (const auto& [id, constraint] : constraints_) {
        if (id != constraint.id) return core::Result<void>::failure(validation("constraint map key mismatch"));
        if (auto result = constraint.validate(); !result) return result;
    }
    std::map<ClothRepresentationKind, bool> kinds;
    for (const auto& representation : representations_) {
        if (auto result = representation.validate(); !result) return result;
        if (kinds.contains(representation.kind)) {
            return core::Result<void>::failure(validation("cloth representation kind is duplicated"));
        }
        kinds.emplace(representation.kind, true);
    }
    return core::Result<void>::success();
}

std::string GarmentAsset::serialize() const {
    std::ostringstream output;
    output << kGarmentMagic << ' ' << kSchemaVersion << '\n';
    output << "SOURCE " << std::quoted(source.source_namespace) << ' ' << source.asset_id << ' '
           << source.revision.value() << ' ' << source.digest.hex() << '\n';
    output << "NAME " << std::quoted(semantic_name) << '\n';
    output << "FABRICS " << fabrics_.size() << '\n';
    for (const auto& [ignored, fabric] : fabrics_) {
        static_cast<void>(ignored);
        output << "FABRIC " << fabric.id.value << ' ' << std::quoted(fabric.family) << ' '
               << fabric.weave_field_digest.hex() << ' ' << std::setprecision(17)
               << fabric.areal_density << ' ' << fabric.thickness << ' '
               << fabric.stretch_stiffness << ' ' << fabric.shear_stiffness << ' '
               << fabric.bend_stiffness << ' ' << fabric.friction << ' ' << fabric.damping << '\n';
    }
    output << "PANELS " << panels_.size() << '\n';
    for (const auto& [ignored, panel] : panels_) {
        static_cast<void>(ignored);
        output << "PANEL " << panel.id.value << ' ' << std::quoted(panel.semantic_type) << ' '
               << panel.outline_digest.hex() << ' ' << panel.rest_shape_digest.hex() << ' '
               << panel.fabric.value << ' ' << panel.boundary_edges << '\n';
    }
    output << "SEAMS " << seams_.size() << '\n';
    for (const auto& [ignored, seam] : seams_) {
        static_cast<void>(ignored);
        output << "SEAM " << seam.id.value << ' ' << seam.first_panel.value << ' '
               << seam.second_panel.value << ' ' << seam.first_edge << ' ' << seam.second_edge
               << ' ' << std::setprecision(17) << seam.rest_length << ' '
               << seam.construction_digest.hex() << '\n';
    }
    output << "PINS " << pins_.size() << '\n';
    for (const auto& [ignored, pin] : pins_) {
        static_cast<void>(ignored);
        output << "PIN " << pin.id.value << ' ' << pin.panel.value << ' '
               << std::quoted(pin.body_source.source_namespace) << ' ' << pin.body_source.asset_id
               << ' ' << pin.body_source.revision.value() << ' ' << pin.body_source.digest.hex()
               << ' ' << pin.body_structure.value << ' ' << pin.panel_vertex << ' '
               << std::setprecision(17) << pin.weight << ' ' << pin.attachment_digest.hex() << '\n';
    }
    output << "CONSTRAINTS " << constraints_.size() << '\n';
    for (const auto& [ignored, constraint] : constraints_) {
        static_cast<void>(ignored);
        output << "CONSTRAINT " << constraint.id.value << ' '
               << cloth_constraint_kind_name(constraint.kind) << ' ' << std::setprecision(17)
               << constraint.compliance << ' ' << constraint.limit << ' ' << constraint.thickness
               << '\n';
    }
    output << "REPRESENTATIONS " << representations_.size() << '\n';
    for (const auto& representation : representations_) {
        output << "CLOTH_REP " << cloth_representation_kind_name(representation.kind) << ' '
               << representation.product_digest.hex() << ' ' << representation.rest_pose_digest.hex()
               << ' ' << representation.lineage_digest.hex() << ' '
               << representation.resident_bytes << '\n';
    }
    output << "END\n";
    return output.str();
}

core::Result<assets::Sha256Digest> GarmentAsset::canonical_digest() const {
    if (auto result = validate(); !result) {
        return core::Result<assets::Sha256Digest>::failure(result.error());
    }
    return core::Result<assets::Sha256Digest>::success(digest_text(serialize()));
}

core::Result<GarmentAsset> GarmentAsset::deserialize(std::string_view text) {
    if (text.empty() || text.size() > kMaxSerializedBytes) {
        return core::Result<GarmentAsset>::failure(parse_error(
            "garment asset text is empty or exceeds its byte bound"));
    }
    std::istringstream input{std::string(text)};
    std::string magic;
    std::uint32_t version = 0U;
    if (!(input >> magic) || magic != kGarmentMagic || !read_unsigned(input, version)) {
        return core::Result<GarmentAsset>::failure(parse_error("garment asset framing is malformed"));
    }
    if (version != kSchemaVersion) {
        return core::Result<GarmentAsset>::failure(version_mismatch(
            "garment asset schema version is unsupported"));
    }
    GarmentAsset garment;
    std::uint64_t revision = 0U;
    if (!require_token(input, "SOURCE") || !(input >> std::quoted(garment.source.source_namespace)) ||
        !read_unsigned(input, garment.source.asset_id) || !read_unsigned(input, revision) ||
        !read_digest(input, garment.source.digest) || !require_token(input, "NAME") ||
        !(input >> std::quoted(garment.semantic_name))) {
        return core::Result<GarmentAsset>::failure(parse_error("garment source is malformed"));
    }
    garment.source.revision = core::Revision{revision};
    std::size_t count = 0U;
    if (!require_token(input, "FABRICS") || !read_count(input, kMaxFabrics, count)) {
        return core::Result<GarmentAsset>::failure(parse_error("fabric count is malformed"));
    }
    for (std::size_t index = 0U; index < count; ++index) {
        FabricIntent fabric;
        if (!require_token(input, "FABRIC") || !read_unsigned(input, fabric.id.value) ||
            !(input >> std::quoted(fabric.family)) || !read_digest(input, fabric.weave_field_digest) ||
            !(input >> fabric.areal_density >> fabric.thickness >> fabric.stretch_stiffness >>
              fabric.shear_stiffness >> fabric.bend_stiffness >> fabric.friction >> fabric.damping)) {
            return core::Result<GarmentAsset>::failure(parse_error("fabric record is malformed"));
        }
        if (auto result = garment.insert_fabric(std::move(fabric)); !result) {
            return core::Result<GarmentAsset>::failure(result.error());
        }
    }
    if (!require_token(input, "PANELS") || !read_count(input, kMaxPanels, count)) {
        return core::Result<GarmentAsset>::failure(parse_error("panel count is malformed"));
    }
    for (std::size_t index = 0U; index < count; ++index) {
        GarmentPanel panel;
        if (!require_token(input, "PANEL") || !read_unsigned(input, panel.id.value) ||
            !(input >> std::quoted(panel.semantic_type)) || !read_digest(input, panel.outline_digest) ||
            !read_digest(input, panel.rest_shape_digest) || !read_unsigned(input, panel.fabric.value) ||
            !read_unsigned(input, panel.boundary_edges)) {
            return core::Result<GarmentAsset>::failure(parse_error("panel record is malformed"));
        }
        if (auto result = garment.insert_panel(std::move(panel)); !result) {
            return core::Result<GarmentAsset>::failure(result.error());
        }
    }
    if (!require_token(input, "SEAMS") || !read_count(input, kMaxSeams, count)) {
        return core::Result<GarmentAsset>::failure(parse_error("seam count is malformed"));
    }
    for (std::size_t index = 0U; index < count; ++index) {
        GarmentSeam seam;
        if (!require_token(input, "SEAM") || !read_unsigned(input, seam.id.value) ||
            !read_unsigned(input, seam.first_panel.value) || !read_unsigned(input, seam.second_panel.value) ||
            !read_unsigned(input, seam.first_edge) || !read_unsigned(input, seam.second_edge) ||
            !(input >> seam.rest_length) || !read_digest(input, seam.construction_digest)) {
            return core::Result<GarmentAsset>::failure(parse_error("seam record is malformed"));
        }
        if (auto result = garment.insert_seam(std::move(seam)); !result) {
            return core::Result<GarmentAsset>::failure(result.error());
        }
    }
    if (!require_token(input, "PINS") || !read_count(input, kMaxPins, count)) {
        return core::Result<GarmentAsset>::failure(parse_error("pin count is malformed"));
    }
    for (std::size_t index = 0U; index < count; ++index) {
        GarmentPin pin;
        if (!require_token(input, "PIN") || !read_unsigned(input, pin.id.value) ||
            !read_unsigned(input, pin.panel.value) ||
            !(input >> std::quoted(pin.body_source.source_namespace)) ||
            !read_unsigned(input, pin.body_source.asset_id) ||
            !read_unsigned(input, revision) || !read_digest(input, pin.body_source.digest) ||
            !read_unsigned(input, pin.body_structure.value) || !read_unsigned(input, pin.panel_vertex) ||
            !(input >> pin.weight) || !read_digest(input, pin.attachment_digest)) {
            return core::Result<GarmentAsset>::failure(parse_error("pin record is malformed"));
        }
        pin.body_source.revision = core::Revision{revision};
        if (auto result = garment.insert_pin(std::move(pin)); !result) {
            return core::Result<GarmentAsset>::failure(result.error());
        }
    }
    if (!require_token(input, "CONSTRAINTS") || !read_count(input, kMaxConstraints, count)) {
        return core::Result<GarmentAsset>::failure(parse_error("constraint count is malformed"));
    }
    for (std::size_t index = 0U; index < count; ++index) {
        ClothConstraint constraint;
        std::string kind;
        if (!require_token(input, "CONSTRAINT") || !read_unsigned(input, constraint.id.value) ||
            !(input >> kind) || !(input >> constraint.compliance >> constraint.limit >> constraint.thickness)) {
            return core::Result<GarmentAsset>::failure(parse_error("constraint record is malformed"));
        }
        const auto parsed_kind = cloth_constraint_kind_from_name(kind);
        if (!parsed_kind) return core::Result<GarmentAsset>::failure(parsed_kind.error());
        constraint.kind = parsed_kind.value();
        if (auto result = garment.insert_constraint(std::move(constraint)); !result) {
            return core::Result<GarmentAsset>::failure(result.error());
        }
    }
    if (!require_token(input, "REPRESENTATIONS") ||
        !read_count(input, kMaxRepresentations, count)) {
        return core::Result<GarmentAsset>::failure(parse_error("cloth representation count is malformed"));
    }
    for (std::size_t index = 0U; index < count; ++index) {
        ClothRepresentation representation;
        std::string kind;
        if (!require_token(input, "CLOTH_REP") || !(input >> kind) ||
            !read_digest(input, representation.product_digest) ||
            !read_digest(input, representation.rest_pose_digest) ||
            !read_digest(input, representation.lineage_digest) ||
            !read_unsigned(input, representation.resident_bytes)) {
            return core::Result<GarmentAsset>::failure(parse_error("cloth representation is malformed"));
        }
        const auto parsed_kind = cloth_representation_kind_from_name(kind);
        if (!parsed_kind) return core::Result<GarmentAsset>::failure(parsed_kind.error());
        representation.kind = parsed_kind.value();
        if (auto result = garment.add_representation(std::move(representation)); !result) {
            return core::Result<GarmentAsset>::failure(result.error());
        }
    }
    if (!require_end(input)) {
        return core::Result<GarmentAsset>::failure(parse_error(
            "garment asset is missing END or contains trailing data"));
    }
    if (auto result = garment.validate(); !result) {
        return core::Result<GarmentAsset>::failure(result.error());
    }
    return core::Result<GarmentAsset>::success(std::move(garment));
}

core::Result<void> validate_sculpt_publication(
    const SculptStack& stack,
    const SculptPublication& publication) {
    if (auto result = stack.validate(); !result) return result;
    if (auto result = publication.validate(); !result) return result;
    const auto stack_digest = stack.canonical_digest();
    if (!stack_digest || publication.source != stack.source ||
        publication.mesh_asset != stack.mesh_asset || publication.stack_digest != stack_digest.value()) {
        return core::Result<void>::failure(stale(
            "sculpt publication does not match the current authored stack"));
    }
    return core::Result<void>::success();
}

core::Result<void> validate_drape_receipt(
    const GarmentAsset& garment,
    const DrapeRequest& request,
    const DrapeReceipt& receipt) {
    if (auto result = garment.validate(); !result) return result;
    if (auto result = request.validate(); !result) return result;
    if (auto result = receipt.validate(); !result) return result;
    const auto garment_digest = garment.canonical_digest();
    const auto request_digest = request.canonical_digest();
    if (!garment_digest || !request_digest || request.source != garment.source ||
        request.garment_digest != garment_digest.value() || receipt.source != request.source ||
        receipt.request_digest != request_digest.value() || receipt.iterations != request.iterations) {
        return core::Result<void>::failure(stale(
            "drape receipt does not match the current garment request"));
    }
    if (receipt.converged && std::max({receipt.maximum_strain, receipt.maximum_shear,
                                       receipt.maximum_bend}) > request.convergence_tolerance) {
        return core::Result<void>::failure(validation(
            "converged drape receipt exceeds the requested convergence tolerance"));
    }
    return core::Result<void>::success();
}

} // namespace carto::production_authoring
