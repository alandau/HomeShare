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

#define CONCAT2(a, b) a ## b
#define CONCAT(a, b) CONCAT2(a, b)
#define SCOPE_EXIT auto CONCAT(tmp_, __LINE__) = ScopeGuardDummy() + [&]()
