#pragma once

#include <type_traits>

namespace carto::navgraph {

// Compile-time negative fence for the dynamic traversal-cost boundary.
//
// Static traversability (what geometry exists and which agent classes can use
// it) is Cartographer truth. Dynamic cost, risk, occupancy, and learned
// knowledge are game-owned runtime layers and must never be authored onto
// neutral navigation records. kHasDynamicTraversalCost<T> is an extensible
// marker: the primary template reports false (a record with no dynamic-cost
// member), and an opt-in specialization can be added per record type so the
// cost of accidentally adding such a member is a build failure here, not a
// silent authority leak. The static assertions in envelope.hpp evaluate this
// trait for every public navigation record.
template <typename Record>
struct kHasDynamicTraversalCost : std::false_type {};

} // namespace carto::navgraph
