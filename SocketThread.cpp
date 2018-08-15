#include <winsock2.h>
#include "SocketThread.h"
#include "lib/win/MessageThread.h"
#include "lib/win/window.h"
#include "lib/win/raii.h"
#include "lib/win/encoding.h"
#include "proto/auth.h"
#include "lib/crypto.h"
#include <string>
#include <vector>
#include <thread>
#include <unordered_map>
#include <deque>

enum { MAX_MESSAGE_SIZE = 100000 };

struct AuthData {
    enum class Mode { Client, Server };
    enum class ClientState {
        Uninitialized,
        ExpectingServerHelloFinished,
        Complete,
        Error,
    };
    enum class ServerState {
        Uninitialized,
        ExpectingClientHello,
        ExpectingClientFinished,
        Complete,
        Error,
    };
    Mode mode;
    ClientState clientState = ClientState::Uninitialized;
    ServerState serverState = ServerState::Uninitialized;
    std::string myKeyShare, myKeySharePriv;
    std::string peerKeyShare;
    std::string myRandom, peerRandom;
    std::string rxkey, txkey;
    std::string rxnonce, txnonce;
    std::string peerPubkey;
    GenericHash transcriptHash;

    Buffer::UniquePtr encryptTx(Buffer::UniquePtr buffer);
    Buffer::UniquePtr decryptRx(Buffer::UniquePtr buffer);
};

struct SocketData {
    SOCKET sock;
    Contact contact;
    AuthData auth;

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

    SocketThread(Logger& logger, const std::string& myPubkey, const std::string& myPrivKey);
    void setQueueEmptyCb(std::function<void(const Contact& c)> queueEmptyCb);
    void setOnMessageCb(std::function<void(const Contact& c, Buffer::UniquePtr message)> onMessageCb);
    void setOnConnectCb(std::function<void(const Contact& c, bool connected)> cb);
    bool SendBuffer(const Contact& c, Buffer::UniquePtr buffer);
    bool SendBuffer(SocketData& data, Buffer::UniquePtr buffer);
    void Connect(const Contact& c, const std::string& hostname, uint16_t port);

protected:
    std::optional<LRESULT> HandleMessage(UINT uMsg, WPARAM wParam, LPARAM lParam) override;
    void InitInThread() override;
private:
    void onSocketEvent(SOCKET s, int event, int error);
    void CloseSocket(SOCKET s);
    void OnConnect(SOCKET s);
    void OnRead(SOCKET s);
    void OnWrite(SOCKET s);
    void handleIncomingMessage(SocketData& data, Buffer::UniquePtr message);

    void SendClientHelloMessage(SocketData& data);
    void ServerRecvClientHelloMessage(SocketData& data, Buffer::UniquePtr message);
    void ClientRecvServerHelloFinishedMessage(SocketData& data, Buffer::UniquePtr message);
    void ServerRecvClientFinishedMessage(SocketData& data, Buffer::UniquePtr message);

    Logger& log;
    SOCKET serverSocket_;
    std::string myPubkey_, myPrivkey_;
    std::function<void(const Contact& c)> queueEmptyCb_;
    std::function<void(const Contact& c, Buffer::UniquePtr message)> onMessageCb_;
    std::function<void(const Contact& c, bool connected)> onConnectCb_;
    std::unordered_map<SOCKET, SocketData> socketData_;
    std::unordered_map<Contact, SOCKET> contactData_;
};

void SocketThreadApi::Init(Logger* logger, const std::string& myPubkey, const std::string& myPrivkey) {
    d = new SocketThread(*logger, myPubkey, myPrivkey);
    d->Start();
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
        return d->SendBuffer(c, Buffer::UniquePtr(buffer));
    });
}
void SocketThreadApi::setOnConnectCb(std::function<void(const Contact& c, bool connected)> cb) {
    d->setOnConnectCb(std::move(cb));
}

