#pragma once

#include <string>
#include <functional>
#include "lib/Buffer.h"
#include "Logger.h"

class SocketThread;

struct Contact {
    const char* hostname;
    unsigned short port;
};

namespace std {

template<> struct hash<Contact> {
    typedef Contact argument_type;
    typedef std::size_t result_type;
    result_type operator()(const argument_type& c) const noexcept {
        const result_type h1(std::hash<std::string>()(c.hostname ? c.hostname : ""));
        const result_type h2(std::hash<unsigned short>()(c.port));
        return h1 ^ (h2 << 1);
    }
};

}

inline bool operator ==(const Contact& c1, const Contact& c2) {
    return c1.port == c2.port && !strcmp(c1.hostname, c2.hostname);
}

class SocketThreadApi {
public:
    void Init(Logger* logger, HWND notifyHwnd);
    ~SocketThreadApi();
    void setQueueEmptyCb(std::function<void(const Contact& c)> queueEmptyCb);
    void SocketThreadApi::setOnMessageCb(std::function<void(const Contact& c, Buffer::UniquePtr message)> onMessageCb);
    // Return true if should cork (this buffer is still enqueued)
    bool SendBuffer(const Contact& c, Buffer* buffer);
private:
    SocketThread* d = nullptr;
};
