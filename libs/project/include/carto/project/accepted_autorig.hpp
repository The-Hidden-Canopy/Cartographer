#pragma once
#include <carto/character/autorig_graph_record.hpp>
#include <carto/character/definition.hpp>
#include <carto/kinematic/limits.hpp>
#include <carto/weights/transfer_record.hpp>
#include <string_view>

namespace carto::project {
class ProjectDocument;
// Historical project source: cold decode checks internal pins and references;
// it does not authenticate the reviewer or establish current mesh provenance.
struct AcceptedAutorigBundle {
    character::AcceptedAutorigGraphRecord reviewed;
    character::RigDefinition rig;
    kinematic::JointLimitSet limits;
    weights::TransferredWeightRecord weights;
};
[[nodiscard]] core::Result<AcceptedAutorigBundle> decode_accepted_autorig_bundle(std::string_view bytes);
// Required before using the historical bundle with the current project: checks
// live source revision, geometry and target index bounds. Source content digest
// remains caller-asserted; this does not authenticate external source bytes.
[[nodiscard]] core::Result<void> validate_accepted_autorig_current(
    const ProjectDocument& document, const AcceptedAutorigBundle& bundle);
}