void SocketThreadApi::Connect(const Contact& c, const std::string& hostname, uint16_t port) {
    d->RunInThread([this, c, hostname, port] {
        d->Connect(c, hostname, port);
    });
}

SocketThread::SocketThread(Logger& logger, const std::string& myPubkey, const std::string& myPrivkey)
    : log(logger)
    , myPubkey_(myPubkey)
    , myPrivkey_(myPrivkey)
{
}

void SocketThread::setQueueEmptyCb(std::function<void(const Contact& c)> queueEmptyCb) {
    queueEmptyCb_ = std::move(queueEmptyCb);
}

void SocketThread::setOnMessageCb(std::function<void(const Contact& c, Buffer::UniquePtr message)> onMessageCb) {
    onMessageCb_ = std::move(onMessageCb);
}

void SocketThread::setOnConnectCb(std::function<void(const Contact& c, bool connected)> cb) {
    onConnectCb_ = std::move(cb);
}

void SocketThread::InitInThread() {
    WSADATA wsd;
    if (WSAStartup(MAKEWORD(2, 2), &wsd) != 0) {
        log.e(L"WSAStartup error {}", errstr(WSAGetLastError()));
        return;
    }
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

void SocketThread::Connect(const Contact& c, const std::string& hostname, uint16_t port) {
    SOCKET s = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in addr = { 0 };
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = inet_addr(hostname.c_str());
    addr.sin_port = htons(port);
    WSAAsyncSelect(s, GetHWND(), WM_SOCKET, FD_CONNECT | FD_READ | FD_WRITE | FD_CLOSE);

    auto it = contactData_.find(c);
    if (it != contactData_.end()) {
        CloseSocket(it->second);
    }
    contactData_[c] = s;
    SocketData& data = socketData_[s];
    data.sock = s;
    data.contact = c;
    data.auth.mode = AuthData::Mode::Client;
    data.auth.clientState = AuthData::ClientState::Uninitialized;
    data.isCorked = true;

    int res = connect(s, (sockaddr*)&addr, sizeof(addr));
    if (res < 0 && (res = WSAGetLastError()) != WSAEWOULDBLOCK) {
        CloseSocket(s);
        log.e(L"Can't connect, WSAGetLastError={}", errstr(res));
    }
}

bool SocketThread::SendBuffer(SocketData& data, Buffer::UniquePtr buffer) {
    SOCKET s = data.sock;

    if ((data.auth.mode == AuthData::Mode::Client && data.auth.clientState == AuthData::ClientState::Complete) ||
        (data.auth.mode == AuthData::Mode::Server && data.auth.serverState == AuthData::ServerState::Complete)) {
        buffer = data.auth.encryptTx(std::move(buffer));
    }
    uint32_t size = buffer->readSize();
    uint8_t* buf = buffer->prependHeader(sizeof(size));
    memcpy(buf, &size, sizeof(size));

    data.queue.push_back(buffer.release());
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

bool SocketThread::SendBuffer(const Contact& c, Buffer::UniquePtr buffer) {
    assert(contactData_.find(c) != contactData_.end());

    SOCKET s = contactData_[c];
    SocketData& data = socketData_[s];
    return SendBuffer(data, std::move(buffer));
}

void SocketThread::CloseSocket(SOCKET s) {
    if (onConnectCb_) {
        onConnectCb_(socketData_[s].contact, false);
    }
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
                log.e(L"Error reading from socket, WSAGetLastError={}", errstr(err));
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
            log.e(L"Error reading from socket, WSAGetLastError={}", errstr(err));
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
    handleIncomingMessage(data, std::move(data.message));
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
            log.e(L"Error writing to socket, WSAGetLastError={}", errstr(res));
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
        log.e(L"Socket error {}", errstr(error));
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
        data.auth.mode = AuthData::Mode::Server;
        data.auth.serverState = AuthData::ServerState::ExpectingClientHello;
        WSAAsyncSelect(clientSock, GetHWND(), WM_SOCKET, FD_READ | FD_WRITE | FD_CLOSE);
        break;
    }
    case FD_CONNECT:
        OnConnect(sock);
        break;
    case FD_READ:
        OnRead(sock);
        break;
    case FD_WRITE:
        OnWrite(sock);
        break;
    case FD_CLOSE:
        OnRead(sock);
        // Remote side closed, we do nothing, since after reading EOF, we'll close the socket
        break;
    }
}

