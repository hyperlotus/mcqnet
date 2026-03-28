#pragma once

// socket_operations.h
// 集中承载 socket async operation 相关类型：
// - detail::SocketOperationBase
// - ConnectOperation
// - AcceptOperation
// - ReadOperation
// - WriteOperation
//
// 交接说明：
// - 本文件已经把 operation 形状、结果类型和 cancel wiring 固定下来
// - 但 backend/operation 侧仍有未完成实现
// - 下面统一用 `TODO(operation)` / `TODO(backend)` 标出待补点
// - 原则上不要改 public 形状，优先在 TODO 所在位置补实现

#include <mcqnet/core/error.h>
#include <mcqnet/core/exception.h>
#include <mcqnet/detail/operation_awaiter.h>
#include <mcqnet/detail/operation_base.h>
#include <mcqnet/net/socket_handle.h>
#include <mcqnet/net/socket_result.h>
#include <mcqnet/net/tcp_listener.h>
#include <mcqnet/net/tcp_stream.h>
#include <mcqnet/runtime/io_operation.h>
#include <string>
#include <string_view>
#include <utility>

namespace mcqnet::net::detail {

MCQNET_NODISCARD
inline runtime::Handle select_operation_runtime_handle(
    runtime::Handle explicit_runtime_handle, runtime::Handle object_runtime_handle) noexcept {
    return explicit_runtime_handle.valid() ? explicit_runtime_handle : object_runtime_handle;
}

// 所有 socket operation 共享的底座。
// 固定协议：
// 1. submit() 先做一次同步 socket 尝试
// 2. 若结果是 would_block，再注册 cancel 并提交 backend
// 3. backend 完成时回到派生类 complete_* helper
// 4. await_resume() 只做异常/取消收束，不重新解释 IO 结果
class SocketOperationBase : public runtime::IoOperationBase {
public:
    SocketOperationBase(
        runtime::IoOperationKind kind,
        SocketHandle socket,
        runtime::Handle runtime_handle = {},
        runtime::CancelToken cancel_token = {},
        const char* debug_tag = nullptr) noexcept
        : runtime::IoOperationBase(kind, socket, runtime_handle, std::move(cancel_token)) {
        set_debug_tag(debug_tag);
    }

    SocketOperationBase(const SocketOperationBase&) = delete;
    SocketOperationBase& operator=(const SocketOperationBase&) = delete;

protected:
    // TODO(operation): 若未来不同平台/不同 backend 需要不同的 cancel 提交流程，
    // 优先在这里统一扩展，不要把四个 operation 各自改散。
    inline void submit_to_backend_after_would_block() {
        arm_cancel_registration(&SocketOperationBase::cancel_from_source, this);
        if ( is_completed() ) {
            return;
        }
        submit_to_io_backend();
    }

    inline void throw_if_cancelled(std::string_view what) const {
        if ( state() != ::mcqnet::detail::OperationState::cancelled ) {
            return;
        }

        const core::errc error =
            completion_error() == 0 ? core::errc::operation_aborted : static_cast<core::errc>(completion_error());
        core::throw_runtime_error(core::error_code { error }, std::string(what));
    }

private:
    static inline void cancel_from_source(void* context) noexcept {
        MCQNET_ASSERT(context != nullptr);
        static_cast<SocketOperationBase*>(context)->request_backend_cancel();
    }
};

} // namespace mcqnet::net::detail

namespace mcqnet::net {

// ConnectOperation 固定返回 ConnectResult，而不是 bool/void。
// 这保证 non-blocking connect 的“进行中”状态不会在 awaitable 层被抹平。
class ConnectOperation final : public detail::SocketOperationBase {
public:
    ConnectOperation(
        TcpStream& stream,
        SocketAddress remote_address,
        runtime::Handle runtime_handle = {},
        runtime::CancelToken cancel_token = {}) noexcept
        : detail::SocketOperationBase(
              runtime::IoOperationKind::connect,
              stream.socket(),
              detail::select_operation_runtime_handle(runtime_handle, stream.runtime_handle()),
              std::move(cancel_token),
              "ConnectOperation")
        , stream_(&stream)
        , remote_address_(remote_address) { }

