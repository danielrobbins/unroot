// Minimal vendored doctest-like subset (bootstrap). Replace with official doctest for full features.
#pragma once
#ifndef UNROOT_MINI_DOCTEST_HPP
#define UNROOT_MINI_DOCTEST_HPP

#include <exception>
#include <iostream>
#include <vector>
#include <string>
#include <cstdlib>

namespace doctest {
struct TestCaseData { const char* name; void (*func)(); };
inline std::vector<TestCaseData>& registry() { static std::vector<TestCaseData> r; return r; }
struct Registrar { Registrar(const char* n, void(*f)()) { registry().push_back({n,f}); } };
struct Context { int run() { int fails=0; for(auto& tc: registry()){ try { tc.func(); } catch(const std::exception& e){ std::cerr << "[fail] "<<tc.name<<": "<<e.what()<<"\n"; ++fails; } catch(...){ std::cerr<<"[fail] "<<tc.name<<": unknown"<<"\n"; ++fails;} } return fails; } };
} // namespace doctest

// Main is supplied by a TU defining DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN before including this header.

#define DOCTEST_CONCAT_INNER(a,b) a##b
#define DOCTEST_CONCAT(a,b) DOCTEST_CONCAT_INNER(a,b)
#define DOCTEST_MAKE_TEST(fn_name, reg_name, test_name) \
		static void fn_name(); \
		static doctest::Registrar reg_name(test_name, fn_name); \
		static void fn_name()

// Use line number for uniqueness (sufficient for bootstrap scenario)
#define TEST_CASE(name) DOCTEST_MAKE_TEST(DOCTEST_CONCAT(test_fn_, __LINE__), DOCTEST_CONCAT(reg_, __LINE__), name)

#define REQUIRE(cond) do { if(!(cond)) { throw std::runtime_error(std::string("REQUIRE failed: ")+ #cond); } } while(0)
#define CHECK(cond) REQUIRE(cond)
#define CHECK_FALSE(cond) REQUIRE(!(cond))

// Exception expectation helpers (minimal)
#define REQUIRE_THROWS(expr) do { bool _thrown=false; try { expr; } catch(...) { _thrown=true; } if(!_thrown) throw std::runtime_error("REQUIRE_THROWS failed: " #expr " did not throw"); } while(0)
#define REQUIRE_THROWS_AS(expr, ex_type) do { bool _thrown=false; try { expr; } catch(const ex_type&) { _thrown=true; } catch(...) { throw std::runtime_error("REQUIRE_THROWS_AS failed: wrong exception for " #expr); } if(!_thrown) throw std::runtime_error("REQUIRE_THROWS_AS failed: no exception for " #expr); } while(0)

#endif // UNROOT_MINI_DOCTEST_HPP