void SocketThread::OnConnect(SOCKET s) {
    if (socketData_.find(s) == socketData_.end()) {
        return;
    }
    SocketData& data = socketData_[s];
    data.isCorked = false;

    assert(data.auth.mode == AuthData::Mode::Client);
    assert(data.auth.clientState == AuthData::ClientState::Uninitialized);
    SendClientHelloMessage(data);
}

void SocketThread::handleIncomingMessage(SocketData& data, Buffer::UniquePtr message) {
    if ((data.auth.mode == AuthData::Mode::Client && data.auth.clientState == AuthData::ClientState::Complete) ||
        (data.auth.mode == AuthData::Mode::Server && data.auth.serverState == AuthData::ServerState::Complete)) {
        message = data.auth.decryptRx(std::move(message));
        if (!message) {
            log.e(L"Can't decrypt message");
            CloseSocket(data.sock);
            return;
        }
        onMessageCb_(Contact{ "blah" }, std::move(message));
        return;
    }

    if (data.auth.mode == AuthData::Mode::Client) {
        if (data.auth.clientState == AuthData::ClientState::ExpectingServerHelloFinished) {
            ClientRecvServerHelloFinishedMessage(data, std::move(message));
        } else if (data.auth.clientState == AuthData::ClientState::Error) {
            log.e(L"Client: Received message in error state");
            CloseSocket(data.sock);
        } else if (data.auth.clientState == AuthData::ClientState::Uninitialized) {
            log.e(L"Client: Received message in uninitialized state");
            CloseSocket(data.sock);
        } else {
            log.e(L"Client: Bad state");
            CloseSocket(data.sock);
        }
    } else {
        if (data.auth.serverState == AuthData::ServerState::ExpectingClientHello) {
            ServerRecvClientHelloMessage(data, std::move(message));
        } else if (data.auth.serverState == AuthData::ServerState::ExpectingClientFinished) {
            ServerRecvClientFinishedMessage(data, std::move(message));
        } else if (data.auth.serverState == AuthData::ServerState::Error) {
            log.e(L"Server: Received message in error state");
            CloseSocket(data.sock);
        } else if (data.auth.serverState == AuthData::ServerState::Uninitialized) {
            log.e(L"Server: Received message in uninitialized state");
            CloseSocket(data.sock);
        } else {
            log.e(L"Server: Bad state");
            CloseSocket(data.sock);
        }
    }
}

void SocketThread::SendClientHelloMessage(SocketData& data) {
    AuthData& auth = data.auth;

    auth.transcriptHash.update(std::string(32, ' '));

    auth.myRandom = getRandom(32);
    auth.txnonce = getRandom(crypto_aead_chacha20poly1305_ietf_NPUBBYTES);
    auth.myKeyShare.resize(crypto_kx_PUBLICKEYBYTES);
    auth.myKeySharePriv.resize(crypto_kx_SECRETKEYBYTES);
    crypto_kx_keypair((unsigned char*)auth.myKeyShare.data(), (unsigned char*)auth.myKeySharePriv.data());

    auth.clientState = AuthData::ClientState::ExpectingServerHelloFinished;

    ClientHelloMessage msg;
    msg.random = auth.myRandom;
    msg.kexKeyShare = auth.myKeyShare;
    msg.nonce = auth.txnonce;
    Buffer::UniquePtr buf = Serializer().serialize(msg);
    SendBuffer(data, std::move(buf));

    auth.transcriptHash.update(msg.random);
    auth.transcriptHash.update(msg.kexKeyShare);
    auth.transcriptHash.update(msg.nonce);
}

