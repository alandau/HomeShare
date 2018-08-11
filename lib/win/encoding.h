#pragma once

#include "../fmt/format.h"

inline std::string Utf16ToUtf8(const std::wstring& s) {
    return fmt::internal::utf16_to_utf8(s).str();
}

inline std::wstring Utf8ToUtf16(const std::string& s) {
    return fmt::internal::utf8_to_utf16(s).str();
}

inline std::wstring errstr(int err) {
    wchar_t buf[256];
    buf[0] = L'\0';
    int res = FormatMessage(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        NULL, err, 0, buf, sizeof(buf) / sizeof(buf[0]), NULL);
    return fmt::format(L"{} ({})", err, buf);
}
