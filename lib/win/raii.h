#pragma once

#include <windows.h>
#include <comdef.h>
#include <stdexcept>

struct ComInit {
    ComInit() {
        if (!SUCCEEDED(CoInitialize(NULL))) {
            throw std::runtime_error("Can't initialize COM");
        }
    }
    ~ComInit() {
        CoUninitialize();
    }
};

struct ScopeGuardDummy {};

template <class F>
struct ScopeGuard {
    ScopeGuard(F f)
        : f(std::forward<F>(f))
    {}
    ~ScopeGuard() {
        f();
    }
private:
    F f;
};

template <class F>
ScopeGuard<F> operator +(ScopeGuardDummy, F f) {
    return ScopeGuard<F>(std::forward<F>(f));
}

#define SCOPE_EXIT auto tmp_ ## LINE = ScopeGuardDummy() + [&]()
