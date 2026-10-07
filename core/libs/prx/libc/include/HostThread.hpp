#ifndef CORE_LIBS_PRX_LIBC_INCLUDE_HOSTTHREAD_HPP
#define CORE_LIBS_PRX_LIBC_INCLUDE_HOSTTHREAD_HPP

// Threads on the host's own primitives, with the semantics of std::thread (HostThread) and
// std::jthread (HostStopThread). libstdc++ is a DLL here: std::thread allocates its state object
// inline, with the caller's operator new, and the DLL's thread routine deletes it with the DLL's
// own operator delete. A module with its own operator new and delete (the AGC driver's block
// arena, OperatorNew.cpp) would see one of its blocks reach the UCRT heap's free(). These keep the
// state inside the module: new and delete both run in the caller's code.
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <process.h>
// windows.h macros that collide with identifiers in code including this header.
#undef interface
#undef near
#undef far
#undef small
#undef IN
#undef OUT
#undef OPTIONAL

#include <cerrno>
#include <exception>
#include <functional>
#include <memory>
#include <stop_token>
#include <system_error>
#include <type_traits>
#include <utility>

class HostThread {
public:
    struct State {
        virtual ~State() = default;
        virtual void Run() = 0;
    };

    HostThread() noexcept = default;
    template <typename Callable, typename = std::enable_if_t<!std::is_same_v<std::remove_cvref_t<Callable>, HostThread>>>
    explicit HostThread(Callable&& callable) {
        start(std::make_unique<Impl<std::decay_t<Callable>>>(std::forward<Callable>(callable)));
    }
    HostThread(const HostThread&) = delete;
    HostThread& operator=(const HostThread&) = delete;
    HostThread(HostThread&& other) noexcept : handle(std::exchange(other.handle, nullptr)) {}
    HostThread& operator=(HostThread&& other) noexcept {
        if (joinable()) std::terminate();
        handle = std::exchange(other.handle, nullptr);
        return *this;
    }
    ~HostThread() {
        if (joinable()) std::terminate();
    }

    bool joinable() const noexcept { return handle != nullptr; }
    void join() {
        if (!joinable()) throw std::system_error(std::make_error_code(std::errc::invalid_argument), "joining a host thread that is not joinable");
        HANDLE joined = std::exchange(handle, nullptr);
        WaitForSingleObject(joined, INFINITE);
        CloseHandle(joined);
    }
    void detach() {
        if (!joinable()) throw std::system_error(std::make_error_code(std::errc::invalid_argument), "detaching a host thread that is not joinable");
        CloseHandle(std::exchange(handle, nullptr));
    }
    void swap(HostThread& other) noexcept { std::swap(handle, other.handle); }

private:
    template <typename Callable>
    struct Impl final : State {
        explicit Impl(Callable moved) : callable(std::move(moved)) {}
        void Run() override { callable(); }
        Callable callable;
    };

    static unsigned __stdcall trampoline(void* data) {
        std::unique_ptr<State> state(static_cast<State*>(data));
        state->Run();
        return 0;
    }

    // Owns `state` once the thread runs; a failed start leaves it to the caller's unique_ptr.
    void start(std::unique_ptr<State> state) {
        const auto started = _beginthreadex(nullptr, 0, &trampoline, state.get(), 0, nullptr);
        if (started == 0) throw std::system_error(errno, std::generic_category(), "starting a host thread");
        state.release();
        handle = reinterpret_cast<HANDLE>(started);
    }

    HANDLE handle = nullptr;
};

class HostStopThread {
public:
    HostStopThread() noexcept : source(std::nostopstate) {}
    template <typename Callable, typename = std::enable_if_t<!std::is_same_v<std::remove_cvref_t<Callable>, HostStopThread>>>
    explicit HostStopThread(Callable&& callable) : source(), thread(wrap(std::forward<Callable>(callable), source.get_token())) {}
    HostStopThread(const HostStopThread&) = delete;
    HostStopThread& operator=(const HostStopThread&) = delete;
    HostStopThread(HostStopThread&&) noexcept = default;
    HostStopThread& operator=(HostStopThread&& other) noexcept {
        if (joinable()) {
            request_stop();
            join();
        }
        source = std::move(other.source);
        thread = std::move(other.thread);
        return *this;
    }
    ~HostStopThread() {
        if (joinable()) {
            request_stop();
            join();
        }
    }

    bool joinable() const noexcept { return thread.joinable(); }
    void join() { thread.join(); }
    void detach() { thread.detach(); }
    std::stop_source get_stop_source() noexcept { return source; }
    std::stop_token get_stop_token() const noexcept { return source.get_token(); }
    bool request_stop() noexcept { return source.request_stop(); }

private:
    // The callable takes the stop token first when it can, as with std::jthread.
    template <typename Callable>
    static auto wrap(Callable&& callable, std::stop_token token) {
        if constexpr (std::is_invocable_v<std::decay_t<Callable>, std::stop_token>) {
            return [callable = std::forward<Callable>(callable), token = std::move(token)]() mutable { std::invoke(callable, std::move(token)); };
        } else {
            return [callable = std::forward<Callable>(callable)]() mutable { std::invoke(callable); };
        }
    }

    std::stop_source source;
    HostThread thread;
};
#else
#include <thread>
using HostThread = std::thread;
using HostStopThread = std::jthread;
#endif

#endif