void SocketThread::ServerRecvClientHelloMessage(SocketData& data, Buffer::UniquePtr message) {
    AuthData& auth = data.auth;
    {
        ClientHelloMessage msg = Serializer().deserialize<ClientHelloMessage>(message.get());

        if (msg.random.size() != 32 ||
            msg.kexKeyShare.size() != crypto_kx_PUBLICKEYBYTES ||
            msg.nonce.size() != crypto_aead_chacha20poly1305_ietf_NPUBBYTES) {
            CloseSocket(data.sock);
            return;
        }
        auth.peerRandom = msg.random;
        auth.peerKeyShare = msg.kexKeyShare;
        auth.rxnonce = msg.nonce;
    }

    auth.transcriptHash.update(std::string(32, ' '));
    auth.transcriptHash.update(auth.peerRandom);
    auth.transcriptHash.update(auth.peerKeyShare);
    auth.transcriptHash.update(auth.rxnonce);

    auth.myRandom = getRandom(32);
    auth.myKeyShare.resize(crypto_kx_PUBLICKEYBYTES);
    auth.myKeySharePriv.resize(crypto_kx_SECRETKEYBYTES);
    crypto_kx_keypair((unsigned char*)auth.myKeyShare.data(), (unsigned char*)auth.myKeySharePriv.data());

    auth.rxkey.resize(crypto_kx_SESSIONKEYBYTES);
    auth.txkey.resize(crypto_kx_SESSIONKEYBYTES);

    if (crypto_kx_server_session_keys((unsigned char*)auth.rxkey.data(), (unsigned char*)auth.txkey.data(),
        (unsigned char*)auth.myKeyShare.data(), (unsigned char*)auth.myKeySharePriv.data(),
        (unsigned char*)auth.peerKeyShare.data()) != 0) {
        // Suspicious client key
        CloseSocket(data.sock);
        return;
    }

    auth.serverState = AuthData::ServerState::ExpectingClientFinished;

    std::string txnonce = getRandom(crypto_aead_chacha20poly1305_ietf_NPUBBYTES);
    auth.txnonce = txnonce;

    auth.transcriptHash.update(auth.myRandom);
    auth.transcriptHash.update(auth.myKeyShare);
    auth.transcriptHash.update(auth.txnonce);
    auth.transcriptHash.update(myPubkey_);

    SignatureMessage sigmsg;
    sigmsg.pubkey = myPubkey_;
    std::string hash = auth.transcriptHash.resultAndContinue();
    sigmsg.signature.resize(crypto_sign_BYTES, '\0');
    crypto_sign_detached((unsigned char*)sigmsg.signature.data(), NULL,
        (const unsigned char*)hash.data(), hash.size(),
        (const unsigned char*)myPrivkey_.data());
    Buffer::UniquePtr serializedSigMsg = Serializer().serialize(sigmsg);
    Buffer::UniquePtr encrypted = auth.encryptTx(std::move(serializedSigMsg));

    ServerHelloFinishedMessage reply;
    reply.random = auth.myRandom;
    reply.kexKeyShare = auth.myKeyShare;
    reply.nonce = txnonce;  // auth.txnonce has been incremented by encryptTx so don't use it here
    reply.encryptedSignatureMessage = std::string((const char*)encrypted->readData(), encrypted->readSize());
    encrypted.reset();

    Buffer::UniquePtr buf = Serializer().serialize(reply);
    SendBuffer(data, std::move(buf));
}

