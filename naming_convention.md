# MCQNet Naming Convention

## 1. Goal

MCQNet should borrow Tokio's semantic vocabulary while keeping a natural C++
surface style.

The rule is:

- Align concepts with Tokio
- Keep C++ naming and API shapes
- Avoid needless churn in existing code

In short:

- Tokio words
- C++ type and function style

## 2. General Rules

### 2.1 Namespace and Module Names

Use lowercase module namespaces:

- `mcqnet::runtime`
- `mcqnet::task`
- `mcqnet::net`
- `mcqnet::time`
- `mcqnet::sync`
- `mcqnet::memory`
- `mcqnet::core`
- `mcqnet::detail`

Guideline:

- User-facing APIs live in semantic modules
- `detail` stays internal
- `core` is for foundational utilities, not async runtime APIs

### 2.2 Type Names

Use UpperCamelCase for types:

- `Runtime`
- `Handle`
- `Task`
- `JoinHandle`
- `TcpStream`
- `TcpListener`
- `UdpSocket`

Avoid vague names such as:

- `Engine`
- `Manager`
- `Service`
- `Helper`

### 2.3 Function Names

Use `snake_case` for functions and methods:

- `spawn`
- `sleep_for`
- `sleep_until`
- `timeout`
- `connect`
- `accept`
- `post`
- `run`
- `run_one`
- `stop`

### 2.4 File Names

Use `snake_case` for header and source files:

- `runtime.h`
- `handle.h`
- `task.h`
- `join_handle.h`
- `tcp_stream.h`
- `tcp_listener.h`
- `udp_socket.h`

### 2.5 Enum and Error Names

Use `snake_case` for enum values and error codes:

- `invalid_argument`
- `timed_out`
- `runtime_stopped`
- `connection_reset`

This matches the current `errc` style and should remain stable.

### 2.6 Macro Names

Use `MCQNET_*` uppercase macros only for configuration and portability:

- `MCQNET_ASSERT`
- `MCQNET_PLATFORM_WINDOWS`
- `MCQNET_HAS_COROUTINE`

New feature APIs should not be exposed as macros when a normal type or function
can express the same thing.

### 2.7 Member Names

Private data members should keep the trailing underscore style already used in
the codebase:

- `handle_`
- `state_`
- `schedule_fn_`

## 3. Public API Vocabulary

### 3.1 Runtime Layer

Preferred public names:

- `mcqnet::runtime::Runtime`
- `mcqnet::runtime::Handle`

Notes:

- `Runtime` is the primary user-facing execution context
- `Handle` is a lightweight reference to an existing runtime
- `Scheduler` may exist internally, but it should not be the primary public
  concept

Recommendation:

- Prefer `Runtime` over `Executor` for the top-level API
- Prefer `Handle` over exposing raw scheduler callbacks

### 3.2 Task Layer

Preferred public names:

- `mcqnet::task::Task<T>`
- `mcqnet::task::JoinHandle<T>`
- `mcqnet::task::spawn`

Notes:

- `spawn` and `JoinHandle` are already aligned with Tokio semantics
- `Task<T>` is the correct C++ abstraction here; do not force a Rust-style
  `Future` naming model

### 3.3 Network Layer

Preferred public names:

- `mcqnet::net::TcpStream`
- `mcqnet::net::TcpListener`
- `mcqnet::net::UdpSocket`

If Unix-domain sockets or platform-specific handles are added later, follow the
same pattern:

- `UnixStream`
- `UnixListener`
- `Socket`

### 3.4 Time Layer

Preferred public names:

- `mcqnet::time::sleep_for`
- `mcqnet::time::sleep_until`
- `mcqnet::time::timeout`
- `mcqnet::time::interval`

Guideline:

- Time operations should read like actions, not like utility helpers

### 3.5 Sync Layer

Preferred public names:

- `mcqnet::sync::oneshot`
- `mcqnet::sync::mpsc`
- `mcqnet::sync::Notify`
- `mcqnet::sync::Semaphore`
- `mcqnet::sync::Mutex`

Guideline:

- Channel families may use lowercase module names
- Concrete synchronization types use UpperCamelCase

## 4. Directory and Header Layout

