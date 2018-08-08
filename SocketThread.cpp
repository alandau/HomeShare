#include <winsock2.h>
#include "SocketThread.h"
#include "lib/win/MessageThread.h"
#include "lib/win/window.h"
#include "lib/win/raii.h"
#include <string>
#include <vector>
#include <thread>
#include <unordered_map>
#include <deque>

enum { MAX_MESSAGE_SIZE = 100000 };
struct SocketData {
    Contact contact;
    sockaddr addr;

    // Input
    uint32_t messageLen = 0;
    uint32_t messageLenLen = 0;
    Buffer::UniquePtr message;

    // Output
    bool isCorked = false;
    bool isQueueFull = false;
    bool onWriteScheduled = false;
    std::deque<Buffer*> queue;
};

class SocketThread : public MessageThread {
public:
    enum {
        WM_SOCKET = WM_APP,
    };

    enum { LOW_WATERMARK = 10, HIGH_WATERMARK = 100 };
    enum { MAX_BUFFERS_TO_SEND = 10 };

    SocketThread(Logger& logger, HWND notifyHwnd);
    void setQueueEmptyCb(std::function<void(const Contact& c)> queueEmptyCb);
    void setOnMessageCb(std::function<void(const Contact& c, Buffer::UniquePtr message)> onMessageCb);
    bool SendBuffer(const Contact& c, Buffer* buffer);

protected:
    std::optional<LRESULT> HandleMessage(UINT uMsg, WPARAM wParam, LPARAM lParam) override;
    void InitInThread() override;
private:
    void onSocketEvent(SOCKET s, int event, int error);
    void CloseSocket(SOCKET s);
    void OnRead(SOCKET s);
    void OnWrite(SOCKET s);
    void handleIncomingMessage(const Contact& c, Buffer::UniquePtr message);

    Logger& log;
    HWND notifyHwnd_;
    SOCKET serverSocket_;
    std::function<void(const Contact& c)> queueEmptyCb_;
    std::function<void(const Contact& c, Buffer::UniquePtr message)> onMessageCb_;
    std::unordered_map<SOCKET, SocketData> socketData_;
    std::unordered_map<Contact, SOCKET> contactData_;
};

void SocketThreadApi::Init(Logger* logger, HWND notifyHwnd) {
    d = new SocketThread(*logger, notifyHwnd);
}

SocketThreadApi::~SocketThreadApi() {
    delete d;
}

void SocketThreadApi::setQueueEmptyCb(std::function<void(const Contact& c)> queueEmptyCb) {
    d->setQueueEmptyCb(std::move(queueEmptyCb));
}

void SocketThreadApi::setOnMessageCb(std::function<void(const Contact& c, Buffer::UniquePtr message)> onMessageCb) {
    d->setOnMessageCb(std::move(onMessageCb));
}

bool SocketThreadApi::SendBuffer(const Contact& c, Buffer* buffer) {
    return d->RunInThreadWithResult([this, c, buffer] {
        return d->SendBuffer(c, buffer);
    });
}

SocketThread::SocketThread(Logger& logger, HWND notifyHwnd)
    : log(logger)
    , notifyHwnd_(notifyHwnd)
{
}

void SocketThread::setQueueEmptyCb(std::function<void(const Contact& c)> queueEmptyCb) {
    queueEmptyCb_ = std::move(queueEmptyCb);
}

void SocketThread::setOnMessageCb(std::function<void(const Contact& c, Buffer::UniquePtr message)> onMessageCb) {
    onMessageCb_ = std::move(onMessageCb);
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

bool SocketThread::SendBuffer(const Contact& c, Buffer* buffer) {
    uint32_t size = buffer->readSize();
    uint8_t* buf = buffer->prependHeader(sizeof(size));
    memcpy(buf, &size, sizeof(size));

    if (contactData_.find(c) != contactData_.end()) {
        SOCKET s = contactData_[c];
        SocketData& data = socketData_[s];
        data.queue.push_back(buffer);
        if (!data.isCorked && !data.onWriteScheduled) {
            data.onWriteScheduled = true;
            RunInThread([this, s] {
                OnWrite(s);
            });
        }
        if (data.queue.size() > HIGH_WATERMARK) {
            data.isQueueFull = true;
        }
        return data.isQueueFull;
    }

    SOCKET s = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in addr = { 0 };
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = inet_addr(c.hostname);
    addr.sin_port = htons(c.port);
    WSAAsyncSelect(s, GetHWND(), WM_SOCKET, FD_CONNECT | FD_READ | FD_WRITE | FD_CLOSE);

    contactData_[c] = s;
    SocketData& data = socketData_[s];
    data.contact = c;
    data.queue.push_back(buffer);
    data.isCorked = true;

    int res = connect(s, (sockaddr*)&addr, sizeof(addr));
    if (res < 0 && (res = WSAGetLastError()) != WSAEWOULDBLOCK) {
        CloseSocket(s);
        log.e(L"Can't connect, WSAGetLastError={}", res);
    }

    return false;
}

void SocketThread::CloseSocket(SOCKET s) {
    closesocket(s);
    contactData_.erase(socketData_[s].contact);
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
            data.message.reset(Buffer::create(data.messageLen));
        }
    }

    // Now read the message itself
    while (data.message->writeSize() != 0) {
        int count = recv(s, (char*)data.message->writeData(), data.message->writeSize(), 0);
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
        data.message->adjustWritePos(count);
    }

    // Prepare for next message
    data.messageLenLen = 0;
    handleIncomingMessage(Contact{ "host", 1234 }, std::move(data.message));
}

void SocketThread::OnWrite(SOCKET s) {
    if (socketData_.find(s) == socketData_.end()) {
        // Socket has been closed, but getting notification for previously received data, ignore
        return;
    }
    SocketData& data = socketData_[s];
    data.isCorked = false;

    SCOPE_EXIT {
        data.onWriteScheduled = !data.isCorked && !data.queue.empty();
        if (data.onWriteScheduled) {
            RunInThread([this, s] {
                OnWrite(s);
            });
        }
        if (data.isQueueFull && data.queue.size() < LOW_WATERMARK) {
            data.isQueueFull = false;
            if (queueEmptyCb_) {
                queueEmptyCb_(data.contact);
            }
        }
    };

    int count = 0;
    while (!data.queue.empty() && count < MAX_BUFFERS_TO_SEND) {
        Buffer* buffer = data.queue.front();
        if (buffer->readSize() == 0) {
            buffer->destroy();
            data.queue.pop_front();
            continue;
        }
        int res = send(s, (const char*)buffer->readData(), buffer->readSize(), 0);
        if (res < 0) {
            if ((res = WSAGetLastError()) == WSAEWOULDBLOCK) {
                data.isCorked = true;
                return;
            }
            log.e(L"Error writing to socket, WSAGetLastError={}", res);
            CloseSocket(s);
            return;
        }
        buffer->adjustReadPos(res);
        if (buffer->readSize() == 0) {
            buffer->destroy();
            data.queue.pop_front();
            count++;
        }
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

void SocketThread::handleIncomingMessage(const Contact& c, Buffer::UniquePtr message) {
    onMessageCb_(c, std::move(message));
}
