#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string_view>

#include <mcqnet/detail/operation_awaiter.h>
#include <mcqnet/mcqnet.h>

namespace {
using mcqnet::detail::OperationBase;
using mcqnet::task::JoinHandle;
using mcqnet::task::Task;
using mcqnet::task::spawn;

[[noreturn]] void fail(std::string_view message) {
    std::cerr << message << '\n';
    std::abort();
}

void check(bool condition, std::string_view message) {
    if ( !condition ) {
        fail(message);
    }
}

Task<int> produce_value(int value) { co_return value; }

Task<int> add_one(int value) { co_return value + 1; }

Task<int> await_child_task() { co_return co_await add_one(41); }

Task<void> mark_started(bool* started) {
    *started = true;
    co_return;
}

Task<int> failing_task() {
    throw std::runtime_error("task failure");
    co_return 0;
}

Task<int> await_join_handle(JoinHandle<int> join) { co_return co_await join; }

Task<void> await_void_join_handle(JoinHandle<void> join, bool* resumed) {
    co_await join;
    *resumed = true;
}

class ManualGate {
public:
    [[nodiscard]] bool await_ready() const noexcept { return open_; }

    bool await_suspend(std::coroutine_handle<> continuation) noexcept {
        continuation_ = continuation;
        return !open_;
    }

    void await_resume() const noexcept { }

    void open() noexcept {
        open_ = true;
        if ( continuation_ != nullptr ) {
            std::coroutine_handle<> continuation = continuation_;
            continuation_ = nullptr;
            continuation.resume();
        }
    }

private:
    bool open_ { false };
    std::coroutine_handle<> continuation_ { };
};

Task<int> gated_value(ManualGate& gate, int value) {
    co_await gate;
    co_return value;
}

Task<void> gated_void(ManualGate& gate, bool* completed) {
    co_await gate;
    *completed = true;
}

Task<void> mark_joined(bool* joined) {
    *joined = true;
    co_return;
}

class ManualOperation final : public OperationBase {
public:
    void submit() { submitted_ = true; }

    void finish(int value) { complete(value); }

    int await_resume() {
        rethrow_if_exception();
        return completion_result();
    }

    [[nodiscard]] bool submitted() const noexcept { return submitted_; }

private:
    bool submitted_ { false };
};

Task<int> await_manual_operation(ManualOperation& operation) {
    co_return co_await mcqnet::detail::make_operation_awaiter(operation);
}

class ThrowingOperation final : public OperationBase {
public:
    void submit() { throw std::runtime_error("submit failure"); }

    void await_resume() { rethrow_if_exception(); }
};

Task<void> await_throwing_operation(ThrowingOperation& operation) {
    co_await mcqnet::detail::make_operation_awaiter(operation);
}
} // namespace

int main() {
    {
        auto task = produce_value(7);
        check(task.valid(), "Task<int> should hold a coroutine handle");
        check(!task.done(), "Task<int> should start suspended");
        task.start();
        check(task.done(), "Task<int> should complete after start()");
        check(task.await_resume() == 7, "Task<int> should preserve the return value");
    }

    {
        auto task = await_child_task();
        task.start();
        check(task.done(), "co_await on Task<int> should resume the parent coroutine");
        check(task.await_resume() == 42, "Task<int> chaining should propagate values");
    }

    {
        bool started = false;
        auto task = mark_started(&started);
        task.start();
        check(started, "Task<void> should run its coroutine body");
        check(task.done(), "Task<void> should complete after start()");
        task.await_resume();
    }

    {
        auto task = failing_task();
        task.start();

        bool caught = false;
        try {
            (void)task.await_resume();
        } catch ( const std::runtime_error& ex ) {
            caught = std::string_view(ex.what()) == "task failure";
        }
        check(caught, "Task<int> should rethrow stored coroutine exceptions");
    }

    {
        ManualGate gate;
        auto task = await_join_handle(spawn(gated_value(gate, 55)));
        task.start();
        check(!task.done(), "JoinHandle<int> should suspend the awaiting task until ready");
        gate.open();
        check(task.done(), "JoinHandle<int> should resume the awaiting task on completion");
        check(task.await_resume() == 55, "JoinHandle<int> should propagate the stored value");
    }

    {
        auto join = spawn(produce_value(91));
        check(join.await_ready(), "JoinHandle<int> should report readiness after set_value()");
        check(join.join() == 91, "JoinHandle<int>::join() should return the stored value");
    }

    {
        ManualGate gate;
        bool completed = false;
        bool resumed = false;
        auto task = await_void_join_handle(spawn(gated_void(gate, &completed)), &resumed);
        task.start();
        check(!task.done(), "JoinHandle<void> should suspend the awaiting task until ready");
        gate.open();
        check(completed, "JoinHandle<void> should resume the spawned task body before join completion");
        check(task.done(), "JoinHandle<void> should resume the awaiting task on completion");
        task.await_resume();
        check(resumed, "JoinHandle<void> should resume the parent coroutine body");
    }

    {
        ManualOperation operation;
        auto task = await_manual_operation(operation);
        task.start();
        check(operation.submitted(), "OperationAwaiter should invoke submit() when awaited");
        check(!task.done(), "Manual operation should keep the task suspended until completion");
        operation.finish(123);
        check(task.done(), "Manual operation completion should resume the awaiting task");
        check(task.await_resume() == 123, "OperationAwaiter should propagate await_resume() values");
    }

    {
        auto join = spawn(produce_value(88));
        check(join.await_ready(), "spawn() should complete immediately for already-ready tasks");
        check(join.join() == 88, "spawn() should bridge Task<int> results into JoinHandle<int>");
    }

    {
        ManualGate gate;
        auto join = spawn(gated_value(gate, 144));
        check(!join.await_ready(), "spawn() should keep JoinHandle pending while the task is suspended");
        gate.open();
        check(join.await_ready(), "spawn() should mark the JoinHandle ready after task completion");
        check(join.join() == 144, "spawn() should forward async Task<int> completion values");
    }

    {
        auto join = spawn(failing_task());

        bool caught = false;
        try {
            (void)join.join();
        } catch ( const std::runtime_error& ex ) {
            caught = std::string_view(ex.what()) == "task failure";
        }
        check(caught, "spawn() should surface Task<int> exceptions through JoinHandle<int>::join()");
    }

    {
        bool joined = false;
        auto join = spawn(mark_joined(&joined));
        join.join();
        check(joined, "spawn() should bridge Task<void> completion into JoinHandle<void>");
    }

    {
        ThrowingOperation operation;
        auto task = await_throwing_operation(operation);
        task.start();

        bool caught = false;
        try {
            task.await_resume();
        } catch ( const std::runtime_error& ex ) {
            caught = std::string_view(ex.what()) == "submit failure";
        }
        check(caught, "OperationAwaiter should surface submit() exceptions via await_resume()");
    }

    std::cout << "coroutine test passed\n";
    return 0;
}
