#include "doctest.h"

#include "filesystem_caps.hpp"

TEST_CASE("filesystem capability protocol round trips ACL support") {
  fsinfo::FilesystemCaps caps;
  caps.posixAcl = {false, "operation not supported"};
  caps.xattr = {true, ""};

  auto parsed = fsinfo::parseRecord(fsinfo::makeRecord(caps));

  REQUIRE(parsed);
  CHECK_FALSE(parsed.caps.posixAcl.supported);
  CHECK(parsed.caps.posixAcl.detail == "operation not supported");
  CHECK(parsed.caps.xattr.supported);
  CHECK(parsed.caps.xattr.detail.empty());
}

TEST_CASE("filesystem capability protocol rejects malformed records") {
  CHECK_FALSE(fsinfo::parseRecord("other-v1 {}"));
  CHECK_FALSE(fsinfo::parseRecord("unroot-filesystem-v1 []"));
  CHECK_FALSE(fsinfo::parseRecord(
      "unroot-filesystem-v1 {\"posix_acl\":{\"supported\":true}}"));
}
