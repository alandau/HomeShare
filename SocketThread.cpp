#include <winsock2.h>
#include "SocketThread.h"
#include "lib/win/MessageThread.h"
#include "lib/win/window.h"
#include <string>
#include <vector>
#include <thread>
#include <unordered_map>
#include <assert.h>

enum { MAX_MESSAGE_SIZE = 100000 };
struct SocketData {
    SOCKET sock;
    sockaddr addr;

    // Fields for reading a message
    uint32_t messageLen = 0;
    uint32_t messageLenLen = 0;
    uint8_t* message = nullptr;
    uint32_t countSoFar = 0;

    // Fields for writing a file
    HANDLE sendFileHandle = nullptr;
    uint64_t sendFileOffset = 0;
    uint8_t sendFileBuffer[65536];
    uint32_t sendFileBufferSize = 0;
    uint32_t sendFileBufferOffset = 0;
};

class SocketThread : public MessageThread {
public:
    enum {
        WM_SOCKET = WM_APP,
    };

    SocketThread(Logger& logger, HWND notifyHwnd);
    void SendFile(const Contact& c, const std::wstring& filename);

protected:
    std::optional<LRESULT> HandleMessage(UINT uMsg, WPARAM wParam, LPARAM lParam) override;
    void InitInThread() override;
private:
    void onSocketEvent(SOCKET s, int event, int error);
    void CloseSocket(SOCKET s);
    void OnRead(SOCKET s);
    void OnWrite(SOCKET s);
    void HandleIncomingMessage(SocketData& data, const uint8_t* message, uint32_t len);

    Logger& log;
    HWND notifyHwnd_;
    SOCKET serverSocket_;
    std::unordered_map<SOCKET, SocketData> socketData_;
};

void SocketThreadApi::Init(Logger* logger, HWND notifyHwnd) {
    d = new SocketThread(*logger, notifyHwnd);
}

SocketThreadApi::~SocketThreadApi() {
    delete d;
}

void SocketThreadApi::SendFile(const Contact& c, const std::wstring& filename) {
    d->RunInThread([this, c, filename] {
        d->SendFile(c, filename);
    });
}

SocketThread::SocketThread(Logger& logger, HWND notifyHwnd)
    : log(logger)
    , notifyHwnd_(notifyHwnd)
{
}

void SocketThread::InitInThread() {
    WSADATA wsd;
    WSAStartup(MAKEWORD(2, 2), &wsd);
    serverSocket_ = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in addr = { 0 };
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(8890);
    bind(serverSocket_, (sockaddr*)&addr, sizeof(addr));
    WSAAsyncSelect(serverSocket_, GetHWND(), WM_SOCKET, FD_ACCEPT | FD_CLOSE);
    listen(serverSocket_, 10);
}

std::optional<LRESULT> SocketThread::HandleMessage(UINT uMsg, WPARAM wParam, LPARAM lParam) {
    if (uMsg == WM_SOCKET) {
        SOCKET sock = (SOCKET)wParam;
        int error = WSAGETSELECTERROR(lParam);
        int event = WSAGETSELECTEVENT(lParam);
        onSocketEvent(sock, event, error);
        return (LRESULT)0;
    }
    return std::nullopt;
}

void SocketThread::SendFile(const Contact& c, const std::wstring& filename) {
    HANDLE hFile = CreateFile(filename.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
        NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, NULL);

    if (hFile == INVALID_HANDLE_VALUE) {
        log.e(L"Can't open file {}", filename);
        return;
    }

    SOCKET s = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in addr = { 0 };
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = inet_addr(c.hostname);
    addr.sin_port = htons(c.port);
    WSAAsyncSelect(s, GetHWND(), WM_SOCKET, FD_CONNECT | FD_READ | FD_WRITE | FD_CLOSE);

    SocketData& data = socketData_[s];
    data.sendFileHandle = hFile;
    data.sendFileOffset = 0;

    int res = connect(s, (sockaddr*)&addr, sizeof(addr));
    if (res < 0 && (res = WSAGetLastError()) != WSAEWOULDBLOCK) {
        CloseHandle(data.sendFileHandle);
        data.sendFileHandle = nullptr;
        CloseSocket(s);
        log.e(L"Can't connect, WSAGetLastError={}", res);
    }
}

void SocketThread::CloseSocket(SOCKET s) {
    closesocket(s);
    socketData_.erase(s);
}

