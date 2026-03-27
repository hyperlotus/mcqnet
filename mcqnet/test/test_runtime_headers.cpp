#include <cstdlib>
#include <iostream>
#include <type_traits>

#include <mcqnet/mcqnet.h>

namespace {

[[noreturn]] void fail(const char* message) {
    std::cerr << message << '\n';
    std::abort();
}

void check(bool condition, const char* message) {
    if ( !condition ) {
        fail(message);
    }
}

} // namespace

int main() {
    static_assert(std::is_default_constructible_v<mcqnet::runtime::Runtime>);
    static_assert(std::is_default_constructible_v<mcqnet::runtime::Handle>);

    mcqnet::runtime::Runtime runtime;
    auto handle = runtime.handle();

    check(handle.valid(), "Runtime::handle() should produce a valid Handle");
    check(static_cast<bool>(handle), "Handle bool conversion should follow valid()");
    check(!runtime.stopped(), "Runtime should not start in the stopped state");

    runtime.stop();
    check(runtime.stopped(), "Runtime::stop() should mark the runtime as stopped");

    std::cout << "runtime header test passed\n";
    return 0;
}
