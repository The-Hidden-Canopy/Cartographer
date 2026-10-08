#pragma once

#include <carto/core/result.hpp>

#include <compare>
#include <cstdint>
#include <limits>
#include <optional>
#include <utility>
#include <vector>

namespace carto::gpu {

template <typename Tag>
struct Handle {
    std::uint32_t index = 0;
    std::uint32_t generation = 0;

    [[nodiscard]] constexpr explicit operator bool() const noexcept {
        return index != 0U && generation != 0U;
    }
    [[nodiscard]] constexpr auto operator<=>(const Handle&) const noexcept = default;
};

struct BufferTag;
struct TextureTag;
struct SamplerTag;
struct ShaderTag;
struct PipelineTag;
struct MeshTag;
struct MaterialTag;
struct RenderTargetTag;

using BufferHandle = Handle<BufferTag>;
using TextureHandle = Handle<TextureTag>;
using SamplerHandle = Handle<SamplerTag>;
using ShaderHandle = Handle<ShaderTag>;
using PipelineHandle = Handle<PipelineTag>;
using MeshGpuHandle = Handle<MeshTag>;
using MaterialGpuHandle = Handle<MaterialTag>;
using RenderTargetHandle = Handle<RenderTargetTag>;

template <typename Tag, typename Value>
class Registry {
public:
    Registry() { slots_.emplace_back(); }

    [[nodiscard]] core::Result<Handle<Tag>> insert(Value value) {
        std::uint32_t index = 0U;
        if (!free_indices_.empty()) {
            index = free_indices_.back();
            free_indices_.pop_back();
        } else {
            if (slots_.size() >= static_cast<std::size_t>(
                    std::numeric_limits<std::uint32_t>::max())) {
                return core::Result<Handle<Tag>>::failure(core::Diagnostic(
                    core::ErrorCode::invalid_state, "GPU resource handle index space is exhausted"));
            }
            index = static_cast<std::uint32_t>(slots_.size());
            slots_.emplace_back();
        }

        Slot& slot = slots_.at(index);
        slot.value = std::move(value);
        ++active_count_;
        return core::Result<Handle<Tag>>::success(Handle<Tag>{index, slot.generation});
    }

    [[nodiscard]] core::Result<void> remove(Handle<Tag> handle) {
        auto slot = checked_slot(handle);
        if (!slot) {
            return core::Result<void>::failure(slot.error());
        }
        Slot& value = *slot.value();
        value.value.reset();
        --active_count_;
        if (value.generation == std::numeric_limits<std::uint32_t>::max()) {
            value.retired = true;
        } else {
            ++value.generation;
            free_indices_.push_back(handle.index);
        }
        return core::Result<void>::success();
    }

    [[nodiscard]] core::Result<Value*> resolve(Handle<Tag> handle) {
        auto slot = checked_slot(handle);
        if (!slot) {
            return core::Result<Value*>::failure(slot.error());
        }
        return core::Result<Value*>::success(&slot.value()->value.value());
    }

    [[nodiscard]] core::Result<const Value*> resolve(Handle<Tag> handle) const {
        auto slot = checked_slot(handle);
        if (!slot) {
            return core::Result<const Value*>::failure(slot.error());
        }
        return core::Result<const Value*>::success(&slot.value()->value.value());
    }

    [[nodiscard]] std::size_t size() const noexcept { return active_count_; }

private:
    struct Slot {
        std::uint32_t generation = 1U;
        bool retired = false;
        std::optional<Value> value;
    };

    [[nodiscard]] core::Result<Slot*> checked_slot(Handle<Tag> handle) {
        if (!handle || handle.index >= slots_.size()) {
            return core::Result<Slot*>::failure(core::Diagnostic(
                core::ErrorCode::stale_data, "GPU resource handle is stale"));
        }
        Slot& slot = slots_.at(handle.index);
        if (slot.retired || !slot.value.has_value() || slot.generation != handle.generation) {
            return core::Result<Slot*>::failure(core::Diagnostic(
                core::ErrorCode::stale_data, "GPU resource handle is stale"));
        }
        return core::Result<Slot*>::success(&slot);
    }

    [[nodiscard]] core::Result<const Slot*> checked_slot(Handle<Tag> handle) const {
        if (!handle || handle.index >= slots_.size()) {
            return core::Result<const Slot*>::failure(core::Diagnostic(
                core::ErrorCode::stale_data, "GPU resource handle is stale"));
        }
        const Slot& slot = slots_.at(handle.index);
        if (slot.retired || !slot.value.has_value() || slot.generation != handle.generation) {
            return core::Result<const Slot*>::failure(core::Diagnostic(
                core::ErrorCode::stale_data, "GPU resource handle is stale"));
        }
        return core::Result<const Slot*>::success(&slot);
    }

    std::vector<Slot> slots_;
    std::vector<std::uint32_t> free_indices_;
    std::size_t active_count_ = 0U;
};

} // namespace carto::gpu
