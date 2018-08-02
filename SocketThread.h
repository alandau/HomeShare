#pragma once

#include <string>
#include "Logger.h"

class SocketThread;

struct Contact {
    const char* hostname;
    unsigned short port;
};

class SocketThreadApi {
public:
    void Init(Logger* logger, HWND notifyHwnd);
    ~SocketThreadApi();
    void SendFile(const Contact& c, const std::wstring& filename);
private:
    SocketThread* d = nullptr;
};