Recommended public include layout for the next phase:

```text
mcqnet/include/mcqnet/
├── mcqnet.h
├── runtime/
│   ├── runtime.h
│   └── handle.h
├── task/
│   ├── task.h
│   ├── join_handle.h
│   └── spawn.h
├── net/
│   ├── tcp_listener.h
│   ├── tcp_stream.h
│   └── udp_socket.h
├── time/
│   ├── sleep.h
│   ├── timeout.h
│   └── interval.h
├── sync/
│   ├── oneshot.h
│   ├── mpsc.h
│   ├── notify.h
│   └── semaphore.h
├── memory/
├── core/
├── config/
└── detail/
```

## 5. How This Applies to the Current Codebase

### 5.1 Keep These Names

These already fit the target style and should stay:

- `Task`
- `JoinHandle`
- `spawn`
- `Runtime` for the upcoming runtime type

### 5.2 Module Naming Is Now Canonical

The biggest naming migration has already happened at the module level.

Current canonical task headers are:

- `mcqnet/include/mcqnet/task/task.h`
- `mcqnet/include/mcqnet/task/join_handle.h`
- `mcqnet/include/mcqnet/task/spawn.h`

The old `coroutine/` compatibility layout has been removed from the repository.

Rule:

- Use `task/` for the stable public async API layout
- Do not reintroduce a parallel `coroutine/` public header tree

### 5.3 Keep `detail` Internal

These names are fine as internal implementation details:

- `OperationBase`
- `OperationAwaiter`

They should remain in `mcqnet::detail` unless a real public operation model is
introduced later.

### 5.4 Do Not Repeat Historical Naming Mistakes

Existing compatibility names may stay for source stability, but new APIs should
not copy their style:

- `thorw_runtime_error`
- `make_unqiue`
- `McqnetMemoryResource`

Guideline:

- Do not prefix types with `Mcqnet` when they already live in `mcqnet::`
- Do not preserve misspellings in new APIs

## 6. Root Namespace Policy

Preferred long-term policy:

- `mcqnet` is the umbrella namespace
- User-facing APIs live in semantic sub-namespaces
- The root namespace may re-export a small compatibility set during migration

Suggested compatibility set:

- `mcqnet::Task`
- `mcqnet::JoinHandle`
- `mcqnet::spawn`

Preferred long-term canonical spellings:

- `mcqnet::task::Task`
- `mcqnet::task::JoinHandle`
- `mcqnet::task::spawn`

This gives a migration path without forcing a hard break immediately.

## 7. Naming Rules for the Runtime Work

For the next implementation phase, use these exact names:

- `mcqnet::runtime::Runtime`
- `mcqnet::runtime::Handle`
- `mcqnet::runtime::Runtime::post`
- `mcqnet::runtime::Runtime::run`
- `mcqnet::runtime::Runtime::run_one`
- `mcqnet::runtime::Runtime::stop`
- `mcqnet::runtime::Runtime::spawn`

Internal names that are acceptable but should stay non-primary:

- `Scheduler`
- `ScheduleFn`
- `SchedulerScope`

Rule:

- Users should think in terms of `Runtime` and `Handle`
- The scheduler callback machinery is an internal transport detail

## 8. Migration Strategy

### Phase 1

Add new canonical headers and namespaces:

- `runtime/`
- `task/`

### Phase 2

Move tests, examples, and new code to canonical names:

- Prefer `mcqnet::task::*`
- Prefer `mcqnet::runtime::*`

Remove transitional compatibility header trees once the repository no longer
uses them internally.

### Phase 3

When the runtime and network layers stabilize:

- Reduce root namespace re-exports
- Deprecate old compatibility headers if needed

## 9. Short Version

The naming rule for MCQNet is:

- Tokio semantics
- C++ style
- Module-first organization

Use:

- `Runtime`
- `Handle`
- `Task`
- `JoinHandle`
- `spawn`
- `TcpStream`
- `TcpListener`
- `sleep_for`
- `timeout`

Avoid:

- `Executor` as the primary top-level public type
- vague names like `Manager` or `Engine`
- new APIs under `coroutine/` as the long-term public module name
- new project-prefixed type names inside `mcqnet::`