    ConnectOperation(const ConnectOperation&) = delete;
    ConnectOperation& operator=(const ConnectOperation&) = delete;

    MCQNET_NODISCARD
    inline auto operator co_await() & noexcept { return ::mcqnet::detail::make_operation_awaiter(*this); }

    auto operator co_await() && = delete;

    MCQNET_NODISCARD
    inline const TcpStream* stream() const noexcept { return stream_; }

    MCQNET_NODISCARD
    inline const SocketAddress& remote_address() const noexcept { return remote_address_; }

    inline void set_remote_address(SocketAddress remote_address) noexcept { remote_address_ = remote_address; }

    MCQNET_NODISCARD
    inline const ConnectResult& result() const noexcept { return result_; }

    // TODO(backend): 真实 backend 完成 connect 时，应该优先调用这个 helper，
    // 并在调用前把 backend completion / SO_ERROR 翻译成最终 ConnectResult。
    // 约束：不要在 backend 层直接调用 finish()，否则会丢掉 completed 位语义。
    inline void complete_connect(ConnectResult result) {
        result_ = result;
        finish(result_.completed ? 1U : 0U, static_cast<std::int32_t>(result_.error.value));
    }

    inline void submit() {
        refresh_binding_from_stream();
        validate_target();

        // TODO(operation): 这里依赖 TcpStream::connect() 的同步尝试语义。
        // 在 linux_socket_api.h 落地后，应确认：
        // - 立刻成功 => completed=true
        // - EINPROGRESS/EALREADY => would_block/in_progress
        // - 其它错误 => 具体 error
        const ConnectResult sync_result = stream_->connect(remote_address_);
        if ( sync_result.in_progress() ) {
            // TODO(backend): completion backend 需要在 connect 完成时回调 complete_connect()，
            // 而不是只写 finish()，否则会丢掉 richer connect state。
            submit_to_backend_after_would_block();
            return;
        }
        complete_connect(sync_result);
    }

    MCQNET_NODISCARD
    inline ConnectResult await_resume() {
        rethrow_if_exception();
        // TODO(operation): 当前只有 cooperative cancel 走异常；
        // 若未来要进一步细分 timed_out / backend-aborted，可在这里统一收束。
        throw_if_cancelled("ConnectOperation was cancelled");
        return result_;
    }

private:
    inline void refresh_binding_from_stream() noexcept {
        MCQNET_ASSERT(stream_ != nullptr);
        set_socket(stream_->socket());
        if ( !has_runtime_handle() && stream_->runtime_handle().valid() ) {
            set_runtime_handle(stream_->runtime_handle());
        }
    }

    inline void validate_target() const {
        if ( stream_ == nullptr ) {
            core::throw_net_error(
                core::error_code { core::errc::invalid_argument }, "ConnectOperation requires a target TcpStream");
        }
        if ( !stream_->valid() ) {
            core::throw_net_error(
                core::error_code { core::errc::invalid_argument }, "ConnectOperation requires a valid TcpStream socket");
        }
        if ( !remote_address_.valid() ) {
            core::throw_net_error(
                core::error_code { core::errc::invalid_argument }, "ConnectOperation requires a valid remote address");
        }
    }

private:
    TcpStream* stream_ { nullptr };
    SocketAddress remote_address_ { };
    ConnectResult result_ { };
};

// AcceptOperation 保留 AcceptResult，避免把 accepted socket 和 peer address 拆散。
// 如果以后要直接返回 TcpStream，应额外包一层 convenience API，而不是改基础协议。
class AcceptOperation final : public detail::SocketOperationBase {
public:
    AcceptOperation(
        TcpListener& listener,
        runtime::Handle runtime_handle = {},
        runtime::CancelToken cancel_token = {}) noexcept
        : detail::SocketOperationBase(
              runtime::IoOperationKind::accept,
              listener.socket(),
              detail::select_operation_runtime_handle(runtime_handle, listener.runtime_handle()),
              std::move(cancel_token),
              "AcceptOperation")
        , listener_(&listener) { }

