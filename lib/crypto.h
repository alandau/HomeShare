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

inline std::string displayStrToKey(const std::wstring& disp) {
    std::string utf8 = Utf16ToUtf8(disp);
    std::string key(crypto_sign_PUBLICKEYBYTES, '\0');
    size_t binlen;
    if (sodium_base642bin((unsigned char*)key.data(), crypto_sign_PUBLICKEYBYTES, utf8.c_str(), utf8.size(),
        NULL, &binlen, NULL, sodium_base64_VARIANT_ORIGINAL_NO_PADDING) != 0 || binlen != crypto_sign_PUBLICKEYBYTES) {
        return std::string();
    }
    return key;
}

class GenericHash {
public:
    GenericHash() {
        reset();
    }
    void reset() {
        crypto_generichash_init(&hash, NULL, 0, crypto_generichash_BYTES);
    }
    void update(const std::string& s) {
        crypto_generichash_update(&hash, (const unsigned char*)s.data(), s.size());
    }
    void update(const unsigned char* buf, size_t size) {
        crypto_generichash_update(&hash, buf, size);
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
