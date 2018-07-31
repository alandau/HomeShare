#include <winsock2.h>
#include "SocketThread.h"
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

class MessageWindow : public Window
{
public:
    enum {
        WM_SOCKET = WM_USER,
        WM_RUN_IN_THREAD,
    };
    using NotifyCb = std::function<void(SOCKET, int, int)>;

    MessageWindow(NotifyCb notifyCb)
        : notifyCb_(std::move(notifyCb))
    {}
    LPCTSTR ClassName() override { return TEXT("MessageWindow"); }
    static MessageWindow *Create(NotifyCb notifyCb) {
        MessageWindow* self = new MessageWindow(notifyCb);
        if (self->WinCreateWindow(0,
            TEXT(""), WS_OVERLAPPEDWINDOW,
            CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
            HWND_MESSAGE, NULL)) {
            return self;
        }
        delete self;
        return nullptr;
    }

    void RunInThread(std::function<void(void)> func) {
        std::function<void(void)>* p = new std::function<void(void)>(std::move(func));
        PostMessage(GetHWND(), WM_RUN_IN_THREAD, (WPARAM)p, 0);
    }
protected:
    LRESULT HandleMessage(UINT uMsg, WPARAM wParam, LPARAM lParam) {
        if (uMsg == WM_RUN_IN_THREAD) {
            std::function<void(void)>* func = (std::function<void(void)>*)wParam;
            (*func)();
            delete func;
            return 0;
        }
        if (uMsg == WM_SOCKET) {
            SOCKET sock = (SOCKET)wParam;
            int error = WSAGETSELECTERROR(lParam);
            int event = WSAGETSELECTEVENT(lParam);
            notifyCb_(sock, event, error);
            return 0;
        }
        return Window::HandleMessage(uMsg, wParam, lParam);
    }
private:
    NotifyCb notifyCb_;
};

class SocketThread {
public:
    SocketThread(HWND notifyHwnd);
    void ExitAndWait();
    void RunInThread(std::function<void(void)> func);
    void SendFile(const Contact& c, const std::wstring& filename);

private:
    void Loop();
    void onSocketEvent(SOCKET s, int event, int error);
    void CloseSocket(SOCKET s);
    void OnRead(SOCKET s);
    void OnWrite(SOCKET s);
    void HandleIncomingMessage(SocketData& data, const uint8_t* message, uint32_t len);

    HWND notifyHwnd_;
    std::thread thread_;
    SOCKET serverSocket_;
    MessageWindow* win_;
    std::unordered_map<SOCKET, SocketData> socketData_;
};

void SocketThreadApi::Init(HWND notifyHwnd) {
    d = new SocketThread(notifyHwnd);
}

SocketThreadApi::~SocketThreadApi() {
    delete d;
}

void SocketThreadApi::ExitAndWait() {
    d->ExitAndWait();
}

void SocketThreadApi::SendFile(const Contact& c, const std::wstring& filename) {
    d->RunInThread([this, c, filename] {
        d->SendFile(c, filename);
    });
}

SocketThread::SocketThread(HWND notifyHwnd)
    : notifyHwnd_(notifyHwnd)
    , thread_(std::thread([this] { Loop(); }))
{
}

void SocketThread::RunInThread(std::function<void(void)> func) {
    win_->RunInThread(std::move(func));
}

void SocketThread::ExitAndWait() {
    RunInThread([] {
        PostQuitMessage(0);
    });
    thread_.join();
}

void SocketThread::SendFile(const Contact& c, const std::wstring& filename) {
    HANDLE hFile = CreateFile(filename.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
        NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, NULL);

    if (hFile == INVALID_HANDLE_VALUE) {
        MessageBox(NULL, (L"Can't open file " + filename).c_str(), NULL, MB_OK);
        return;
    }

    SOCKET s = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in addr = { 0 };
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = inet_addr(c.hostname);
    addr.sin_port = htons(c.port);
    WSAAsyncSelect(s, win_->GetHWND(), MessageWindow::WM_SOCKET, FD_CONNECT | FD_READ | FD_WRITE | FD_CLOSE);

    SocketData& data = socketData_[s];
    data.sendFileHandle = hFile;
    data.sendFileOffset = 0;

    int res = connect(s, (sockaddr*)&addr, sizeof(addr));
    if (res < 0 && (res = WSAGetLastError()) != WSAEWOULDBLOCK) {
        CloseHandle(data.sendFileHandle);
        data.sendFileHandle = nullptr;
        CloseSocket(s);
        MessageBox(NULL, (L"Can't connect " + std::to_wstring(res)).c_str(), NULL, MB_OK);
    }
}

