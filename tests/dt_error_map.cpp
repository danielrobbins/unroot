#include "doctest.h"
#include "util/error_map.hpp"
#include "util/errors.hpp"
#include "util/app_exit.hpp"
#include <errno.h>

TEST_CASE("mapErrorToExit core category mappings") {
    using util::Error; using util::LibErr; using app::Exit; using app::mapErrorToExit;
    CHECK(mapErrorToExit(Error{LibErr::Invalid, 0, ""}) == Exit::Usage);
    CHECK(mapErrorToExit(Error{LibErr::NotFound, 0, ""}) == Exit::NotFound);
    CHECK(mapErrorToExit(Error{LibErr::Namespace, 0, ""}) == Exit::Ns);
    CHECK(mapErrorToExit(Error{LibErr::Binfmt, 0, ""}) == Exit::Unknown);
    CHECK(mapErrorToExit(Error{LibErr::Unknown, 0, ""}) == Exit::Unknown);
}

TEST_CASE("mapErrorToExit IO + errno specialization") {
    using util::Error; using util::LibErr; using app::Exit; using app::mapErrorToExit;
    CHECK(mapErrorToExit(Error{LibErr::Io, ENOENT, ""}) == Exit::NotFound); // ENOENT special cased
    CHECK(mapErrorToExit(Error{LibErr::Io, EACCES, ""}) == Exit::Io);
    CHECK(mapErrorToExit(Error{LibErr::Syscall, EFAULT, ""}) == Exit::Io);
    CHECK(mapErrorToExit(Error{LibErr::Permission, EACCES, ""}) == Exit::Io);
}
