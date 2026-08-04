#include "doctest.h"
#include "util/exception_handler.hpp"
#include "app_exception.hpp"
#include "include/util/app_exit.hpp"
#include <stdexcept>
#include <new>
#include <sstream>
#include <iostream>

// Helper to capture std::cout and std::cerr during test
struct CoutCerrCapture {
    std::streambuf* old_out{nullptr};
    std::streambuf* old_err{nullptr};
    std::ostringstream out_ss; std::ostringstream err_ss;
    CoutCerrCapture() {
        old_out = std::cout.rdbuf(out_ss.rdbuf());
        old_err = std::cerr.rdbuf(err_ss.rdbuf());
    }
    ~CoutCerrCapture() {
        std::cout.rdbuf(old_out);
        std::cerr.rdbuf(old_err);
    }
};

TEST_CASE("handle_exceptions maps AppException to Usage exit code (CLI)") {
    CoutCerrCapture cap;
    int rc = util::handle_exceptions([](){ throw AppException(util::make_error(util::LibErr::Invalid,0,"bad"), "action"); return 0; }, util::ErrorContext::CLI, "cmd");
    CHECK(rc == app::toInt(app::Exit::Usage));
    CHECK(cap.err_ss.str().find("bad") != std::string::npos);
}

TEST_CASE("handle_exceptions maps invalid_argument to Usage exit code (NDJSON)") {
    CoutCerrCapture cap;
    int rc = util::handle_exceptions([](){ throw std::invalid_argument("oops"); return 0; }, util::ErrorContext::NDJSON, "cmd");
    CHECK(rc == app::toInt(app::Exit::Usage));
    CHECK(cap.out_ss.str().find("oops") != std::string::npos);
}

TEST_CASE("handle_exceptions maps out_of_range to Unknown exit code") {
    CoutCerrCapture cap;
    int rc = util::handle_exceptions([](){ throw std::out_of_range("rng"); return 0; }, util::ErrorContext::CLI, "cmd");
    CHECK(rc == app::toInt(app::Exit::Unknown));
    CHECK(cap.err_ss.str().find("rng") != std::string::npos);
}

TEST_CASE("handle_exceptions maps bad_alloc to Unknown exit code") {
    CoutCerrCapture cap;
    int rc = util::handle_exceptions([](){ throw std::bad_alloc(); return 0; }, util::ErrorContext::NDJSON, "cmd");
    CHECK(rc == app::toInt(app::Exit::Unknown));
    CHECK(cap.out_ss.str().find("Memory allocation failed") != std::string::npos);
}

TEST_CASE("handle_exceptions maps generic exception to Unknown exit code") {
    struct CustomEx : public std::exception { const char* what() const noexcept override { return "custom"; } };
    CoutCerrCapture cap;
    int rc = util::handle_exceptions([](){ throw CustomEx(); return 0; }, util::ErrorContext::CLI, "cmd");
    CHECK(rc == app::toInt(app::Exit::Unknown));
    CHECK(cap.err_ss.str().find("custom") != std::string::npos);
}
