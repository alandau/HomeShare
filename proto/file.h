#pragma once

#include "Serializer.h"

enum MessageType {
    SENDFILE_HEADER = 1,
    SENDFILE_DATA = 2,
    SENDFILE_TRAILER = 3,
    SENDFILE_LIST = 4,
};

struct Header {
    uint16_t streamId;
    uint16_t type;
};

struct SendFileListHeader {
    uint32_t count;
    uint64_t size;

    template <class X>
    void visit(X& x) {
        x(1, count);
        x(2, size);
    }
};

struct SendFileHeader {
    std::string name;
    uint64_t size;

    template <class X>
    void visit(X& x) {
        x(1, name);
        x(2, size);
    }
};

struct SendFileTrailer {
    std::string checksum;

    template <class X>
    void visit(X& x) {
        x(1, checksum);
    }
};
