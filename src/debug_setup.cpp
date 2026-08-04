#include "debug_setup.hpp"
#include <cstdlib>
#include <cstring>

namespace debug_setup {

int processDebugFlags(int& argc, char** argv) {
  if (argc > 1 && std::strcmp(argv[1], "--debug") == 0) {
    ::setenv("UNROOT_VERBOSE", "2", 1);
    ::setenv("UNROOT_LOG", "json", 1);
    ::setenv("UNROOT_WRAPPER_DEBUG", "1", 1);
    
    // Shift arguments to remove --debug flag
    for (int i = 1; i + 1 < argc; ++i) {
      argv[i] = argv[i+1];
    }
    --argc;
    return 1; // Flag was processed
  }
  return 0; // No debug flag found
}

} // namespace debug_setup
