#pragma once

#include "lib/sodium.h"
#include <string>

inline std::string getRandom(size_t size) {
    std::string s(size, 0);
    randombytes_buf(s.data(), size);
    return s;
}