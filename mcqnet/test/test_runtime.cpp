#include <future>
#include <chrono>
#include <cstdlib>
#include <deque>
#include <iostream>
#include <stdexcept>
#include <string_view>
#include <thread>

#include <mcqnet/detail/operation_awaiter.h>
#include <mcqnet/mcqnet.h>

namespace {
using mcqnet::RuntimeException;
using mcqnet::Task;
using mcqnet::errc;
using mcqnet::runtime::Handle;
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

Task<void> mark_flag(bool* flag) {
    *flag = true;
    co_return;
}

Task<void> mark_flag_and_stop(bool* flag, Runtime* runtime) {
    *flag = true;
    runtime->stop();
    co_return;
}

Task<void> reenter_run(Runtime* runtime, bool* caught) {
    try {
        runtime->run();
    } catch ( const RuntimeException& ex ) {
        *caught = ex.code().value == errc::invalid_state;
    }
    runtime->stop();
    co_return;
}

Task<void> reenter_run_one(Runtime* runtime, bool* caught) {
    try {
        (void)runtime->run_one();
    } catch ( const RuntimeException& ex ) {
        *caught = ex.code().value == errc::invalid_state;
    }
    co_return;
}

Task<int> produce_value(int value) { co_return value; }

Task<void> spawn_and_join(Handle handle, Runtime* runtime, int* result, bool* completed) {
    auto join = handle.spawn(produce_value(42));
    *result = co_await join;
    *completed = true;
    runtime->stop();
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

Task<void> await_free_spawn(Runtime* runtime, ManualGate* gate, int* result, bool* completed) {
    auto join = mcqnet::task::spawn(gated_value(*gate, 9));
    *result = co_await join;
    *completed = true;
    runtime->stop();
}

class ManualOperation final : public mcqnet::detail::OperationBase {
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

Task<void> await_manual_operation_and_stop(
    ManualOperation& operation, Runtime* runtime, int* result, bool* completed) {
    *result = co_await mcqnet::detail::make_operation_awaiter(operation);
    *completed = true;
    runtime->stop();
}

Task<void> await_manual_operation_without_stop(ManualOperation& operation, int* result, bool* completed) {
    *result = co_await mcqnet::detail::make_operation_awaiter(operation);
    *completed = true;
}

Task<void> await_untracked_gate(ManualGate& gate, bool* completed) {
    co_await gate;
    *completed = true;
}

class TrackedGate {
public:
    [[nodiscard]] bool await_ready() const noexcept { return open_; }

    bool await_suspend(std::coroutine_handle<> continuation) noexcept {
        continuation_ = continuation;
        scheduler_ = mcqnet::detail::SchedulerScope::current();
        scheduler_.retain_work();
        return !open_;
    }

    void await_resume() const noexcept { }

    void open() noexcept {
        open_ = true;
        if ( continuation_ != nullptr ) {
            std::coroutine_handle<> continuation = continuation_;
            continuation_ = nullptr;
            if ( scheduler_.valid() ) {
                scheduler_.schedule(continuation);
            } else {
                continuation.resume();
            }
        }
    }

private:
    bool open_ { false };
    std::coroutine_handle<> continuation_ { };
    mcqnet::detail::SchedulerBinding scheduler_ { };
};

Task<void> await_tracked_gate(TrackedGate& gate, bool* completed) {
    co_await gate;
    *completed = true;
}

class ManualCompletionBackend;

class BackendOperation final : public mcqnet::detail::OperationBase {
public:
    explicit BackendOperation(ManualCompletionBackend& backend) noexcept
        : backend_(backend) { }

    void submit();

    void finish(int value) { complete(value); }

    int await_resume() {
        rethrow_if_exception();
        return completion_result();
    }

    [[nodiscard]] bool submitted() const noexcept { return submitted_; }

private:
    ManualCompletionBackend& backend_;
    bool submitted_ { false };
};

class ManualCompletionBackend final : public mcqnet::runtime::CompletionBackend {
public:
    struct ReadyCompletion {
        BackendOperation* operation { nullptr };
        int value { 0 };
    };

    void submit(BackendOperation* operation) {
        std::lock_guard<std::mutex> lock(mutex_);
        pending_.push_back(operation);
    }

    void complete_next(int value) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            MCQNET_ASSERT(!pending_.empty());
            ready_.push_back(ReadyCompletion { pending_.front(), value });
            pending_.pop_front();
        }
        cv_.notify_one();
    }

    bool poll(clock::duration timeout) override {
        ReadyCompletion completion { };
        {
            std::unique_lock<std::mutex> lock(mutex_);
            const auto ready = [this] { return wake_requested_ || !ready_.empty(); };

            if ( ready_.empty() && !wake_requested_ ) {
                if ( timeout == clock::duration::zero() ) {
                    return false;
                }
                if ( timeout == clock::duration::max() ) {
                    cv_.wait(lock, ready);
                } else {
                    cv_.wait_for(lock, timeout, ready);
                }
            }

            if ( ready_.empty() ) {
                wake_requested_ = false;
                return false;
            }

            completion = ready_.front();
            ready_.pop_front();
            wake_requested_ = false;
        }

        completion.operation->finish(completion.value);
        return true;
    }

    void wake() noexcept override {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            wake_requested_ = true;
        }
        cv_.notify_all();
    }

private:
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<BackendOperation*> pending_;
    std::deque<ReadyCompletion> ready_;
    bool wake_requested_ { false };
};

