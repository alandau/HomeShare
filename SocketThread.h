#pragma once

#include <string>

class SocketThread;

struct Contact {
    const char* hostname;
    unsigned short port;
};

class SocketThreadApi {
public:
    void Init(HWND notifyHwnd);
    ~SocketThreadApi();
    void SendFile(const Contact& c, const std::wstring& filename);
private:
    SocketThread* d = nullptr;
};
