#include <carto/eval/cache.hpp>

#include <mutex>
#include <utility>

namespace carto::eval {

core::Result<void> EvaluationCacheValue::validate() const {
    if (output_digest.empty()) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::invalid_argument, "evaluation cache values require an output digest"));
    }
    return core::Result<void>::success();
}

core::Result<void> EvaluationCache::put(
    EvaluationCacheKey key,
    EvaluationCacheValue value) {
    if (!key.node || key.dependency_digest.empty() || key.parameter_revision.exhausted()) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::invalid_argument,
            "evaluation cache keys require a node, dependency digest, and live revision"));
    }
    if (auto result = value.validate(); !result) return result;
    std::unique_lock lock(mutex_);
    values_[std::move(key)] = std::move(value);
    return core::Result<void>::success();
}

std::optional<EvaluationCacheValue> EvaluationCache::find(
    const EvaluationCacheKey& key) const {
    std::shared_lock lock(mutex_);
    const auto iterator = values_.find(key);
    if (iterator == values_.end()) return std::nullopt;
    return iterator->second;
}

std::size_t EvaluationCache::size() const noexcept {
    std::shared_lock lock(mutex_);
    return values_.size();
}

} // namespace carto::eval
