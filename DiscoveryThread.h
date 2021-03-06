#pragma once

#include "lib/win/MessageThread.h"
#include "lib/Buffer.h"
#include "Logger.h"
#include <winsock2.h>
#include <deque>

class DiscoveryThread : public MessageThread {
public:
    enum { WM_SOCKET = WM_APP };
    struct DiscoveryResult {
        std::string pubkey;
        std::string host;
        uint16_t port;
        std::wstring ifaceName;
    };

    DiscoveryThread(Logger& logger, const std::string& pubkey)
        : log(logger)
        , pubkey_(pubkey)
    {}
    ~DiscoveryThread() {
        closesocket(notificationSocket_);
        for (const auto& si : sockets_) {
            closesocket(si.first);
        }
    }
    void StartDiscovery();
    void setOnResult(std::function<void(const std::vector<DiscoveryResult>& result)> cb) {
        resultCb_ = std::move(cb);
    }

protected:
    void InitInThread() override;
    std::optional<LRESULT> HandleMessage(UINT uMsg, WPARAM wParam, LPARAM lParam) override;
private:
    struct QueueItem {
        std::shared_ptr<Buffer> buffer;
        sockaddr addr;
        int addrlen;
    };
    struct SocketData {
        sockaddr_in localAddr;
        uint32_t metric;
        std::wstring ifaceName;
        std::deque<QueueItem> queue;
    };

    Logger& log;
    std::string pubkey_;
    std::unordered_map<SOCKET, SocketData> sockets_;
    std::function<void(const std::vector<DiscoveryResult>& result)> resultCb_;
    std::vector<DiscoveryResult> discoveryResults_;
    std::unordered_map<std::string, uint32_t> discoveryAux_;
    SOCKET notificationSocket_;

    void onSocketEvent(SOCKET sock, int event, int error);
    void OnRead(SOCKET s);
    void OnWrite(SOCKET s);
    void CreateSockets();
    void Send(SOCKET s, QueueItem&& item);
    void SendAll(const QueueItem& item);
};
