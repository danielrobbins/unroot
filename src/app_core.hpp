#pragma once

#include <string>
#include <vector>

namespace app_core {
  void setupLogging();
  void setupVerboseLogging(const std::vector<std::string>& origArgv);

  void checkAndRunWrapper(int argc, char** argv);

  const char* getVersion();
  void showUsage();
  void showVersion();

  bool isVersionCommand(const std::string& cmd);
}
