#pragma once

#include "Serializer.h"

enum MessageType {
    SENDFILE_REQ = 1,
    SENDFILE_ACK = 2,
    SENDFILE_DATA = 3,
    SENDFILE_TRAILER = 4,
};

struct Header {
    uint16_t streamId;
    uint16_t type;

    template <class X>
    void visit(X& x) {
        x(1, streamId);
        x(2, type);
    }
};

struct SendFileReq {
    std::string name;
    uint64_t size;

    template <class X>
    void visit(X& x) {
        x(1, name);
        x(2, size);
    }
};

struct SendFileResp {
    //std::string name;
    uint64_t size;
    std::optional<uint32_t> opt;

    template <class X>
    void visit(X& x) {
        deprecated<std::string> tmp;
        x(1, tmp);
        x(2, size);
        x(3, opt);
    }
};