void SocketThread::Loop() {
    win_ = MessageWindow::Create([this](SOCKET s, int event, int error) { onSocketEvent(s, event, error); });

    WSADATA wsd;
    WSAStartup(MAKEWORD(2, 2), &wsd);
    serverSocket_ = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in addr = { 0 };
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(8890);
    bind(serverSocket_, (sockaddr*)&addr, sizeof(addr));
    WSAAsyncSelect(serverSocket_, win_->GetHWND(), MessageWindow::WM_SOCKET, FD_ACCEPT | FD_CLOSE);
    listen(serverSocket_, 10);

    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
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
                if (WSAGetLastError() == WSAEWOULDBLOCK) {
                    return;
                }
                CloseSocket(s);
                MessageBox(NULL, L"Error reading from socket", NULL, MB_OK);
                return;
            } else if (count == 0) {
                // EOF
                if (data.messageLenLen != 0) {
                    // Unexpected EOF
                    MessageBox(NULL, L"End of file reading size from socket", NULL, MB_OK);
                }
                CloseSocket(s);
                return;
            }
            data.messageLenLen += count;
        }

        if (data.messageLenLen == 4) {
            // We just finished reading the length
            if (data.messageLen < 4 || data.messageLen >= MAX_MESSAGE_SIZE) {
                CloseSocket(s);
                MessageBox(NULL, L"Too large message", NULL, MB_OK);
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
            if (WSAGetLastError() == WSAEWOULDBLOCK) {
                return;
            }
            CloseSocket(s);
            MessageBox(NULL, L"Error reading from socket", NULL, MB_OK);
        } else if (count == 0) {
            // Unexpected EOF
            CloseSocket(s);
            MessageBox(NULL, L"End of file reading message from socket", NULL, MB_OK);
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

    PostMessage(notifyHwnd_, WM_USER, (WPARAM)wbuf, wsize);
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
                CloseHandle(data.sendFileHandle);
                data.sendFileHandle = nullptr;
                CloseSocket(s);
                MessageBox(NULL, L"Error reading from file", NULL, MB_OK);
                return;
            }
            if (count == 0) {
                // EOF
                CloseHandle(data.sendFileHandle);
                data.sendFileHandle = nullptr;
                CloseSocket(s);
                MessageBox(NULL, L"Done!", NULL, MB_OK);
                return;
            }
            memcpy(data.sendFileBuffer, &count, sizeof(uint32_t));
            data.sendFileBufferSize = count + sizeof(uint32_t);
            data.sendFileBufferOffset = 0;
        }
        int res = send(s, (const char*)data.sendFileBuffer + data.sendFileBufferOffset,
            data.sendFileBufferSize - data.sendFileBufferOffset, 0);
        if (res < 0) {
            if (WSAGetLastError() == WSAEWOULDBLOCK) {
                return;
            }
            CloseHandle(data.sendFileHandle);
            data.sendFileHandle = nullptr;
            CloseSocket(s);
            MessageBox(NULL, L"Error writing to socket", NULL, MB_OK);
            return;
        }
        data.sendFileOffset += res;
        data.sendFileBufferOffset += res;
    }
}

void SocketThread::onSocketEvent(SOCKET sock, int event, int error) {
    if (error) {
        MessageBox(NULL, L"Socket error", NULL, MB_OK);
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
        WSAAsyncSelect(clientSock, win_->GetHWND(), MessageWindow::WM_SOCKET, FD_READ | FD_WRITE | FD_CLOSE);
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