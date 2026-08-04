#include "exception_handler.hpp"
#include "../app_exception.hpp"
#include "../include/util/app_exit.hpp"
#include <nlohmann/json.hpp>
#include <iostream>

namespace util {
    int handle_exceptions(
        std::function<int()> main_logic,
        ErrorContext context,
        const std::string& command_name
    ) {
        try {
            return main_logic();
        }
        catch (const AppException& e) {
            if (context == ErrorContext::CLI) {
                std::cerr << "Error: " << e.what() << std::endl;
            } else {
                nlohmann::json error_json = {
                    {"action", command_name},
                    {"status", "error"},
                    {"error", e.what()}
                };
                std::cout << error_json.dump() << std::endl;
            }
            return app::toInt(app::Exit::Usage);
        }
        catch (const std::invalid_argument& e) {
            if (context == ErrorContext::CLI) {
                std::cerr << "Invalid argument: " << e.what() << std::endl;
            } else {
                nlohmann::json error_json = {
                    {"action", command_name},
                    {"status", "error"},
                    {"error", std::string("Invalid argument: ") + e.what()}
                };
                std::cout << error_json.dump() << std::endl;
            }
            return app::toInt(app::Exit::Usage);
        }
        catch (const std::out_of_range& e) {
            if (context == ErrorContext::CLI) {
                std::cerr << "Out of range error: " << e.what() << std::endl;
            } else {
                nlohmann::json error_json = {
                    {"action", command_name},
                    {"status", "error"},
                    {"error", std::string("Out of range: ") + e.what()}
                };
                std::cout << error_json.dump() << std::endl;
            }
            return app::toInt(app::Exit::Unknown);
        }
        catch (const std::bad_alloc& e) {
            if (context == ErrorContext::CLI) {
                std::cerr << "Memory allocation failed: " << e.what() << std::endl;
            } else {
                nlohmann::json error_json = {
                    {"action", command_name},
                    {"status", "error"},
                    {"error", std::string("Memory allocation failed: ") + e.what()}
                };
                std::cout << error_json.dump() << std::endl;
            }
            return app::toInt(app::Exit::Unknown);
        }
        catch (const std::exception& e) {
            if (context == ErrorContext::CLI) {
                std::cerr << "Unexpected error: " << e.what() << std::endl;
            } else {
                nlohmann::json error_json = {
                    {"action", command_name},
                    {"status", "error"},
                    {"error", std::string("Unexpected error: ") + e.what()}
                };
                std::cout << error_json.dump() << std::endl;
            }
            return app::toInt(app::Exit::Unknown);
        }
    }
}