void SocketThread::OnRead(SOCKET s) {
    if (socketData_.find(s) == socketData_.end()) {
        // Socket has been closed, but getting notification for previously received data, ignore
        return;
    }
    SocketData& data = socketData_[s];

    if (data.messageLenLen < 4) {
        // First read the message length
        while (data.messageLenLen < 4) {
            int count = recv(s, (char*)&data.messageLen, 4 - data.messageLenLen, 0);
            if (count < 0) {
                int err;
                if ((err = WSAGetLastError()) == WSAEWOULDBLOCK) {
                    return;
                }
                log.e(L"Error reading from socket, WSAGetLastError={}", err);
                CloseSocket(s);
                return;
            } else if (count == 0) {
                // EOF
                if (data.messageLenLen != 0) {
                    // Unexpected EOF
                    log.e(L"End of file reading size from socket");
                }
                CloseSocket(s);
                return;
            }
            data.messageLenLen += count;
        }

        if (data.messageLenLen == 4) {
            // We just finished reading the length
            if (data.messageLen < 4 || data.messageLen >= MAX_MESSAGE_SIZE) {
                log.e(L"Too large message received (dec={0}, hex={0:08x})", data.messageLen);
                CloseSocket(s);
                return;
            }
            data.message = new uint8_t[data.messageLen];
            data.countSoFar = 0;
        }
    }

    // Now read the message itself
    while (data.countSoFar < data.messageLen) {
        int count = recv(s, (char*)data.message, data.messageLen - data.countSoFar, 0);
        if (count < 0) {
            int err;
            if ((err = WSAGetLastError()) == WSAEWOULDBLOCK) {
                return;
            }
            log.e(L"Error reading from socket, WSAGetLastError={}", err);
            CloseSocket(s);
        } else if (count == 0) {
            // Unexpected EOF
            log.e(L"End of file reading message from socket");
            CloseSocket(s);
        }
        data.countSoFar += count;
    }

    // Prepare for next message
    data.messageLenLen = 0;
    HandleIncomingMessage(data, data.message, data.messageLen);
}

void SocketThread::HandleIncomingMessage(SocketData& data, const uint8_t* message, uint32_t len) {
    wchar_t* wbuf = new wchar_t[len];
    int wsize = MultiByteToWideChar(CP_UTF8, 0, (const char*)message, len, wbuf, len);

    PostMessage(notifyHwnd_, WM_USER+1, (WPARAM)wbuf, wsize);
}

void SocketThread::OnWrite(SOCKET s) {
    if (socketData_.find(s) == socketData_.end()) {
        // Socket has been closed, but getting notification for previously received data, ignore
        return;
    }
    SocketData& data = socketData_[s];
    if (data.sendFileHandle == nullptr) {
        return;
    }

    while (1) {
        if (data.sendFileBufferOffset == data.sendFileBufferSize) {
            DWORD count;
            bool success = ReadFile(data.sendFileHandle, data.sendFileBuffer + sizeof(uint32_t),
                sizeof(data.sendFileBuffer) - sizeof(uint32_t), &count, NULL);
            if (!success) {
                log.e(L"Error reading from file");
                CloseHandle(data.sendFileHandle);
                data.sendFileHandle = nullptr;
                CloseSocket(s);
                return;
            }
            if (count == 0) {
                // EOF
                log.i(L"Done!");
                CloseHandle(data.sendFileHandle);
                data.sendFileHandle = nullptr;
                CloseSocket(s);
                return;
            }
            memcpy(data.sendFileBuffer, &count, sizeof(uint32_t));
            data.sendFileBufferSize = count + sizeof(uint32_t);
            data.sendFileBufferOffset = 0;
        }
        int res = send(s, (const char*)data.sendFileBuffer + data.sendFileBufferOffset,
            data.sendFileBufferSize - data.sendFileBufferOffset, 0);
        if (res < 0) {
            if ((res = WSAGetLastError()) == WSAEWOULDBLOCK) {
                return;
            }
            log.e(L"Error writing to socket, WSAGetLastError={}", res);
            CloseHandle(data.sendFileHandle);
            data.sendFileHandle = nullptr;
            CloseSocket(s);
            return;
        }
        data.sendFileOffset += res;
        data.sendFileBufferOffset += res;
    }
}

void SocketThread::onSocketEvent(SOCKET sock, int event, int error) {
    if (error) {
        log.e(L"Socket error {}", error);
        CloseSocket(sock);
        return;
    }
    switch (event) {
    case FD_ACCEPT: {
        sockaddr addr = { 0 };
        int addrlen = sizeof(addr);
        SOCKET clientSock = accept(sock, &addr, &addrlen);
        SocketData& data = socketData_[clientSock];
        data.sock = clientSock;
        data.addr = addr;
        WSAAsyncSelect(clientSock, GetHWND(), WM_SOCKET, FD_READ | FD_WRITE | FD_CLOSE);
        break;
    }
    case FD_READ:
        OnRead(sock);
        break;
    case FD_WRITE:
        OnWrite(sock);
        break;
    case FD_CLOSE:
        // Remote side closed, we do nothing, since after reading EOF, we'll close the socket
        break;
    }
}
