#include "DiscoveryThread.h"
#include "proto/discovery.h"
#include "lib/win/encoding.h"
#include <iphlpapi.h>

enum { PORT = 8891 };

std::string sockaddr_to_str(const sockaddr& addr) {
    if (addr.sa_family == AF_INET) {
        auto& a = ((sockaddr_in*)&addr)->sin_addr.S_un.S_un_b;
        return fmt::format("{}.{}.{}.{}", a.s_b1, a.s_b2, a.s_b3, a.s_b4);
    }
    return "";
}

void DiscoveryThread::InitInThread() {
    WSADATA wsd;
    WSAStartup(MAKEWORD(2, 2), &wsd);

    CreateSockets();
}

void DiscoveryThread::Send(SOCKET s, QueueItem&& item) {
    sockets_[s].queue.push_back(std::move(item));
    OnWrite(s);
}

void DiscoveryThread::SendAll(const QueueItem& item) {
    for (const auto& sockitem : sockets_) {
        Send(sockitem.first, QueueItem(item));
    }
}


void DiscoveryThread::StartDiscovery() {
    RunInThread([this] {
        discoveryResults_.clear();

        Buffer::UniquePtr buf(Buffer::create(sizeof(uint32_t)));
        uint32_t magic = DISCOVERY_REQ_MAGIC;
        memcpy(buf->writeData(), &magic, sizeof(uint32_t));
        buf->adjustWritePos(sizeof(uint32_t));
        QueueItem item;
        item.buffer = std::move(buf);

        sockaddr_in addr = { 0 };
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = INADDR_BROADCAST;
        addr.sin_port = htons(PORT);

        memcpy(&item.addr, &addr, sizeof(addr));
        item.addrlen = sizeof(addr);
        SendAll(item);

        SetTimer(GetHWND(), 1, 500, NULL);
    });
}

void DiscoveryThread::OnWrite(SOCKET s) {
    SocketData& data = sockets_[s];
    if (data.queue.empty()) {
        return;
    }
    QueueItem& item = data.queue.front();
    int res = sendto(s, (const char *)item.buffer->readData(), item.buffer->readSize(), 0,
        &item.addr, item.addrlen);
    if (res < 0) {
        if ((res = WSAGetLastError()) != WSAEWOULDBLOCK) {
            log.e(L"Can't send discovery packet {}", errstr(res));
            data.queue.pop_front();
            return;
        }
        // Would block, do nothing
        return;
    }
    data.queue.pop_front();
}

void DiscoveryThread::OnRead(SOCKET s) {
    Buffer::UniquePtr data(Buffer::create(1500));
    sockaddr from = { 0 };
    int fromlen = sizeof(from);
    int res = recvfrom(s, (char *)data->writeData(), data->writeSize(), 0, &from, &fromlen);
    if (res < 0) {
        log.e(L"Can't receive discovery packet {}", errstr(WSAGetLastError()));
        return;
    }
    if (res < 4) {
        log.d(L"Bad discovery packet of size {}", res);
        return;
    }
    data->adjustWritePos(res);
    uint32_t magic;
    memcpy(&magic, data->readData(), sizeof(uint32_t));
    data->adjustReadPos(sizeof(uint32_t));

    if (magic == DISCOVERY_REQ_MAGIC) {
        for (const auto& pair : sockets_) {
            if (memcmp(&pair.second.localAddr, &from, sizeof(pair.second.localAddr)) == 0) {
                // Ignore messages from ourselves
                return;
            }
        }
        DiscoveryResp resp;
        resp.ip = sockaddr_to_str(*(sockaddr*)&sockets_[s].localAddr);
        resp.port = 8890;
        resp.pubkey = pubkey_;
        QueueItem item;
        item.buffer = Serializer().serialize(resp);
        uint8_t* magicPos = item.buffer->prependHeader(sizeof(uint32_t));
        magic = DISCOVERY_RESP_MAGIC;
        memcpy(magicPos, &magic, sizeof(uint32_t));
        item.addr = from;
        item.addrlen = fromlen;
        Send(s, std::move(item));
    } else if (magic == DISCOVERY_RESP_MAGIC) {
        DiscoveryResp resp = Serializer().deserialize<DiscoveryResp>(data.get());
        DiscoveryResult res;
        res.pubkey = resp.pubkey;
        res.host = resp.ip;
        res.port = resp.port;
        discoveryResults_.push_back(std::move(res));
    } else {
        log.d(L"Received bad discovery magic 0x{:08x}", magic);
    }
}

void DiscoveryThread::onSocketEvent(SOCKET sock, int event, int error) {
    if (error) {
        log.e(L"Discovery socket error {}", errstr(error));
        return;
    }
    switch (event) {
    case FD_READ:
        OnRead(sock);
        break;
    case FD_WRITE:
        OnWrite(sock);
        break;
    }
}

std::optional<LRESULT> DiscoveryThread::HandleMessage(UINT uMsg, WPARAM wParam, LPARAM lParam) {
    if (uMsg == WM_SOCKET) {
        SOCKET sock = (SOCKET)wParam;
        int error = WSAGETSELECTERROR(lParam);
        int event = WSAGETSELECTEVENT(lParam);
        onSocketEvent(sock, event, error);
        return (LRESULT)0;
    }
    if (uMsg == WM_TIMER) {
        KillTimer(GetHWND(), 1);
        resultCb_(discoveryResults_);
        discoveryResults_.clear();
        return (LRESULT)0;
    }
    return std::nullopt;
}

void DiscoveryThread::CreateSockets() {
    for (auto& socketitem : sockets_) {
        closesocket(socketitem.first);
    }
    sockets_.clear();

    ULONG flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER;
    ULONG size = 10 * 1024;
    std::vector<uint8_t> buf(size);
    ULONG res = GetAdaptersAddresses(AF_INET, flags, nullptr, (IP_ADAPTER_ADDRESSES *)&buf[0], &size);
    if (res == ERROR_BUFFER_OVERFLOW) {
        buf.resize(size);
        res = GetAdaptersAddresses(AF_INET, flags, nullptr, (IP_ADAPTER_ADDRESSES *)&buf[0], &size);
    }
    if (res != ERROR_SUCCESS) {
        log.d(L"Can't get list of adapters: {}", errstr(res));
        return;
    }

    IP_ADAPTER_ADDRESSES *p = (IP_ADAPTER_ADDRESSES *)&buf[0];
    for (; p; p = p->Next) {
        if (p->OperStatus != IfOperStatusUp) {
            continue;
        }
        if (!p->FirstUnicastAddress) {
            continue;
        }
 
        SOCKET s = socket(AF_INET, SOCK_DGRAM, 0);
        SocketData& data = sockets_[s];
        int val = 1;
        setsockopt(s, SOL_SOCKET, SO_BROADCAST, (const char *)&val, sizeof(val));
        memcpy(&data.localAddr, p->FirstUnicastAddress->Address.lpSockaddr, sizeof(data.localAddr));
        data.localAddr.sin_port = htons(PORT);
        if (bind(s, (sockaddr *)&data.localAddr, sizeof(data.localAddr)) < 0) {
            log.e(L"Bind to {}:{} failed, discovery may not work correctly. Error {}",
                Utf8ToUtf16(sockaddr_to_str(*(sockaddr *)&data.localAddr)), PORT, errstr(WSAGetLastError()));
        }
        WSAAsyncSelect(s, GetHWND(), WM_SOCKET, FD_WRITE | FD_READ);
    }
}