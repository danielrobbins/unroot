#include "doctest.h"

#include "oci.hpp"

TEST_CASE("OCI platforms normalize common Linux architecture names") {
  oci::Platform platform;
  REQUIRE(oci::parsePlatform("linux/x86_64", platform));
  CHECK(platform.string() == "linux/amd64");

  REQUIRE(oci::parsePlatform("linux/aarch64/v8", platform));
  CHECK(platform.string() == "linux/arm64/v8");
}

TEST_CASE("OCI platforms reject incomplete and non-Linux selectors") {
  oci::Platform platform;
  for (const char* value : {"amd64", "windows/amd64", "linux/amd64/",
                            "linux/amd64/v8/extra", "linux/../amd64"})
    CHECK_FALSE(oci::parsePlatform(value, platform));
}
