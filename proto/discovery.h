#pragma once

#include "Serializer.h"

enum {
    DISCOVERY_REQ_MAGIC = 0x41485348,   // "HSHA"
    DISCOVERY_RESP_MAGIC = 0x42485348,  // "HSHB"
};

struct DiscoveryResp {
    std::string pubkey;
    std::string ip;
    uint16_t port;

    template <class X>
    void visit(X& x) {
        x(1, pubkey);
        x(2, ip);
        x(3, port);
    }
};
