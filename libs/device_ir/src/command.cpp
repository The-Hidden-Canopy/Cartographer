#include <carto/device_ir/command.hpp>

#include <cmath>
#include <limits>
#include <set>
#include <sstream>
#include <type_traits>
#include <utility>

namespace carto::device_ir {

namespace {

using core::Diagnostic;
using core::ErrorCode;

Diagnostic invalid(std::string message) {
    return Diagnostic(ErrorCode::invalid_argument, std::move(message));
}

Diagnostic validation(std::string message) {
    return Diagnostic(ErrorCode::validation_failed, std::move(message));
}

bool valid(ResourceHandle resource) {
    return std::visit([](const auto handle) { return static_cast<bool>(handle); }, resource);
}

bool finite(const std::array<float, 4U>& values) {
    for (const float value : values) {
        if (!std::isfinite(value)) return false;
    }
    return true;
}

template <typename T>
std::string command_name() {
#define CARTO_COMMAND_NAME(type, name) \
    if constexpr (std::is_same_v<T, type>) return name;
    CARTO_COMMAND_NAME(CmdBeginLabel, "begin_label")
    CARTO_COMMAND_NAME(CmdEndLabel, "end_label")
    CARTO_COMMAND_NAME(CmdTransitionResource, "transition_resource")
    CARTO_COMMAND_NAME(CmdUavBarrier, "uav_barrier")
    CARTO_COMMAND_NAME(CmdCopyBuffer, "copy_buffer")
    CARTO_COMMAND_NAME(CmdCopyBufferToTexture, "copy_buffer_to_texture")
    CARTO_COMMAND_NAME(CmdCopyTextureToBuffer, "copy_texture_to_buffer")
    CARTO_COMMAND_NAME(CmdCopyTexture, "copy_texture")
    CARTO_COMMAND_NAME(CmdBlitTexture, "blit_texture")
    CARTO_COMMAND_NAME(CmdClearColor, "clear_color")
    CARTO_COMMAND_NAME(CmdClearDepth, "clear_depth")
    CARTO_COMMAND_NAME(CmdBeginRendering, "begin_rendering")
    CARTO_COMMAND_NAME(CmdEndRendering, "end_rendering")
    CARTO_COMMAND_NAME(CmdSetViewport, "set_viewport")
    CARTO_COMMAND_NAME(CmdSetScissor, "set_scissor")
    CARTO_COMMAND_NAME(CmdBindPipeline, "bind_pipeline")
    CARTO_COMMAND_NAME(CmdBindVertexBuffer, "bind_vertex_buffer")
    CARTO_COMMAND_NAME(CmdBindIndexBuffer, "bind_index_buffer")
    CARTO_COMMAND_NAME(CmdBindResources, "bind_resources")
    CARTO_COMMAND_NAME(CmdPushConstants, "push_constants")
    CARTO_COMMAND_NAME(CmdDraw, "draw")
    CARTO_COMMAND_NAME(CmdDrawIndexed, "draw_indexed")
    CARTO_COMMAND_NAME(CmdDispatch, "dispatch")
    CARTO_COMMAND_NAME(CmdWriteTimestamp, "write_timestamp")
    CARTO_COMMAND_NAME(CmdResolveTimestamps, "resolve_timestamps")
    CARTO_COMMAND_NAME(CmdWaitTimeline, "wait_timeline")
    CARTO_COMMAND_NAME(CmdSignalTimeline, "signal_timeline")
    CARTO_COMMAND_NAME(CmdPresent, "present")
#undef CARTO_COMMAND_NAME
    return "unknown";
}

} // namespace

core::Result<void> DeviceCommandStream::validate() const {
    bool rendering = false;
    std::size_t label_depth = 0U;
    for (const Command& command : commands_) {
        const auto result = std::visit(
            [&rendering, &label_depth](const auto& value) -> core::Result<void> {
                using T = std::decay_t<decltype(value)>;
                if constexpr (std::is_same_v<T, CmdBeginLabel>) {
                    if (value.name.empty()) {
                        return core::Result<void>::failure(invalid(
                            "device command labels must have a name"));
                    }
                    ++label_depth;
                } else if constexpr (std::is_same_v<T, CmdEndLabel>) {
                    if (label_depth == 0U) {
                        return core::Result<void>::failure(validation(
                            "device command stream closes a label that was not opened"));
                    }
                    --label_depth;
                } else if constexpr (std::is_same_v<T, CmdTransitionResource>) {
                    if (!valid(value.resource) || value.before == value.after) {
                        return core::Result<void>::failure(invalid(
                            "resource transitions require a live resource and different states"));
                    }
                } else if constexpr (std::is_same_v<T, CmdUavBarrier>) {
                    if (!valid(value.resource)) {
                        return core::Result<void>::failure(invalid(
                            "UAV barriers require a live resource"));
                    }
                } else if constexpr (std::is_same_v<T, CmdCopyBuffer>) {
                    if (!value.source || !value.destination || value.bytes == 0U) {
                        return core::Result<void>::failure(invalid(
                            "buffer copies require live handles and non-zero bytes"));
                    }
                } else if constexpr (std::is_same_v<T, CmdCopyBufferToTexture>) {
                    if (!value.source || !value.destination) {
                        return core::Result<void>::failure(invalid(
                            "buffer-to-texture copies require live handles"));
                    }
                } else if constexpr (std::is_same_v<T, CmdCopyTextureToBuffer>) {
                    if (!value.source || !value.destination) {
                        return core::Result<void>::failure(invalid(
                            "texture-to-buffer copies require live handles"));
                    }
                } else if constexpr (std::is_same_v<T, CmdCopyTexture> ||
                                     std::is_same_v<T, CmdBlitTexture>) {
                    if (!value.source || !value.destination) {
                        return core::Result<void>::failure(invalid(
                            "texture operations require live handles"));
                    }
                } else if constexpr (std::is_same_v<T, CmdClearColor>) {
                    if (!value.target || !finite(value.color)) {
                        return core::Result<void>::failure(invalid(
                            "color clears require a live target and finite values"));
                    }
                } else if constexpr (std::is_same_v<T, CmdClearDepth>) {
                    if (!value.target || !std::isfinite(value.depth) ||
                        value.depth < 0.0F || value.depth > 1.0F) {
                        return core::Result<void>::failure(invalid(
                            "depth clears require a live target and a depth in range"));
                    }
                } else if constexpr (std::is_same_v<T, CmdBeginRendering>) {
                    if (rendering || !value.color_target ||
                        (value.depth_target.has_value() && !*value.depth_target)) {
                        return core::Result<void>::failure(validation(
                            "rendering scopes must be balanced and use live targets"));
                    }
                    rendering = true;
                } else if constexpr (std::is_same_v<T, CmdEndRendering>) {
                    if (!rendering) {
                        return core::Result<void>::failure(validation(
                            "rendering scope ended without a matching begin"));
                    }
                    rendering = false;
                } else if constexpr (std::is_same_v<T, CmdSetViewport>) {
                    if (!std::isfinite(value.x) || !std::isfinite(value.y) ||
                        !std::isfinite(value.width) || !std::isfinite(value.height) ||
                        !std::isfinite(value.min_depth) || !std::isfinite(value.max_depth) ||
                        value.width <= 0.0F || value.height <= 0.0F ||
                        value.min_depth < 0.0F || value.max_depth > 1.0F ||
                        value.min_depth > value.max_depth) {
                        return core::Result<void>::failure(invalid(
                            "viewport dimensions and depth range are invalid"));
                    }
                } else if constexpr (std::is_same_v<T, CmdSetScissor>) {
                    if (value.width == 0U || value.height == 0U) {
                        return core::Result<void>::failure(invalid(
                            "scissors require a non-zero extent"));
                    }
                } else if constexpr (std::is_same_v<T, CmdBindPipeline>) {
                    if (!value.pipeline) {
                        return core::Result<void>::failure(invalid(
                            "pipeline binding requires a live handle"));
                    }
                } else if constexpr (std::is_same_v<T, CmdBindVertexBuffer>) {
                    if (!value.buffer || value.stride_bytes < 16U) {
                        return core::Result<void>::failure(invalid(
                            "vertex bindings require a live buffer and a four-component stride"));
                    }
                } else if constexpr (std::is_same_v<T, CmdBindIndexBuffer>) {
                    if (!value.buffer) {
                        return core::Result<void>::failure(invalid(
                            "index bindings require a live buffer"));
                    }
                } else if constexpr (std::is_same_v<T, CmdBindResources>) {
                    std::set<std::pair<std::uint32_t, std::uint32_t>> bindings;
                    for (const auto& binding : value.bindings) {
                        if (!valid(binding.resource) ||
                            (binding.sampler.has_value() && !*binding.sampler) ||
                            !bindings.emplace(binding.set, binding.binding).second) {
                            return core::Result<void>::failure(validation(
                                "resource bindings must be live and unique"));
                        }
                    }
                } else if constexpr (std::is_same_v<T, CmdPushConstants>) {
                    if (value.bytes.size() > 256U) {
                        return core::Result<void>::failure(invalid(
                            "push constants exceed the engine safety bound"));
                    }
                } else if constexpr (std::is_same_v<T, CmdDraw>) {
                    if (!rendering || value.vertex_count == 0U || value.instance_count == 0U) {
                        return core::Result<void>::failure(validation(
                            "draw requires a rendering scope and non-zero counts"));
                    }
                } else if constexpr (std::is_same_v<T, CmdDrawIndexed>) {
                    if (!rendering || value.index_count == 0U || value.instance_count == 0U ||
                        value.index_count % 3U != 0U) {
                        return core::Result<void>::failure(validation(
                            "indexed draw requires a rendering scope and triangle-aligned counts"));
                    }
                } else if constexpr (std::is_same_v<T, CmdDispatch>) {
                    if (rendering || value.group_count_x == 0U || value.group_count_y == 0U ||
                        value.group_count_z == 0U) {
                        return core::Result<void>::failure(
                            validation("dispatch requires a non-rendering scope and non-zero group counts"));
                    }
                } else if constexpr (std::is_same_v<T, CmdWriteTimestamp>) {
                    static_cast<void>(value);
                } else if constexpr (std::is_same_v<T, CmdResolveTimestamps>) {
                    if (!value.destination || value.query_count == 0U) {
                        return core::Result<void>::failure(invalid(
                            "timestamp resolution requires a live destination and non-zero count"));
                    }
                } else if constexpr (std::is_same_v<T, CmdWaitTimeline> ||
                                     std::is_same_v<T, CmdSignalTimeline>) {
                    if (value.serial == 0U) {
                        return core::Result<void>::failure(invalid(
                            "timeline commands require a non-zero serial"));
                    }
                } else if constexpr (std::is_same_v<T, CmdPresent>) {
                    if (rendering || !value.source) {
                        return core::Result<void>::failure(validation(
                            "present requires a live source outside a rendering scope"));
                    }
                }
                return core::Result<void>::success();
            },
            command);
        if (!result) return result;
    }
    if (rendering || label_depth != 0U) {
        return core::Result<void>::failure(validation(
            "device command stream contains an unclosed scope"));
    }
    return core::Result<void>::success();
}

std::string DeviceCommandStream::dump() const {
    std::ostringstream output;
    for (std::size_t index = 0U; index < commands_.size(); ++index) {
        output << '[' << index << "] "
               << std::visit([](const auto& command) {
                      return command_name<std::decay_t<decltype(command)>>();
                  }, commands_.at(index))
               << '\n';
    }
    return output.str();
}

} // namespace carto::device_ir
