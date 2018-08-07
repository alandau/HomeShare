#pragma once

#include "../fmt/format.h"

std::string Utf16ToUtf8(const std::wstring& s) {
    return fmt::internal::utf16_to_utf8(s).str();
}

std::wstring Utf8ToUtf16(const std::string& s) {
    return fmt::internal::utf8_to_utf16(s).str();
}
