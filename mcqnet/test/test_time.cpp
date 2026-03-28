#include <chrono>
#include <cstdlib>
#include <future>
#include <iostream>
#include <string_view>
#include <thread>

#include <mcqnet/mcqnet.h>

namespace {
using namespace std::chrono_literals;

using mcqnet::CancelSource;
using mcqnet::RuntimeException;
using mcqnet::Task;
using mcqnet::errc;
using mcqnet::runtime::Runtime;

[[noreturn]] void fail(std::string_view message) {
    std::cerr << message << '\n';
    std::abort();
}

void check(bool condition, std::string_view message) {
    if ( !condition ) {
        fail(message);
    }
}

Task<void> sleep_until_and_stop(Runtime* runtime, bool* completed) {
    co_await mcqnet::time::sleep_until(Runtime::clock::now() + 10ms);
    *completed = true;
    runtime->stop();
}

Task<void> timeout_and_stop(Runtime* runtime, bool* timed_out) {
    try {
        co_await mcqnet::time::timeout(10ms);
    } catch ( const RuntimeException& ex ) {
        *timed_out = ex.code().value == errc::timed_out;
    }
    runtime->stop();
}

Task<void> cancelled_sleep_and_stop(Runtime* runtime, mcqnet::CancelToken cancel_token, bool* cancelled) {
    try {
        co_await mcqnet::time::sleep_for(5s, cancel_token);
    } catch ( const RuntimeException& ex ) {
        *cancelled = ex.code().value == errc::operation_aborted;
    }
    runtime->stop();
}

Task<void> sleep_without_stop(std::chrono::milliseconds duration, bool* completed) {
    co_await mcqnet::time::sleep_for(duration);
    *completed = true;
}

Task<void> explicit_handle_sleep(Runtime* runtime, bool* completed) {
    co_await mcqnet::time::sleep_for(runtime->handle(), 10ms);
    *completed = true;
    runtime->stop();
}

} // namespace

int main() {
    {
        Runtime runtime;
        bool completed = false;
        auto task = sleep_until_and_stop(&runtime, &completed);

        runtime.post(task.handle());
        runtime.run();

        check(completed, "sleep_until() should resume the awaiting task on the runtime");
    }

    {
        Runtime runtime;
        bool timed_out = false;
        auto task = timeout_and_stop(&runtime, &timed_out);

        runtime.post(task.handle());
        runtime.run();

        check(timed_out, "timeout() should surface timed_out through RuntimeException");
    }

    {
        Runtime runtime;
        CancelSource cancel_source;
        bool cancelled = false;
        auto task = cancelled_sleep_and_stop(&runtime, cancel_source.token(), &cancelled);

        runtime.post(task.handle());
        check(runtime.run_one(), "run_one() should start the task that awaits a cancellable sleep");

        cancel_source.cancel();
        check(runtime.run_one(), "run_one() should resume the task after sleep cancellation");
        task.await_resume();
        check(cancelled, "Cancelling a sleep should resume the waiter with operation_aborted");
    }

    {
        Runtime runtime;
        bool completed = false;
        std::promise<void> runner_done;
        auto runner_done_future = runner_done.get_future();
        auto task = sleep_without_stop(30ms, &completed);

        runtime.post(task.handle());
        check(runtime.run_one(), "run_one() should start the task that creates timer pending work");
        runtime.stop();

        std::jthread runner([&] {
            runtime.run();
            runner_done.set_value();
        });

        check(
            runner_done_future.wait_for(10ms) == std::future_status::timeout,
            "run() should stay blocked after stop() while a sleep timer is still pending");

        runner.join();
        check(completed, "A pending sleep timer should eventually resume the suspended task");
    }

    {
        Runtime runtime;
        bool completed = false;
        auto task = explicit_handle_sleep(&runtime, &completed);

        task.start();
        runtime.run();

        check(completed, "Explicit runtime handles should let sleep_for() bind outside a runtime context");
    }

    std::cout << "time test passed\n";
    return 0;
}
