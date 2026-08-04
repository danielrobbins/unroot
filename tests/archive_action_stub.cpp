#include "actions/archive_action.hpp"
#include "actions/archive_config.hpp"

namespace actions {

int ArchiveAction::perform(const PackConfig&) { return 0; }
int ArchiveAction::perform(const UnpackConfig&) { return 0; }

}  // namespace actions