inline void BackendOperation::submit() {
    submitted_ = true;
    backend_.submit(this);
}

Task<void> await_backend_operation_and_stop(
    BackendOperation& operation, Runtime* runtime, int* result, bool* completed) {
    *result = co_await mcqnet::detail::make_operation_awaiter(operation);
    *completed = true;
    runtime->stop();
}

} // namespace

int main() {
    {
        Runtime runtime;
        bool resumed = false;
        auto task = mark_flag(&resumed);

        check(!runtime.run_one(), "run_one() should return false when the ready queue is empty");

        runtime.post(task.handle());
        check(!resumed, "post() should enqueue work instead of resuming inline");
        check(runtime.run_one(), "run_one() should execute one queued continuation");
        check(resumed, "run_one() should resume the queued continuation");
        check(!runtime.run_one(), "run_one() should return false after the queue is drained");
    }

    {
        Runtime runtime;
        auto join = runtime.spawn(produce_value(7));

        check(!join.await_ready(), "Runtime::spawn() should schedule the task instead of running inline");
        check(runtime.run_one(), "run_one() should start the runtime-owned bridge task");
        check(!join.await_ready(), "Runtime::spawn() should keep the bridge continuation on the runtime queue");
        check(runtime.run_one(), "run_one() should resume the queued bridge continuation");
        check(join.await_ready(), "Runtime::spawn() should complete after the bridge continuation runs");
        check(join.join() == 7, "Runtime::spawn() should forward the task result into JoinHandle");
    }

    {
        Runtime runtime;
        int result = 0;
        bool completed = false;
        auto task = spawn_and_join(runtime.handle(), &runtime, &result, &completed);

        runtime.post(task.handle());
        runtime.run();

        check(completed, "run() should drain queued work until stop() after nested spawn/join");
        check(result == 42, "JoinHandle continuation should resume back on the runtime queue");
    }

    {
        Runtime runtime;
        runtime.stop();

        bool caught = false;
        try {
            auto task = mark_flag(nullptr);
            runtime.post(task.handle());
        } catch ( const RuntimeException& ex ) {
            caught = ex.code().value == errc::runtime_stopped;
        }
        check(caught, "post() should throw runtime_stopped after stop()");
    }

    {
        Runtime runtime;
        runtime.stop();

        bool caught = false;
        try {
            (void)runtime.spawn(produce_value(1));
        } catch ( const RuntimeException& ex ) {
            caught = ex.code().value == errc::runtime_stopped;
        }
        check(caught, "spawn() should throw runtime_stopped after stop()");
    }

    {
        Runtime runtime;
        bool resumed = false;
        auto task = mark_flag_and_stop(&resumed, &runtime);
        std::promise<void> runner_started;
        auto runner_started_future = runner_started.get_future();

        std::jthread runner([&] {
            runner_started.set_value();
            runtime.run();
        });

        runner_started_future.wait();
        runtime.post(task.handle());
        runner.join();

        check(resumed, "post() from another thread should wake run() and execute queued work");
    }

    {
        Runtime runtime;
        std::promise<void> runner_started;
        auto runner_started_future = runner_started.get_future();

        std::jthread runner([&] {
            runner_started.set_value();
            runtime.run();
        });

        runner_started_future.wait();
        runtime.stop();
        runner.join();

        check(runtime.stopped(), "stop() from another thread should wake an idle run()");
    }

    {
        Runtime runtime;
        std::promise<void> runner_started;
        auto runner_started_future = runner_started.get_future();
        bool caught = false;

        std::jthread runner([&] {
            runner_started.set_value();
            runtime.run();
        });

        runner_started_future.wait();
        try {
            (void)runtime.run_one();
        } catch ( const RuntimeException& ex ) {
            caught = ex.code().value == errc::invalid_state;
        }
        runtime.stop();
        runner.join();

        check(caught, "run_one() should throw invalid_state while run() is already driving the runtime");
    }

    {
        Runtime runtime;
        bool caught = false;
        auto task = reenter_run(&runtime, &caught);

        runtime.post(task.handle());
        runtime.run();

        check(caught, "run() should throw invalid_state when re-entered from a running continuation");
    }

    {
        Runtime runtime;
        bool caught = false;
        auto task = reenter_run_one(&runtime, &caught);

        runtime.post(task.handle());
        check(runtime.run_one(), "run_one() should execute the continuation that attempts nested driving");
        check(caught, "run_one() should throw invalid_state when re-entered from a running continuation");
    }

    {
        Runtime runtime;
        ManualOperation operation;
        auto task = await_manual_operation(operation);

        runtime.post(task.handle());
        check(runtime.run_one(), "run_one() should start the task that awaits an operation");
        check(operation.submitted(), "OperationAwaiter should still submit the operation under runtime");
        check(!task.done(), "Task should suspend while waiting for the operation");

        operation.finish(123);
        check(!task.done(), "Operation completion should enqueue the continuation instead of resuming inline");
        check(runtime.run_one(), "run_one() should resume the operation continuation from the runtime queue");
        check(task.done(), "Operation continuation should complete after the queued resume");
        check(task.await_resume() == 123, "OperationAwaiter should preserve the completion result");
    }

    {
        Runtime runtime;
        ManualOperation operation;
        int result = 0;
        bool completed = false;
        auto task = await_manual_operation_and_stop(operation, &runtime, &result, &completed);
        std::promise<void> runner_started;
        auto runner_started_future = runner_started.get_future();

        runtime.post(task.handle());
        std::jthread runner([&] {
            runner_started.set_value();
            runtime.run();
        });

        runner_started_future.wait();
        while ( !operation.submitted() ) {
            std::this_thread::yield();
        }

        std::jthread completer([&] { operation.finish(321); });
        completer.join();
        runner.join();

        check(completed, "Operation completion from another thread should resume the awaiting task");
        check(result == 321, "Operation completion from another thread should preserve the result");
    }

    {
        Runtime runtime;
        ManualOperation operation;
        int result = 0;
        bool completed = false;
        std::promise<void> runner_done;
        auto runner_done_future = runner_done.get_future();
        auto task = await_manual_operation_without_stop(operation, &result, &completed);

        runtime.post(task.handle());
        std::jthread runner([&] {
            runtime.run();
            runner_done.set_value();
        });

        while ( !operation.submitted() ) {
            std::this_thread::yield();
        }

        runtime.stop();
        check(
            runner_done_future.wait_for(std::chrono::milliseconds(20)) == std::future_status::timeout,
            "run() should not exit after stop() while pending work is still outstanding");

        operation.finish(456);
        runner.join();

        check(completed, "run() should drain pending operation work before returning after stop()");
        check(result == 456, "pending operation completion should still propagate its result after stop()");
    }

    {
        Runtime runtime;
        ManualOperation operation;
        int result = 0;
        bool completed = false;
        auto task = await_manual_operation_without_stop(operation, &result, &completed);

        runtime.post(task.handle());
        check(runtime.run_one(), "run_one() should start the task that creates pending work");
        check(!runtime.run_one(), "run_one() should remain non-blocking while pending work exists but nothing is ready");

        runtime.stop();
        check(!runtime.run_one(), "run_one() should still return false after stop() if no continuation is ready yet");

        operation.finish(654);
        check(runtime.run_one(), "run_one() should process the continuation once pending work becomes ready");
        check(completed, "run_one() should resume the task after pending work becomes ready");
        check(result == 654, "run_one() should preserve results after pending work resumes");
    }

    {
        Runtime runtime;
        ManualGate gate;
        int result = 0;
        bool completed = false;
        auto task = await_free_spawn(&runtime, &gate, &result, &completed);

        runtime.post(task.handle());
        check(runtime.run_one(), "run_one() should start the task that awaits free spawn()");
        check(!task.done(), "Task should suspend on the JoinHandle returned by free spawn()");

        gate.open();
        check(!task.done(), "Opening the gate should queue runtime work instead of finishing inline");
        check(runtime.run_one(), "run_one() should resume the free spawn bridge continuation");
        check(runtime.run_one(), "run_one() should resume the parent continuation waiting on JoinHandle");
        check(task.done(), "Parent task should complete after the runtime drains queued continuations");
        task.await_resume();
        check(completed, "Free spawn JoinHandle should resume the parent on the runtime queue");
        check(result == 9, "Free spawn JoinHandle should preserve the child task result");
    }

    {
        Runtime runtime;
        ManualGate gate;
        bool completed = false;
        auto task = await_untracked_gate(gate, &completed);

        runtime.post(task.handle());
        check(runtime.run_one(), "run_one() should start the task that awaits an untracked awaitable");

        runtime.stop();
        runtime.run();

        check(
            !completed,
            "A raw awaitable without scheduler/work-tracker integration should not count as runtime pending work");
    }

    {
        Runtime runtime;
        TrackedGate gate;
        bool completed = false;
        std::promise<void> runner_done;
        auto runner_done_future = runner_done.get_future();
        auto task = await_tracked_gate(gate, &completed);

        runtime.post(task.handle());
        check(runtime.run_one(), "run_one() should start the task that awaits a tracked awaitable");

        runtime.stop();
        std::jthread runner([&] {
            runtime.run();
            runner_done.set_value();
        });
        check(
            runner_done_future.wait_for(std::chrono::milliseconds(20)) == std::future_status::timeout,
            "A scheduler-aware awaitable that retains work should keep run() alive after stop()");

        gate.open();
        runner.join();
        check(completed, "Tracked awaitable completion should resume the suspended task through the runtime");
    }

    {
        ManualCompletionBackend backend;
        Runtime runtime(&backend);
        BackendOperation operation(backend);
        int result = 0;
        bool completed = false;
        auto task = await_backend_operation_and_stop(operation, &runtime, &result, &completed);

        runtime.post(task.handle());
        std::promise<void> runner_started;
        auto runner_started_future = runner_started.get_future();

        std::jthread runner([&] {
            runner_started.set_value();
            runtime.run();
        });

        runner_started_future.wait();
        while ( !operation.submitted() ) {
            std::this_thread::yield();
        }
        check(operation.submitted(), "Backend-driven operation should still submit through OperationAwaiter");

        backend.complete_next(777);
        runner.join();
        check(task.done(), "Completion backend dispatch should eventually resume the awaiting task");
        task.await_resume();
        check(completed, "Completion backend should resume the waiting task through runtime::run()");
        check(result == 777, "Completion backend should preserve operation results");
    }

    std::cout << "runtime test passed\n";
    return 0;
}
