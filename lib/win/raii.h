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