    AcceptOperation(const AcceptOperation&) = delete;
    AcceptOperation& operator=(const AcceptOperation&) = delete;

    MCQNET_NODISCARD
    inline auto operator co_await() & noexcept { return ::mcqnet::detail::make_operation_awaiter(*this); }

    auto operator co_await() && = delete;

    MCQNET_NODISCARD
    inline const TcpListener* listener() const noexcept { return listener_; }

    MCQNET_NODISCARD
    inline const AcceptResult& result() const noexcept { return result_; }

    MCQNET_NODISCARD
    inline const SocketAddress& peer_address() const noexcept { return result_.peer_address; }

    MCQNET_NODISCARD
    inline SocketHandle accepted_socket() const noexcept { return result_.socket; }

    // TODO(backend): 真实 backend accept 完成时，应该把 accepted socket 和 peer address
    // 组装成 AcceptResult 后走这个 helper。
    // 约束：无论 completion 来源是 poll/epoll/io_uring，最终都应收束到同一结果结构。
    inline void complete_accept(AcceptResult result) {
        result_ = result;
        finish(result_.socket.valid() ? 1U : 0U, static_cast<std::int32_t>(result_.error.value));
    }

    inline void submit() {
        refresh_binding_from_listener();
        validate_target();

        // TODO(operation): 这里依赖 TcpListener::accept_raw() 的同步尝试语义。
        // 在 linux_socket_api.h 落地后，应确认：
        // - 无连接 => would_block
        // - 有连接 => accepted socket + peer address
        // - 其它错误 => 具体 error
        const AcceptResult sync_result = listener_->accept_raw();
        if ( sync_result.would_block() ) {
            // TODO(backend): completion backend 需要在 accept 完成时回调 complete_accept()，
            // 以保留 peer address，而不是只传一个整型 result。
            submit_to_backend_after_would_block();
            return;
        }
        complete_accept(sync_result);
    }

    MCQNET_NODISCARD
    inline AcceptResult await_resume() {
        rethrow_if_exception();
        // TODO(operation): 若未来要在更高层直接包装成 TcpStream，可在 await_resume()
        // 之上再加 convenience API，不要改掉这里返回 AcceptResult 的基础协议。
        throw_if_cancelled("AcceptOperation was cancelled");
        return result_;
    }

private:
    inline void refresh_binding_from_listener() noexcept {
        MCQNET_ASSERT(listener_ != nullptr);
        set_socket(listener_->socket());
        if ( !has_runtime_handle() && listener_->runtime_handle().valid() ) {
            set_runtime_handle(listener_->runtime_handle());
        }
    }

    inline void validate_target() const {
        if ( listener_ == nullptr ) {
            core::throw_net_error(
                core::error_code { core::errc::invalid_argument }, "AcceptOperation requires a target TcpListener");
        }
        if ( !listener_->valid() ) {
            core::throw_net_error(
                core::error_code { core::errc::invalid_argument }, "AcceptOperation requires a valid listener socket");
        }
    }

private:
    TcpListener* listener_ { nullptr };
    AcceptResult result_ { };
};

// ReadOperation 只表达一次 read_some 风格尝试。
// partial read、EOF、would_block 都由 SocketIoResult 表达，不在 await_resume() 里改写。
class ReadOperation final : public detail::SocketOperationBase {
public:
    ReadOperation(
        TcpStream& stream,
        MutableBuffer buffer,
        runtime::Handle runtime_handle = {},
        runtime::CancelToken cancel_token = {}) noexcept
        : detail::SocketOperationBase(
              runtime::IoOperationKind::receive,
              stream.socket(),
              detail::select_operation_runtime_handle(runtime_handle, stream.runtime_handle()),
              std::move(cancel_token),
              "ReadOperation")
        , stream_(&stream)
        , buffer_(buffer) { }

