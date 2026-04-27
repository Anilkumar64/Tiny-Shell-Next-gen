#pragma once
#include <coroutine>
#include <iostream>

namespace tsh {
    struct Task {
        struct promise_type {
            Task get_return_object() { 
                return Task{std::coroutine_handle<promise_type>::from_promise(*this)}; 
            }
            std::suspend_never initial_suspend() { return {}; }
            std::suspend_always final_suspend() noexcept { return {}; }
            void return_void() {}
            void unhandled_exception() { std::terminate(); }
        };
        std::coroutine_handle<promise_type> handle;
        explicit Task(std::coroutine_handle<promise_type> h) : handle(h) {}
        ~Task() { if (handle) handle.destroy(); }
    };
}
