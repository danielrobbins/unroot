#include "doctest.h"

#include "archive_report.hpp"

TEST_CASE("archive inspection protocol round trips structured findings") {
  archiveinfo::ArchiveReport report;
  report.format = "POSIX pax interchange format";
  report.filters = {"gzip"};
  report.members = 4;
  report.regularBytes = 1024;
  report.maxUid = 1000;
  report.maxGid = 2000;
  report.outsideDev = {12, 34, true, false};
  report.ociLayout = true;
  report.rootPrefix = "bundle/rootfs";
  report.rootPrefixPresent = true;
  report.rootPrefixReservedMetadata = true;
  report.xattrs = {2, {"bin/tool"}};

  auto parsed = archiveinfo::parseRecord(archiveinfo::makeRecord(report));

  REQUIRE(parsed);
  CHECK(parsed.report.format == report.format);
  CHECK(parsed.report.filters == report.filters);
  CHECK(parsed.report.members == 4);
  CHECK(parsed.report.regularBytes == 1024);
  CHECK(parsed.report.maxUid == 1000);
  CHECK(parsed.report.maxGid == 2000);
  CHECK(parsed.report.outsideDev.maxUid == 12);
  CHECK(parsed.report.outsideDev.maxGid == 34);
  CHECK(parsed.report.outsideDev.posixAcl);
  CHECK_FALSE(parsed.report.outsideDev.xattr);
  CHECK(parsed.report.ociLayout);
  CHECK(parsed.report.rootPrefix == "bundle/rootfs");
  CHECK(parsed.report.rootPrefixPresent);
  CHECK(parsed.report.rootPrefixReservedMetadata);
  CHECK(parsed.report.xattrs.count == 2);
  CHECK(parsed.report.xattrs.examples == std::vector<std::string>{"bin/tool"});
}

TEST_CASE("archive inspection requires typed outside-dev requirements") {
  const auto valid = archiveinfo::ArchiveReport().toJson();
  for (const auto* field : {"max_uid", "max_gid", "posix_acl", "xattr"}) {
    auto missing = valid;
    missing["outside_dev"].erase(field);
    CHECK_FALSE(archiveinfo::parseRecord(
        std::string(archiveinfo::Protocol) + " " + missing.dump()));
    auto malformed = valid;
    malformed["outside_dev"][field] = "invalid";
    CHECK_FALSE(archiveinfo::parseRecord(
        std::string(archiveinfo::Protocol) + " " + malformed.dump()));
  }
}

TEST_CASE("archive inspection protocol rejects malformed and oversized examples") {
  CHECK_FALSE(archiveinfo::parseRecord("other-v1 {}"));
  CHECK_FALSE(archiveinfo::parseRecord("unroot-archive-v1 []"));

  archiveinfo::ArchiveReport report;
  report.format = "tar";
  report.xattrs.examples = {"a", "b", "c", "d", "e"};
  CHECK_FALSE(archiveinfo::parseRecord(archiveinfo::makeRecord(report)));
}