    ReadOperation(const ReadOperation&) = delete;
    ReadOperation& operator=(const ReadOperation&) = delete;

    MCQNET_NODISCARD
    inline auto operator co_await() & noexcept { return ::mcqnet::detail::make_operation_awaiter(*this); }

    auto operator co_await() && = delete;

    MCQNET_NODISCARD
    inline const TcpStream* stream() const noexcept { return stream_; }

    MCQNET_NODISCARD
    inline MutableBuffer buffer() const noexcept { return buffer_; }

    inline void set_buffer(MutableBuffer buffer) noexcept { buffer_ = buffer; }

    MCQNET_NODISCARD
    inline const SocketIoResult& result() const noexcept { return result_; }

    // TODO(backend): 真实 backend read 完成时，应该把 transferred/error 翻译成 SocketIoResult，
    // 再走这个 helper。
    // 约束：buffer 生命周期由调用方负责，operation/backend 不拥有底层内存。
    inline void complete_read(SocketIoResult result) {
        result_ = result;
        finish(static_cast<std::uint32_t>(result_.transferred), static_cast<std::int32_t>(result_.error.value));
    }

    inline void submit() {
        refresh_binding_from_stream();
        validate_target();

        // TODO(operation): 这里依赖 TcpStream::read_some() 的同步尝试语义。
        // 在 linux_socket_api.h 落地后，应确认：
        // - n > 0 => success
        // - n == 0 => EOF
        // - EAGAIN/EWOULDBLOCK => would_block
        // - 其它错误 => 具体 error
        const SocketIoResult sync_result = stream_->read_some(buffer_);
        if ( sync_result.would_block() ) {
            // TODO(backend): completion backend 需要在 read 完成时回调 complete_read()，
            // 并保证 buffer 生命周期契约由调用方承担。
            submit_to_backend_after_would_block();
            return;
        }
        complete_read(sync_result);
    }

    MCQNET_NODISCARD
    inline SocketIoResult await_resume() {
        rethrow_if_exception();
        // TODO(operation): 当前 read 的 EOF 仍走返回值，而不是异常；保持这个协议不要改散。
        throw_if_cancelled("ReadOperation was cancelled");
        return result_;
    }

private:
    inline void refresh_binding_from_stream() noexcept {
        MCQNET_ASSERT(stream_ != nullptr);
        set_socket(stream_->socket());
        if ( !has_runtime_handle() && stream_->runtime_handle().valid() ) {
            set_runtime_handle(stream_->runtime_handle());
        }
    }

    inline void validate_target() const {
        if ( stream_ == nullptr ) {
            core::throw_net_error(
                core::error_code { core::errc::invalid_argument }, "ReadOperation requires a target TcpStream");
        }
        if ( !stream_->valid() ) {
            core::throw_net_error(
                core::error_code { core::errc::invalid_argument }, "ReadOperation requires a valid TcpStream socket");
        }
        if ( !buffer_ && !buffer_.empty() ) {
            core::throw_net_error(
                core::error_code { core::errc::invalid_argument }, "ReadOperation requires a valid buffer view");
        }
    }

private:
    TcpStream* stream_ { nullptr };
    MutableBuffer buffer_ { };
    SocketIoResult result_ { };
};

// WriteOperation 与 ReadOperation 对称，表达一次 write_some 风格尝试。
// 若未来补 send_all/write_all，应建立在这个基础 operation 之上循环，而不是改协议。
class WriteOperation final : public detail::SocketOperationBase {
public:
    WriteOperation(
        TcpStream& stream,
        ConstBuffer buffer,
        runtime::Handle runtime_handle = {},
        runtime::CancelToken cancel_token = {}) noexcept
        : detail::SocketOperationBase(
              runtime::IoOperationKind::send,
              stream.socket(),
              detail::select_operation_runtime_handle(runtime_handle, stream.runtime_handle()),
              std::move(cancel_token),
              "WriteOperation")
        , stream_(&stream)
        , buffer_(buffer) { }

