#pragma once

// 协作式取消基元。
// 这层只表达“取消是否已被请求”以及“如何唤醒等待方”；
// 具体取消后要不要向 OS backend 发 abort、要不要转成 timed_out，由上层操作决定。

#include <algorithm>
#include <condition_variable>
#include <mcqnet/detail/macro.h>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

namespace mcqnet::runtime {

class CancelToken;

namespace detail {

class CancelCallbackState {
public:
    using CallbackFn = void (*)(void*) noexcept;

    CancelCallbackState(CallbackFn callback, void* context) noexcept
        : callback_(callback)
        , context_(context) { }

    CancelCallbackState(const CancelCallbackState&) = delete;
    CancelCallbackState& operator=(const CancelCallbackState&) = delete;

    inline void invoke() noexcept {
        CallbackFn callback = nullptr;
        void* context = nullptr;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if ( !armed_ || callback_ == nullptr ) {
                return;
            }
            armed_ = false;
            running_ = true;
            callback = callback_;
            context = context_;
        }

        callback(context);

        {
            std::lock_guard<std::mutex> lock(mutex_);
            running_ = false;
        }
        cv_.notify_all();
    }

    inline void disarm_and_wait() noexcept {
        std::unique_lock<std::mutex> lock(mutex_);
        armed_ = false;
        cv_.wait(lock, [this] { return !running_; });
    }

    MCQNET_NODISCARD
    inline bool armed() const noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        return armed_;
    }

private:
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    CallbackFn callback_ { nullptr };
    void* context_ { nullptr };
    bool armed_ { true };
    bool running_ { false };
};

class CancelState {
public:
    CancelState() = default;
    CancelState(const CancelState&) = delete;
    CancelState& operator=(const CancelState&) = delete;

    inline bool stop_requested() const noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        return stop_requested_;
    }

    inline bool request_stop() {
        std::vector<std::shared_ptr<CancelCallbackState>> callbacks;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if ( stop_requested_ ) {
                return false;
            }
            stop_requested_ = true;
            callbacks.swap(callbacks_);
        }

        for ( const auto& callback : callbacks ) {
            if ( callback != nullptr ) {
                callback->invoke();
            }
        }
        return true;
    }

    MCQNET_NODISCARD
    inline bool add_callback(const std::shared_ptr<CancelCallbackState>& callback) {
        std::lock_guard<std::mutex> lock(mutex_);
        if ( stop_requested_ ) {
            return false;
        }
        callbacks_.push_back(callback);
        return true;
    }

    inline void remove_callback(const std::shared_ptr<CancelCallbackState>& callback) noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        callbacks_.erase(
            std::remove_if(
                callbacks_.begin(),
                callbacks_.end(),
                [&callback](const std::shared_ptr<CancelCallbackState>& candidate) { return candidate == callback; }),
            callbacks_.end());
    }

private:
    mutable std::mutex mutex_;
    bool stop_requested_ { false };
    std::vector<std::shared_ptr<CancelCallbackState>> callbacks_;
};

} // namespace detail

class CancelRegistration {
public:
    using CallbackFn = detail::CancelCallbackState::CallbackFn;

    CancelRegistration() noexcept = default;

    CancelRegistration(const CancelToken& token, CallbackFn callback, void* context) {
        reset(token, callback, context);
    }

    CancelRegistration(const CancelRegistration&) = delete;
    CancelRegistration& operator=(const CancelRegistration&) = delete;

    CancelRegistration(CancelRegistration&& other) noexcept
        : state_(std::move(other.state_))
        , callback_(std::move(other.callback_)) { }

    CancelRegistration& operator=(CancelRegistration&& other) noexcept {
        if ( this != std::addressof(other) ) {
            reset();
            state_ = std::move(other.state_);
            callback_ = std::move(other.callback_);
        }
        return *this;
    }

    ~CancelRegistration() { reset(); }

    inline void reset() noexcept {
        if ( callback_ == nullptr ) {
            state_.reset();
            return;
        }

        if ( state_ != nullptr ) {
            state_->remove_callback(callback_);
        }

        callback_->disarm_and_wait();
        callback_.reset();
        state_.reset();
    }

    inline void reset(const CancelToken& token, CallbackFn callback, void* context);

    MCQNET_NODISCARD
    inline bool active() const noexcept { return callback_ != nullptr && callback_->armed(); }

private:
    std::shared_ptr<detail::CancelState> state_ { };
    std::shared_ptr<detail::CancelCallbackState> callback_ { };
};

class CancelSource {
public:
    CancelSource()
        : state_(std::make_shared<detail::CancelState>()) { }

    CancelSource(const CancelSource&) = default;
    CancelSource& operator=(const CancelSource&) = default;
    CancelSource(CancelSource&&) noexcept = default;
    CancelSource& operator=(CancelSource&&) noexcept = default;

    MCQNET_NODISCARD
    inline bool valid() const noexcept { return state_ != nullptr; }

    MCQNET_NODISCARD
    inline explicit operator bool() const noexcept { return valid(); }

    MCQNET_NODISCARD
    inline bool stop_requested() const noexcept { return state_ != nullptr && state_->stop_requested(); }

    inline bool cancel() {
        if ( state_ == nullptr ) {
            return false;
        }
        return state_->request_stop();
    }

    MCQNET_NODISCARD
    inline CancelToken token() const noexcept;

private:
    std::shared_ptr<detail::CancelState> state_ { };
};

class CancelToken {
public:
    CancelToken() noexcept = default;

    MCQNET_NODISCARD
    inline bool valid() const noexcept { return state_ != nullptr; }

    MCQNET_NODISCARD
    inline explicit operator bool() const noexcept { return valid(); }

    MCQNET_NODISCARD
    inline bool stop_requested() const noexcept { return state_ != nullptr && state_->stop_requested(); }

private:
    friend class CancelRegistration;
    friend class CancelSource;

    explicit CancelToken(std::shared_ptr<detail::CancelState> state) noexcept
        : state_(std::move(state)) { }

    std::shared_ptr<detail::CancelState> state_ { };
};

inline CancelToken CancelSource::token() const noexcept { return CancelToken { state_ }; }

inline void CancelRegistration::reset(const CancelToken& token, CallbackFn callback, void* context) {
    reset();

    if ( !token.valid() || callback == nullptr ) {
        return;
    }

    auto callback_state = std::make_shared<detail::CancelCallbackState>(callback, context);
    if ( !token.state_->add_callback(callback_state) ) {
        callback_state->invoke();
        return;
    }

    state_ = token.state_;
    callback_ = std::move(callback_state);
}

} // namespace mcqnet::runtime

namespace mcqnet {

using runtime::CancelRegistration;
using runtime::CancelSource;
using runtime::CancelToken;

} // namespace mcqnet
