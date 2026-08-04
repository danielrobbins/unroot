#pragma once
#include <string>

// Forward declare our custom terminate handler
void custom_terminate_handler();

std::string namespacePolicyDiagnostic(const char* operation, int error);
