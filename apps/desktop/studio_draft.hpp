#pragma once

#include <carto/application/application.hpp>

#include <optional>
#include <string>

namespace carto::studio {

struct InspectorDraftContext {
    std::uint64_t generation = 0U;
    core::Revision revision;
    scene::ObjectId object;
    geometry::VertexId vertex;

    bool operator==(const InspectorDraftContext&) const = default;
};

// Text widgets retain active edit buffers by ID. Bind those buffers as well as
// numeric values so a focused field cannot follow a new owner/source revision.
[[nodiscard]] inline std::string inspector_draft_widget_id(InspectorDraftContext context) {
    return std::to_string(context.generation) + ":" + std::to_string(context.revision.value()) +
        ":" + std::to_string(context.object.value) + ":" + std::to_string(context.vertex.value);
}

// Presentation state only. A changed source may refresh a clean field, but
// must never silently rebase an operator's edited value onto a newer revision.
template <typename Value>
class InspectorDraft final {
public:
    void observe(InspectorDraftContext current, const Value& source) {
        if (!context_.has_value() || context_->generation != current.generation ||
            context_->object != current.object || context_->vertex != current.vertex) {
            reset(current, source);
        } else if (context_->revision != current.revision) {
            if (dirty_) stale_ = true;
            else reset(current, source);
        }
    }

    void reset(InspectorDraftContext current, const Value& source) {
        context_ = current;
        value_ = source;
        dirty_ = false;
        stale_ = false;
    }
    void clear() noexcept { context_.reset(); dirty_ = stale_ = false; }
    void mark_edited() noexcept { dirty_ = true; }
    [[nodiscard]] Value& value() noexcept { return value_; }
    [[nodiscard]] bool stale() const noexcept { return stale_; }

    [[nodiscard]] core::Result<Value> checked_value(InspectorDraftContext current) const {
        if (!current.object || !context_.has_value() || *context_ != current || stale_) {
            return core::Result<Value>::failure(core::Diagnostic(core::ErrorCode::stale_data,
                "Inspector draft source changed; reload current values before applying."));
        }
        return core::Result<Value>::success(value_);
    }

private:
    std::optional<InspectorDraftContext> context_;
    Value value_{};
    bool dirty_ = false;
    bool stale_ = false;
};

} // namespace carto::studio
