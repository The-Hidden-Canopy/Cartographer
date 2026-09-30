#pragma once

#include <carto/application/application.hpp>

namespace carto::application::testing {

[[nodiscard]] inline core::Result<DispatchReceipt> dispatch(
    ApplicationSession& session,
    const ApplicationAction& action) {
    return HumanApplicationAccess::dispatch(session, action);
}

} // namespace carto::application::testing
