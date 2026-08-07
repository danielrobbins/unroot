#include "app_core.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <limits.h>
#include <unistd.h>

#include "util/log.hpp"
#include "util/log_host.hpp"
#include "util/str.hpp"
#include "wrapper.hpp"
#include "../build/version.hpp"

using util::q;

namespace app_core {

const char* getVersion() {
  return UNROOT_VERSION_STRING;
}

void showUsage() {
  std::cout << "Usage:\n";
  std::cout << "  unroot enter ROOT [OPTIONS] [-- COMMAND [ARGUMENTS...]]\n";
  std::cout << "  unroot unpack ARCHIVE ROOT [OPTIONS]\n";
  std::cout << "  unroot pack ROOT ARCHIVE [OPTIONS]\n\n";
  std::cout << "Actions:\n";
  std::cout << "  enter       Enter an initialized or explicitly native root filesystem.\n";
  std::cout << "  pack        Capture a mapped root filesystem as a tar archive.\n";
  std::cout << "  unpack      Create a mapped root filesystem from a tar archive.\n";
  
  std::cout << "\n";
  std::cout << "General Options:\n";
  std::cout << "  --debug       Enable structured diagnostic logging\n";
  std::cout << "  --help, -h    Show this help message\n";
  std::cout << "  --version, -V  Show version information\n";
  std::cout << "\n";
  std::cout << "Use 'unroot <action> --help' for action-specific help.\n";
}

void showVersion() {
  std::cout << getVersion() << "\n";
}

bool isVersionCommand(const std::string& cmd) {
  return cmd == "--version" || cmd == "-V";
}

void checkAndRunWrapper(int argc, char** argv) {
  if (argc > 0) {
    const char* bn = std::strrchr(argv[0], '/');
    bn = bn ? bn + 1 : argv[0];
    if (bn && std::strcmp(bn, "wrapper") == 0) {
      runWrapper(argc, argv);
    }
  }
}

void setupLogging() {
  util::setLogSink([]([[maybe_unused]] util::LogLevel level, const util::StepView& sv){
    const char* v = ::getenv("UNROOT_LOG");
    bool json = (v && std::strcmp(v, "json") == 0);
    if (json) {
      std::fprintf(stderr,
        "{\"step\":\"%s\",\"ok\":%s,\"err\":%d,\"note\":\"%s\"}\n",
        sv.name, sv.ok?"true":"false", sv.err, sv.note?sv.note:"");
    } else {
      std::fprintf(stderr, "[%s] ok=%s err=%d%s%s\n",
        sv.name, sv.ok?"true":"false", sv.err, sv.err?" (":"", sv.err?std::strerror(sv.err):"");
      if (sv.note && *sv.note) std::fprintf(stderr, "  note: %s\n", sv.note);
    }
  });
}

void setupVerboseLogging(const std::vector<std::string>& origArgv) {
  if (util::hostVerboseLevel() >= 1 && util::hostJsonLog()) {
    char cwdbuf[PATH_MAX];
    cwdbuf[0] = '\0';
    if (!::getcwd(cwdbuf, sizeof(cwdbuf))) std::strncpy(cwdbuf, "", sizeof(cwdbuf));
    std::string j = std::string("{\"unroot\":\"cli\",\"cwd\":") + q(cwdbuf) + ",\"argv\":[";
    for (size_t i = 0; i < origArgv.size(); ++i) {
      if (i) j.push_back(',');
      j += q(origArgv[i]);
    }
    j += "]}";
    std::fprintf(stderr, "%s\n", j.c_str());
  }
}

} // namespace app_core
