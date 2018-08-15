#pragma once

#include "sodium.h"
#include "win/encoding.h"
#include <string>

inline std::string getRandom(size_t size) {
    std::string s(size, 0);
    randombytes_buf(s.data(), size);
    return s;
}

inline std::wstring keyToDisplayStr(const std::string& key) {
    // includes trailing 0
    size_t b64size = sodium_base64_ENCODED_LEN(key.size(), sodium_base64_VARIANT_ORIGINAL_NO_PADDING);
    std::string s(b64size, '\0');
    sodium_bin2base64(s.data(), b64size, (const unsigned char*)key.data(), key.size(), sodium_base64_VARIANT_ORIGINAL_NO_PADDING);
    // remove trailing 0
    s.pop_back();
    return Utf8ToUtf16(s);
}

class GenericHash {
public:
    GenericHash() {
        crypto_generichash_init(&hash, NULL, 0, crypto_generichash_BYTES);
    }
    void update(const std::string& s) {
        crypto_generichash_update(&hash, (const unsigned char*)s.data(), s.size());
    }
    std::string result() {
        std::string s(crypto_generichash_BYTES, '\0');
        crypto_generichash_final(&hash, (unsigned char*)s.data(), crypto_generichash_BYTES);
        return s;
    }
    std::string resultAndContinue() {
        crypto_generichash_state copy = hash;
        std::string s(crypto_generichash_BYTES, '\0');
        crypto_generichash_final(&copy, (unsigned char*)s.data(), crypto_generichash_BYTES);
        return s;
    }
private:
    crypto_generichash_state hash;
};
