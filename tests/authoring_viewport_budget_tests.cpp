#include <carto/production/authoring_viewport.hpp>
#include <limits>
using namespace carto::production;
static_assert(valid_authoring_native_extent(3840, 2160));
static_assert(valid_authoring_native_extent(2160, 3840));
static_assert(!valid_authoring_native_extent(3840, 2161));
static_assert(!valid_authoring_native_extent(4097, 1));
static_assert(!valid_authoring_native_extent(0, 2160));
static_assert(!valid_authoring_native_extent(std::numeric_limits<unsigned>::max(), std::numeric_limits<unsigned>::max()));
static_assert(kAuthoringNativeMaxPixels <= kAuthoringNativeAttachmentBudget / 128ULL);
static_assert(kAuthoringCpuMaxPixels == 1048576ULL);
int main() { return 0; }
