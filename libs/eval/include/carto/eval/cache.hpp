#pragma once

#include <carto/core/revision.hpp>
#include <carto/core/result.hpp>
#include <carto/eval/ids.hpp>

#include <map>
#include <optional>
#include <shared_mutex>
#include <string>

namespace carto::eval {

struct EvaluationCacheKey {
    NodeId node;
    core::Revision parameter_revision;
    std::string dependency_digest;

    [[nodiscard]] auto operator<=>(const EvaluationCacheKey&) const noexcept = default;
};

struct EvaluationCacheValue {
    std::string output_digest;

    [[nodiscard]] core::Result<void> validate() const;
};

class EvaluationCache {
public:
    [[nodiscard]] core::Result<void> put(
        EvaluationCacheKey key,
        EvaluationCacheValue value);
    [[nodiscard]] std::optional<EvaluationCacheValue> find(
        const EvaluationCacheKey& key) const;
    [[nodiscard]] std::size_t size() const noexcept;

private:
    mutable std::shared_mutex mutex_;
    std::map<EvaluationCacheKey, EvaluationCacheValue> values_;
};

} // namespace carto::eval
