#pragma once

#include <string>
#include <functional>
#include "lib/Buffer.h"
#include "Logger.h"

class SocketThread;

struct Contact {
    std::string pubkey;
};

namespace std {

template<> struct hash<Contact> {
    typedef Contact argument_type;
    typedef std::size_t result_type;
    result_type operator()(const argument_type& c) const noexcept {
        return std::hash<std::string>()(c.pubkey);
    }
};

}

inline bool operator ==(const Contact& c1, const Contact& c2) {
    return c1.pubkey == c2.pubkey;
}

class SocketThreadApi {
public:
    void Init(Logger* logger, HWND notifyHwnd);
    ~SocketThreadApi();
    void setQueueEmptyCb(std::function<void(const Contact& c)> queueEmptyCb);
    void setOnMessageCb(std::function<void(const Contact& c, Buffer::UniquePtr message)> onMessageCb);
    void setOnConnectCb(std::function<void(const Contact& c, bool connected)> cb);
    void Connect(const Contact& c, const std::string& hostname, uint16_t port);
    // Return true if should cork (this buffer is still enqueued)
    bool SendBuffer(const Contact& c, Buffer* buffer);
private:
    SocketThread* d = nullptr;
};
