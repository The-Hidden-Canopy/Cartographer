#pragma once
#include <carto/character/autorig_graph_record.hpp>
#include <carto/character/definition.hpp>
#include <carto/kinematic/limits.hpp>
#include <carto/weights/transfer_record.hpp>
namespace carto::project::detail {
[[nodiscard]] core::Result<std::string> make_accepted_autorig_bundle(
    const character::AcceptedAutorigGraphRecord&, const character::RigDefinition&,
    const kinematic::JointLimitSet&, const weights::TransferredWeightRecord&);
[[nodiscard]] core::Result<void> validate_accepted_autorig_bundle(std::string_view);
}
