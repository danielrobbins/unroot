// Unroot command-line entry point.
#include <exception>
#include <iostream>
#include <string>
#include <vector>
#include <unistd.h>

#include "diagnostics.hpp"
#include "util/app_exit.hpp"
#include "util/error_map.hpp"
#include "app_exception.hpp"
#include "app_core.hpp"
#include "debug_setup.hpp"
#include "actions/unified_action_registry.hpp"
#include "actions/parsed_args.hpp"
#include "program_context.hpp"

int main(int argc, char** argv) {
  // Set the custom terminate handler as early as possible.
  std::set_terminate(custom_terminate_handler);
  
  try { 
    // Initialize program context with argv[0]
    program::Context::initialize(argv[0]);
    
    // Store original arguments for logging
    std::vector<std::string> origArgv; 
    origArgv.reserve(argc);
    for (int i = 0; i < argc; ++i) origArgv.emplace_back(argv[i]);
    
    // Check for wrapper mode and run if detected
    app_core::checkAndRunWrapper(argc, argv);
    
    // Setup logging system
    app_core::setupLogging();
    
    // Process debug flags (modifies argc/argv)
    debug_setup::processDebugFlags(argc, argv);
    
    // Setup verbose logging if enabled
    app_core::setupVerboseLogging(origArgv);
    
    // Handle basic cases
    if (argc < 2) { 
      app_core::showUsage(); 
      return 0; 
    }
    
    // Convert argc/argv to structured format immediately - no more raw argc/argv passing!
    auto parsed_args = actions::ToBeParsedArgs::from_argv(argc, argv);
    
    // Handle global help (unroot --help)
    if (parsed_args.is_global_help()) {
      app_core::showUsage();
      return 0;
    }
    
    // At this point we should have an action
    if (!parsed_args.action_name.has_value()) {
      throw AppException(util::make_error(util::LibErr::Invalid, 0, "Action required"), "usage");
    }
    
    std::string action = *parsed_args.action_name;
    
    // Handle version action
    if (app_core::isVersionCommand(action)) { 
      app_core::showVersion(); 
      return 0; 
    }
    
    if (parsed_args.is_action_help()) {
      if (!actions::ActionRegistry::has_action(action)) {
        throw AppException(
            util::make_error(util::LibErr::Invalid, 0,
                             std::string("unknown action: ") + action),
            "usage");
      }
      std::cout << actions::ActionRegistry::get_action(action).help() << std::endl;
      return 0;
    }

    if (actions::ActionRegistry::has_action(action)) {
      return actions::ActionRegistry::execute(action, parsed_args);
    }

    throw AppException(util::make_error(
        util::LibErr::Invalid, 0, std::string("unknown action: ") + action),
        "usage");
  } catch (const std::exception& ex) {
    if (auto ax = dynamic_cast<const AppException*>(&ex)) {
      auto e = ax->err;
      auto exi = app::mapErrorToExit(e);
      std::cerr << ax->what() << "\n";
      return app::toInt(exi);
    }
    std::cerr << "fatal: " << ex.what() << "\n";
    return app::toInt(app::Exit::Unknown);
  } catch (...) {
    std::cerr << "fatal: unknown error\n";
    return app::toInt(app::Exit::Unknown);
  }
}
