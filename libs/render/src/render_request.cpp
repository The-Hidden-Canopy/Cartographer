#include <carto/render/render_request.hpp>

#include <algorithm>
#include <cmath>
#include <set>
#include <utility>

namespace carto::render {

namespace {

core::Diagnostic invalid(std::string message) {
    return core::Diagnostic(core::ErrorCode::invalid_argument, std::move(message));
}

core::Diagnostic validation(std::string message) {
    return core::Diagnostic(core::ErrorCode::validation_failed, std::move(message));
}

} // namespace

core::Result<void> Extent2D::validate() const {
    if (width == 0U || height == 0U) {
        return core::Result<void>::failure(invalid("render extent must be non-zero"));
    }
    return core::Result<void>::success();
}

core::Result<void> RenderQualitySettings::validate() const {
    if (shadow_map_size == 0U || shadow_cascades == 0U || samples == 0U ||
        (anisotropy != 1U && anisotropy != 2U && anisotropy != 4U && anisotropy != 8U &&
         anisotropy != 16U)) {
        return core::Result<void>::failure(
            invalid("render quality settings contain a zero or unsupported value"));
    }
    return core::Result<void>::success();
}

core::Result<RenderQualitySettings> resolve_quality(RenderQualityProfile profile) {
    RenderQualitySettings result;
    switch (profile) {
    case RenderQualityProfile::draft:
        result = {1024U, 2U, 1U, 4U, false, false, false};
        break;
    case RenderQualityProfile::interactive:
        result = {2048U, 4U, 1U, 8U, true, false, false};
        break;
    case RenderQualityProfile::final_fast:
        result = {2048U, 4U, 8U, 8U, true, true, true};
        break;
    case RenderQualityProfile::final_high:
        result = {4096U, 4U, 32U, 16U, true, true, true};
        break;
    case RenderQualityProfile::reference:
        result = {8192U, 4U, 128U, 16U, true, true, true};
        break;
    }
    if (auto validation_result = result.validate(); !validation_result) {
        return core::Result<RenderQualitySettings>::failure(validation_result.error());
    }
    return core::Result<RenderQualitySettings>::success(result);
}

core::Result<RenderQualitySettings> RenderRequest::resolve() const {
    auto resolved = resolve_quality(quality);
    if (!resolved) {
        return resolved;
    }
    if (samples != 0U) {
        resolved.value().samples = samples;
        if (auto result = resolved.value().validate(); !result) {
            return core::Result<RenderQualitySettings>::failure(result.error());
        }
    }
    if (output == OutputFormat::exr && !resolved.value().exr_capable) {
        return core::Result<RenderQualitySettings>::failure(validation(
            "selected render quality profile does not support EXR output"));
    }
    return resolved;
}

core::Result<void> RenderRequest::validate() const {
    if (!camera) {
        return core::Result<void>::failure(invalid("render request requires a camera object"));
    }
    if (auto result = extent.validate(); !result) {
        return result;
    }
    if (auto result = resolve(); !result) {
        return core::Result<void>::failure(result.error());
    }
    std::set<CaptureResource> captures_seen;
    for (const CaptureResource capture : captures) {
        if (!captures_seen.insert(capture).second) {
            return core::Result<void>::failure(
                validation("render request contains duplicate capture resources"));
        }
    }
    if (transparent_background && output == OutputFormat::png) {
        return core::Result<void>::failure(validation(
            "transparent PNG output requires an explicit alpha-preserving policy"));
    }
    return core::Result<void>::success();
}

} // namespace carto::render
