#include <carto/pose/ik_fk_mode_key_record.hpp>

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <locale>
#include <span>
#include <sstream>
#include <utility>

namespace carto::pose {
namespace {
core::Diagnostic invalid(const char* message) {
    return {core::ErrorCode::validation_failed, message};
}
bool token(std::istream& in, std::string_view expected) {
    std::string actual;
    return static_cast<bool>(in >> actual) && actual == expected;
}
bool read_digest(std::istream& in, assets::Sha256Digest& digest) {
    std::string hex;
    if (!(in >> hex)) return false;
    auto decoded = assets::Sha256Digest::from_hex(hex);
    if (!decoded) return false;
    digest = decoded.value();
    return true;
}
void write_time(std::ostream& out, character::RationalTime value) {
    out << value.tick << ' ' << value.ticks_per_second;
}
bool read_time(std::istream& in, character::RationalTime& value) {
    return static_cast<bool>(in >> value.tick >> value.ticks_per_second);
}
bool valid_mode(IkFkMode mode) { return mode == IkFkMode::fk || mode == IkFkMode::ik; }
} // namespace

core::Result<void> IkFkModeKeyRecord::validate() const {
    if (auto r = context.validate(); !r) return r;
    if (auto r = crosswalk.validate(); !r) return r;
    if (!document || !clip || clip_digest.is_zero() || !valid_mode(source_mode) ||
        !valid_mode(target_mode) || source_mode == target_mode ||
        !std::isfinite(mode_before) || !std::isfinite(mode_at_key) || mode_before == mode_at_key ||
        proposed_curve.id != crosswalk.curve ||
        proposed_curve.timebase_ticks_per_second != context.clip_tick.ticks_per_second ||
        proposed_curve.interpolation != motion::InterpolationKind::step)
        return core::Result<void>::failure(invalid("invalid authored mode-key record identity, mode or timebase"));
    if (auto r = proposed_curve.validate(); !r) return r;
    if (auto r = edit.validate(); !r) return r;
    const auto result_digest = proposed_curve.canonical_digest();
    if (!result_digest || edit.kind != motion::CurveEditKind::insert_key ||
        edit.affected_key_count != 1U || edit.source_digest != crosswalk.curve_digest ||
        edit.result_digest != result_digest.value())
        return core::Result<void>::failure(invalid("mode-key edit receipt does not bind the source and result"));
    const auto key = std::find_if(proposed_curve.keys.begin(), proposed_curve.keys.end(),
        [this](const auto& candidate) { return candidate.time.tick == context.clip_tick.tick; });
    if (key == proposed_curve.keys.end() || key->value != mode_at_key ||
        key->tangent_in != 0.0 || key->tangent_out != 0.0)
        return core::Result<void>::failure(invalid("mode-key insertion does not contain the requested key"));
    const character::RationalTime previous{context.clip_tick.tick - 1, context.clip_tick.ticks_per_second};
    if (proposed_curve.keys.front().time.tick > previous.tick)
        return core::Result<void>::failure(invalid("mode-key prior sample has no authored source key"));
    auto prior = proposed_curve.sample(previous);
    auto at = proposed_curve.sample(context.clip_tick);
    if (!prior || !at || prior.value() != mode_before || at.value() != mode_at_key)
        return core::Result<void>::failure(invalid("mode-key curve samples do not match the authored intent"));
    for (const auto& item : proposed_curve.keys)
        if (item.value != mode_before && item.value != mode_at_key)
            return core::Result<void>::failure(invalid("mode-key curve contains a non-discrete value"));
    auto removed = motion::delete_keys(proposed_curve, {context.clip_tick.tick});
    if (!removed) return core::Result<void>::failure(removed.error());
    auto source_digest = removed.value().curve.canonical_digest();
    if (!source_digest || source_digest.value() != crosswalk.curve_digest)
        return core::Result<void>::failure(invalid("mode-key reconstruction does not match the source curve pin"));
    auto inserted = motion::insert_key(removed.value().curve,
        {context.clip_tick, mode_at_key, 0.0, 0.0}, false);
    if (!inserted || inserted.value().curve.serialize() != proposed_curve.serialize() ||
        inserted.value().receipt.serialize() != edit.serialize())
        return core::Result<void>::failure(invalid("mode-key insertion is not canonical or reversible"));
    return core::Result<void>::success();
}

std::string IkFkModeKeyRecord::serialize() const {
    std::ostringstream out;
    out.imbue(std::locale::classic());
    out << std::setprecision(17) << "CARTO_IK_FK_MODE_KEY_INTENT 1\nSOURCE "
        << std::quoted(context.source_namespace) << ' ' << context.source_revision.value() << ' '
        << context.source_digest.hex() << ' ' << context.motion_document_digest.hex()
        << "\nRIG " << context.rig.value << ' ' << context.rig_digest.hex() << ' '
        << context.rest_graph_digest.hex() << ' ' << context.limits_digest.hex() << ' '
        << context.source_pose_digest.hex() << ' ' << context.authoring_digest.hex()
        << "\nSOURCE_POSE_ASSERTION " << static_cast<unsigned>(context.source_pose_assertion)
        << "\nFRAME " << std::quoted(context.reference_frame)
        << "\nSOLVER_INPUT_FRAME " << static_cast<unsigned>(context.solver_input_frame)
        << "\nLINEAGE " << std::quoted(context.lineage.scenario) << ' '
        << std::quoted(context.lineage.branch) << ' ' << std::quoted(context.lineage.session) << ' '
        << (context.lineage.restore ? 1 : 0) << ' '
        << std::quoted(context.lineage.restore.value_or(std::string{})) << "\nCLIP_TIME ";
    write_time(out, context.clip_tick); out << ' '; write_time(out, context.clip_start); out << ' '; write_time(out, context.clip_end);
    out << "\nSOURCE_TIME ";
    write_time(out, context.source_tick); out << ' '; write_time(out, context.source_start); out << ' '; write_time(out, context.source_end);
    out << "\nCHANNEL_TIME ";
    write_time(out, context.channel_tick); out << ' '; write_time(out, context.channel_start); out << ' '; write_time(out, context.channel_end);
    out << "\nCROSSWALK " << static_cast<unsigned>(crosswalk.stable_target.kind) << ' '
        << std::quoted(crosswalk.stable_target.stable_id) << ' ' << crosswalk.channel.value << ' '
        << crosswalk.channel_digest.hex() << ' ' << crosswalk.layer.value << ' ' << crosswalk.curve.value << ' '
        << crosswalk.curve_digest.hex() << ' ' << crosswalk.control.value << ' ' << static_cast<unsigned>(crosswalk.axis)
        << "\nDOCUMENT " << document.value << ' ' << clip.value << ' ' << clip_digest.hex()
        << "\nMODES " << static_cast<unsigned>(source_mode) << ' ' << static_cast<unsigned>(target_mode)
        << ' ' << mode_before << ' ' << mode_at_key
        << "\nCURVE " << std::quoted(proposed_curve.serialize())
        << "\nEDIT " << std::quoted(edit.serialize()) << "\nEND\n";
    return out.str();
}

core::Result<IkFkModeKeyRecord> IkFkModeKeyRecord::deserialize(std::string_view text) {
    using Result = core::Result<IkFkModeKeyRecord>;
    const auto fail = [] { return Result::failure(invalid("malformed or noncanonical mode-key intent record")); };
    if (text.empty() || text.size() > kMaxSerializedBytes) return fail();
    std::istringstream in{std::string(text)};
    in.imbue(std::locale::classic());
    IkFkModeKeyRecord value;
    unsigned version = 0U, target_kind = 0U, axis = 0U, source_mode = 0U, target_mode = 0U, has_restore = 0U;
    unsigned source_pose_assertion = 0U, solver_input_frame = 0U;
    std::uint64_t revision = 0U;
    std::string restore, curve, edit;
    if (!token(in, "CARTO_IK_FK_MODE_KEY_INTENT") || !(in >> version) || version != kSchemaVersion ||
        !token(in, "SOURCE") || !(in >> std::quoted(value.context.source_namespace) >> revision) ||
        !read_digest(in, value.context.source_digest) || !read_digest(in, value.context.motion_document_digest) ||
        !token(in, "RIG") || !(in >> value.context.rig.value) || !read_digest(in, value.context.rig_digest) ||
        !read_digest(in, value.context.rest_graph_digest) || !read_digest(in, value.context.limits_digest) ||
        !read_digest(in, value.context.source_pose_digest) || !read_digest(in, value.context.authoring_digest) ||
        !token(in, "SOURCE_POSE_ASSERTION") || !(in >> source_pose_assertion) || source_pose_assertion > 7U ||
        !token(in, "FRAME") || !(in >> std::quoted(value.context.reference_frame)) ||
        !token(in, "SOLVER_INPUT_FRAME") || !(in >> solver_input_frame) || solver_input_frame > 1U ||
        !token(in, "LINEAGE") || !(in >> std::quoted(value.context.lineage.scenario) >>
            std::quoted(value.context.lineage.branch) >> std::quoted(value.context.lineage.session) >>
            has_restore >> std::quoted(restore)) || has_restore > 1U || (!has_restore && !restore.empty()) ||
        !token(in, "CLIP_TIME") || !read_time(in, value.context.clip_tick) ||
        !read_time(in, value.context.clip_start) || !read_time(in, value.context.clip_end) ||
        !token(in, "SOURCE_TIME") || !read_time(in, value.context.source_tick) ||
        !read_time(in, value.context.source_start) || !read_time(in, value.context.source_end) ||
        !token(in, "CHANNEL_TIME") || !read_time(in, value.context.channel_tick) ||
        !read_time(in, value.context.channel_start) || !read_time(in, value.context.channel_end) ||
        !token(in, "CROSSWALK") || !(in >> target_kind >> std::quoted(value.crosswalk.stable_target.stable_id) >>
            value.crosswalk.channel.value) || !read_digest(in, value.crosswalk.channel_digest) ||
        !(in >> value.crosswalk.layer.value >> value.crosswalk.curve.value) || !read_digest(in, value.crosswalk.curve_digest) ||
        !(in >> value.crosswalk.control.value >> axis) || axis >= character::kAxisCount ||
        target_kind > static_cast<unsigned>(temporal::TargetBindingKind::light) ||
        !token(in, "DOCUMENT") || !(in >> value.document.value >> value.clip.value) || !read_digest(in, value.clip_digest) ||
        !token(in, "MODES") || !(in >> source_mode >> target_mode >> value.mode_before >> value.mode_at_key) ||
        source_mode > 1U || target_mode > 1U ||
        !token(in, "CURVE") || !(in >> std::quoted(curve)) || curve.size() > motion::KeyedCurve::kMaxSerializedBytes ||
        !token(in, "EDIT") || !(in >> std::quoted(edit)) || edit.size() > motion::CurveEditReceipt::kMaxSerializedBytes ||
        !token(in, "END")) return fail();
    value.context.source_revision = core::Revision{revision};
    value.context.source_pose_assertion = static_cast<SourcePoseAssertion>(source_pose_assertion);
    value.context.solver_input_frame = static_cast<SolverInputFrameAssertion>(solver_input_frame);
    if (has_restore) value.context.lineage.restore = std::move(restore);
    value.crosswalk.stable_target.kind = static_cast<temporal::TargetBindingKind>(target_kind);
    value.crosswalk.axis = static_cast<std::uint8_t>(axis);
    value.source_mode = static_cast<IkFkMode>(source_mode);
    value.target_mode = static_cast<IkFkMode>(target_mode);
    auto decoded_curve = motion::KeyedCurve::deserialize(curve);
    auto decoded_edit = motion::CurveEditReceipt::deserialize(edit);
    if (!decoded_curve || !decoded_edit) return fail();
    value.proposed_curve = std::move(decoded_curve.value());
    value.edit = std::move(decoded_edit.value());
    if (!value.validate() || value.serialize() != text) return fail();
    return Result::success(std::move(value));
}

core::Result<assets::Sha256Digest> IkFkModeKeyRecord::canonical_digest() const {
    if (auto r = validate(); !r) return core::Result<assets::Sha256Digest>::failure(r.error());
    const auto bytes = serialize();
    if (bytes.size() > kMaxSerializedBytes)
        return core::Result<assets::Sha256Digest>::failure(invalid("mode-key intent record exceeds its bound"));
    return core::Result<assets::Sha256Digest>::success(assets::sha256(
        std::span<const std::uint8_t>{reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size()}));
}

core::Result<IkFkModeKeyRecord> make_ik_fk_mode_key_record(const IkFkModeKeyIntent& intent) {
    using Result = core::Result<IkFkModeKeyRecord>;
    IkFkModeKeyRecord result{intent.context, intent.crosswalk, intent.document, intent.clip,
        intent.clip_digest, intent.source_mode, intent.target_mode, intent.mode_before,
        intent.mode_at_key, intent.proposed_curve, intent.edit};
    if (auto checked = result.canonical_digest(); !checked) return Result::failure(checked.error());
    return Result::success(std::move(result));
}
} // namespace carto::pose