    WriteOperation(const WriteOperation&) = delete;
    WriteOperation& operator=(const WriteOperation&) = delete;

    MCQNET_NODISCARD
    inline auto operator co_await() & noexcept { return ::mcqnet::detail::make_operation_awaiter(*this); }

    auto operator co_await() && = delete;

    MCQNET_NODISCARD
    inline const TcpStream* stream() const noexcept { return stream_; }

    MCQNET_NODISCARD
    inline ConstBuffer buffer() const noexcept { return buffer_; }

    inline void set_buffer(ConstBuffer buffer) noexcept { buffer_ = buffer; }

    MCQNET_NODISCARD
    inline const SocketIoResult& result() const noexcept { return result_; }

    // TODO(backend): 真实 backend write 完成时，应该把 transferred/error 翻译成 SocketIoResult，
    // 再走这个 helper。
    // 约束：这里允许 partial write；“是否写满”由更高层策略决定。
    inline void complete_write(SocketIoResult result) {
        result_ = result;
        finish(static_cast<std::uint32_t>(result_.transferred), static_cast<std::int32_t>(result_.error.value));
    }

    inline void submit() {
        refresh_binding_from_stream();
        validate_target();

        // TODO(operation): 这里依赖 TcpStream::write_some() 的同步尝试语义。
        // 在 linux_socket_api.h 落地后，应确认：
        // - n >= 0 => success
        // - EAGAIN/EWOULDBLOCK => would_block
        // - EPIPE/ECONNRESET 等 => 具体 error
        const SocketIoResult sync_result = stream_->write_some(buffer_);
        if ( sync_result.would_block() ) {
            // TODO(backend): completion backend 需要在 write 完成时回调 complete_write()，
            // 而不是只回一个裸整型字节数。
            submit_to_backend_after_would_block();
            return;
        }
        complete_write(sync_result);
    }

    MCQNET_NODISCARD
    inline SocketIoResult await_resume() {
        rethrow_if_exception();
        // TODO(operation): 当前 write 的 broken_pipe/connection_reset 仍走返回值，而不是异常。
        throw_if_cancelled("WriteOperation was cancelled");
        return result_;
    }

private:
    inline void refresh_binding_from_stream() noexcept {
        MCQNET_ASSERT(stream_ != nullptr);
        set_socket(stream_->socket());
        if ( !has_runtime_handle() && stream_->runtime_handle().valid() ) {
            set_runtime_handle(stream_->runtime_handle());
        }
    }

    inline void validate_target() const {
        if ( stream_ == nullptr ) {
            core::throw_net_error(
                core::error_code { core::errc::invalid_argument }, "WriteOperation requires a target TcpStream");
        }
        if ( !stream_->valid() ) {
            core::throw_net_error(
                core::error_code { core::errc::invalid_argument }, "WriteOperation requires a valid TcpStream socket");
        }
        if ( !buffer_ && !buffer_.empty() ) {
            core::throw_net_error(
                core::error_code { core::errc::invalid_argument }, "WriteOperation requires a valid buffer view");
        }
    }

private:
    TcpStream* stream_ { nullptr };
    ConstBuffer buffer_ { };
    SocketIoResult result_ { };
};

} // namespace mcqnet::net

namespace mcqnet {

using net::AcceptOperation;
using net::ConnectOperation;
using net::ReadOperation;
using net::WriteOperation;

} // namespace mcqnet
