#pragma once

#include <carto/core/result.hpp>
#include <carto/gpu/rhi.hpp>

#include <compare>
#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace carto::render_graph {

struct ResourceId {
    std::uint32_t value = 0;

    [[nodiscard]] constexpr explicit operator bool() const noexcept { return value != 0U; }
    [[nodiscard]] constexpr auto operator<=>(const ResourceId&) const noexcept = default;
};

struct PassId {
    std::uint32_t value = 0;

    [[nodiscard]] constexpr explicit operator bool() const noexcept { return value != 0U; }
    [[nodiscard]] constexpr auto operator<=>(const PassId&) const noexcept = default;
};

enum class ResourceKind {
    buffer,
    texture,
};

enum class Access {
    read,
    write,
    read_write,
};

enum class PipelineStage {
    copy,
    compute,
    vertex,
    fragment,
    color_output,
    depth_stencil,
    present,
};

struct ResourceDesc {
    std::string name;
    ResourceKind kind = ResourceKind::buffer;
    bool imported = false;
    std::uint32_t alias_group = 0U;
    std::uint64_t bytes = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t depth = 0;
    std::uint32_t layers = 0;
    std::uint32_t mip_levels = 0;
    gpu::Format format = gpu::Format::unknown;
};

struct ResourceUsage {
    ResourceId resource;
    Access access = Access::read;
    PipelineStage stage = PipelineStage::fragment;
};

struct PassDesc {
    std::string name;
    std::vector<ResourceUsage> usages;
};

struct Barrier {
    ResourceId resource;
    PassId before;
    PassId after;
    Access before_access;
    Access after_access;
    PipelineStage before_stage;
    PipelineStage after_stage;
};

struct ResourceLifetime {
    ResourceId resource;
    std::size_t first_pass = 0U;
    std::size_t last_pass = 0U;
};

struct CompiledGraph {
    std::vector<PassId> pass_order;
    std::vector<Barrier> barriers;
    std::vector<ResourceLifetime> lifetimes;
};

class GraphBuilder {
public:
    [[nodiscard]] core::Result<ResourceId> create_resource(ResourceDesc descriptor);
    [[nodiscard]] core::Result<PassId> add_pass(PassDesc descriptor);
    [[nodiscard]] core::Result<CompiledGraph> compile() const;

    [[nodiscard]] std::size_t resource_count() const noexcept { return resources_.size(); }
    [[nodiscard]] std::size_t pass_count() const noexcept { return passes_.size(); }

private:
    [[nodiscard]] core::Result<void> validate_resource(const ResourceDesc& descriptor) const;
    [[nodiscard]] core::Result<void> validate_usage(const ResourceUsage& usage) const;

    std::map<ResourceId, ResourceDesc> resources_;
    std::map<PassId, PassDesc> passes_;
    std::uint32_t next_resource_id_ = 1U;
    std::uint32_t next_pass_id_ = 1U;
};

} // namespace carto::render_graph