void SocketThread::ClientRecvServerHelloFinishedMessage(SocketData& data, Buffer::UniquePtr message) {
    AuthData& auth = data.auth;
    ServerHelloFinishedMessage msg = Serializer().deserialize<ServerHelloFinishedMessage>(message.get());

    if (msg.random.size() != 32 ||
        msg.kexKeyShare.size() != crypto_kx_PUBLICKEYBYTES ||
        msg.nonce.size() != crypto_aead_chacha20poly1305_ietf_NPUBBYTES ||
        msg.encryptedSignatureMessage.size() > 2000) {
        CloseSocket(data.sock);
        log.d(L"Client: Bad ServerHelloFinished");
        return;
    }
    auth.peerRandom = msg.random;
    auth.peerKeyShare = msg.kexKeyShare;
    auth.rxnonce = msg.nonce;

    auth.rxkey.resize(crypto_kx_SESSIONKEYBYTES);
    auth.txkey.resize(crypto_kx_SESSIONKEYBYTES);

    if (crypto_kx_client_session_keys((unsigned char*)auth.rxkey.data(), (unsigned char*)auth.txkey.data(),
        (unsigned char*)auth.myKeyShare.data(), (unsigned char*)auth.myKeySharePriv.data(),
        (unsigned char*)auth.peerKeyShare.data()) != 0) {
        // Suspicious server key
        CloseSocket(data.sock);
        return;
    }

    auth.transcriptHash.update(auth.peerRandom);
    auth.transcriptHash.update(auth.peerKeyShare);
    auth.transcriptHash.update(auth.rxnonce);

    {
        Buffer::UniquePtr encrypted(Buffer::create(msg.encryptedSignatureMessage.size()));
        memcpy(encrypted->writeData(), msg.encryptedSignatureMessage.data(), msg.encryptedSignatureMessage.size());
        encrypted->adjustWritePos(msg.encryptedSignatureMessage.size());

        Buffer::UniquePtr decrypted = auth.decryptRx(std::move(encrypted));
        if (!decrypted) {
            log.d(L"Client: Error decrypting ServerHelloFinished");
            CloseSocket(data.sock);
            return;
        }

        SignatureMessage serversigmsg = Serializer().deserialize<SignatureMessage>(decrypted.get());
        if (serversigmsg.pubkey.size() != crypto_sign_PUBLICKEYBYTES || serversigmsg.signature.size() != crypto_sign_BYTES) {
            log.d(L"Client: Wrong server pubkey or signature size");
            CloseSocket(data.sock);
            return;
        }
        auth.peerPubkey = serversigmsg.pubkey;
        auth.transcriptHash.update(auth.peerPubkey);
        std::string hash = auth.transcriptHash.resultAndContinue();
        if (crypto_sign_verify_detached((const unsigned char*)serversigmsg.signature.data(),
            (const unsigned char*)hash.data(), hash.size(),
            (const unsigned char*)auth.peerPubkey.data()) != 0) {
            log.e(L"Couldn't authenticate server");
            CloseSocket(data.sock);
            return;
        }
        log.d(L"Client: Authenticated server {}", keyToDisplayStr(auth.peerPubkey));
    }

    auth.transcriptHash.update(myPubkey_);

    {
        std::string hash = auth.transcriptHash.result();
        SignatureMessage clientsigmsg;
        clientsigmsg.pubkey = myPubkey_;
        clientsigmsg.signature.resize(crypto_sign_BYTES, '\0');
        crypto_sign_detached((unsigned char*)clientsigmsg.signature.data(), NULL,
            (const unsigned char*)hash.data(), hash.size(),
            (const unsigned char*)myPrivkey_.data());
        Buffer::UniquePtr serializedSigMsg = Serializer().serialize(clientsigmsg);
        Buffer::UniquePtr encrypted = auth.encryptTx(std::move(serializedSigMsg));

        ClientFinishedMessage reply;
        reply.encryptedSignatureMessage = std::string((const char*)encrypted->readData(), encrypted->readSize());
        encrypted.reset();

        Buffer::UniquePtr buf = Serializer().serialize(reply);
        SendBuffer(data, std::move(buf));
    }

    auth.clientState = AuthData::ClientState::Complete;

    if (onConnectCb_) {
        onConnectCb_(data.contact, true);
    }

    if (queueEmptyCb_) {
        queueEmptyCb_(data.contact);
    }
}

