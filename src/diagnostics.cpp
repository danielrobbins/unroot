#include <iostream>
#include <exception>
#include <stdexcept>
#include <typeinfo> // For typeid
#include <cxxabi.h> // For demangling the type name
#include <cerrno>
#include <cstdlib>  // For free()
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

namespace {

std::string readPolicyValue(const char* path) {
    std::ifstream input(path);
    std::string value;
    std::getline(input, value);
    return value;
}

} // namespace

std::string namespacePolicyDiagnostic(const char* operation, int error) {
    std::string message = operation;
    if (error) message += ": " + std::string(std::strerror(error));
    if (error != EPERM && error != EACCES) return message;

    std::vector<std::string> policy;
    if (readPolicyValue(
            "/proc/sys/kernel/apparmor_restrict_unprivileged_userns") == "1") {
        policy.emplace_back(
            "AppArmor unprivileged-userns restriction is enabled "
            "(kernel.apparmor_restrict_unprivileged_userns=1)");
    }
    if (readPolicyValue("/proc/sys/kernel/unprivileged_userns_clone") == "0") {
        policy.emplace_back("unprivileged user namespaces are disabled "
                            "(kernel.unprivileged_userns_clone=0)");
    }
    if (readPolicyValue("/proc/sys/user/max_user_namespaces") == "0") {
        policy.emplace_back("user namespaces are exhausted or disabled "
                            "(user.max_user_namespaces=0)");
    }
    if (readPolicyValue("/sys/fs/selinux/enforce") == "1") {
        std::string context = readPolicyValue("/proc/self/attr/current");
        policy.emplace_back(context.empty() ? "SELinux is enforcing"
                                            : "SELinux is enforcing for " + context);
    }
    if (policy.empty()) {
        policy.emplace_back(
            "permission may be denied by kernel, LSM, seccomp, or outer-container policy");
    }
    for (const auto& detail : policy) message += "; " + detail;
    return message;
}

// The custom handler function
void custom_terminate_handler() {
    std::cerr << "[FATAL] Unhandled exception. This is a bug." << std::endl;

    try {
        // Check if there is an active exception that we can inspect.
        auto exc_ptr = std::current_exception();
        if (exc_ptr) {
            // rethrow() will throw the exception again. We can then catch it
            // to find out its type and message.
            std::rethrow_exception(exc_ptr);
        }
    } catch (const std::exception& e) {
        // We caught a standard exception. We can get its message and,
        // most importantly, its real type.

        // Demangle the type name to make it human-readable (e.g., "std::runtime_error" instead of "St13runtime_error")
        int status;
        char* demangled_name = abi::__cxa_demangle(typeid(e).name(), nullptr, nullptr, &status);

        std::cerr << "  Type: " << (status == 0 ? demangled_name : typeid(e).name()) << std::endl;
        std::cerr << "  What: " << e.what() << std::endl;

        if (demangled_name) {
            free(demangled_name);
        }

    } catch (...) {
        // The exception was not a standard C++ exception (e.g., a custom C exception).
        // We can't get much info, but we can report that.
        std::cerr << "  Type: Unknown exception type" << std::endl;
    }

    std::cerr << "Terminating." << std::endl;
    std::abort(); // Ensure the program still aborts as expected.
}
