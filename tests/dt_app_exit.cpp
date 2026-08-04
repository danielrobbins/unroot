#include "doctest.h"
#include "util/app_exit.hpp"

TEST_CASE("toInt converts enum to underlying int") {
    CHECK(app::toInt(app::Exit::Ok) == 0);
    CHECK(app::toInt(app::Exit::Usage) == 2);
    CHECK(app::toInt(app::Exit::NotFound) == 3);
    CHECK(app::toInt(app::Exit::Io) == 5);
    CHECK(app::toInt(app::Exit::Exec) == 12);
    CHECK(app::toInt(app::Exit::Ns) == 20);
    CHECK(app::toInt(app::Exit::ChildSignaled) == 25);
    CHECK(app::toInt(app::Exit::Unknown) == 30);
}
