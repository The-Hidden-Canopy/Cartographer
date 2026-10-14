#pragma once

#include <carto/core/result.hpp>

#include <cstddef>

namespace carto::agent {

struct SceneQueryBudget {
    std::size_t max_queries = 64U;
    std::size_t max_objects_examined = 4096U;
    std::size_t max_topology_elements_examined = 16384U;
    std::size_t max_semantic_records_examined = 8192U;
    std::size_t max_result_bytes = 1U * 1024U * 1024U;

    std::size_t queries_used = 0U;
    std::size_t objects_examined = 0U;
    std::size_t topology_elements_examined = 0U;
    std::size_t semantic_records_examined = 0U;
    std::size_t result_bytes = 0U;

    [[nodiscard]] core::Result<void> validate() const;
};

} // namespace carto::agent