void SocketThread::ServerRecvClientFinishedMessage(SocketData& data, Buffer::UniquePtr message) {
    AuthData& auth = data.auth;
    ClientFinishedMessage msg = Serializer().deserialize<ClientFinishedMessage>(message.get());

    if (msg.encryptedSignatureMessage.size() > 2000) {
        CloseSocket(data.sock);
        log.d(L"Server:Bad ClientFinished");
        return;
    }

    Buffer::UniquePtr encrypted(Buffer::create(msg.encryptedSignatureMessage.size()));
    memcpy(encrypted->writeData(), msg.encryptedSignatureMessage.data(), msg.encryptedSignatureMessage.size());
    encrypted->adjustWritePos(msg.encryptedSignatureMessage.size());

    Buffer::UniquePtr decrypted = auth.decryptRx(std::move(encrypted));
    if (!decrypted) {
        log.d(L"Server: Error decrypting ClientFinished");
        CloseSocket(data.sock);
        return;
    }

    SignatureMessage sigmsg = Serializer().deserialize<SignatureMessage>(decrypted.get());
    if (sigmsg.pubkey.size() != crypto_sign_PUBLICKEYBYTES || sigmsg.signature.size() != crypto_sign_BYTES) {
        log.d(L"Server: Wrong client pubkey or signature size");
        CloseSocket(data.sock);
        return;
    }
    auth.peerPubkey = sigmsg.pubkey;
    auth.transcriptHash.update(auth.peerPubkey);
    std::string hash = auth.transcriptHash.result();
    if (crypto_sign_verify_detached((const unsigned char*)sigmsg.signature.data(),
        (const unsigned char*)hash.data(), hash.size(),
        (const unsigned char*)auth.peerPubkey.data()) != 0) {
        log.e(L"Couldn't authenticate client");
        CloseSocket(data.sock);
        return;
    }
    log.d(L"Server: Authenticated client {}", keyToDisplayStr(auth.peerPubkey));

    auth.serverState = AuthData::ServerState::Complete;
}

Buffer::UniquePtr AuthData::encryptTx(Buffer::UniquePtr buffer) {
    Buffer::UniquePtr encrypted(Buffer::create(buffer->readSize() + crypto_aead_chacha20poly1305_IETF_ABYTES));
    unsigned long long clen;
    crypto_aead_chacha20poly1305_ietf_encrypt(encrypted->writeData(), &clen, buffer->readData(), buffer->readSize(),
        NULL, 0, NULL,
        (const unsigned char*)txnonce.data(), (const unsigned char*)txkey.data());
    encrypted->adjustWritePos((intptr_t)clen);
    sodium_increment((unsigned char*)txnonce.data(), txnonce.size());
    return encrypted;
}

Buffer::UniquePtr AuthData::decryptRx(Buffer::UniquePtr buffer) {
    if (buffer->readSize() < crypto_aead_chacha20poly1305_IETF_ABYTES) {
        return Buffer::UniquePtr();
    }
    Buffer::UniquePtr decrypted(Buffer::create(buffer->readSize() - crypto_aead_chacha20poly1305_IETF_ABYTES));
    unsigned long long mlen;
    bool forged = crypto_aead_chacha20poly1305_ietf_decrypt(decrypted->writeData(), &mlen, NULL,
        buffer->readData(), buffer->readSize(), NULL, 0,
        (const unsigned char*)rxnonce.data(), (const unsigned char*)rxkey.data()) != 0;
    decrypted->adjustWritePos((intptr_t)mlen);
    sodium_increment((unsigned char*)rxnonce.data(), rxnonce.size());
    if (forged) {
        decrypted.reset();
    }
    return decrypted;
}
