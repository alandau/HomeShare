#pragma once

#include "lib/win/MessageThread.h"
#include "lib/Buffer.h"
#include "SocketThread.h"
#include "Logger.h"
#include "proto/file.h"
#include <deque>
#include <memory>

class DiskThread : public MessageThread {
public:
    DiskThread(Logger* logger, SocketThreadApi* socketThread, const std::wstring& receivePath);
    void Enqueue(const Contact& c, const std::wstring& filename);

private:
    struct QueueItem {
        enum class State { SEND_HEADER, SEND_DATA, SEND_TRAILER };
        QueueItem(const Contact& c, const std::wstring& filename)
            : c(c)
            , filename(filename)
        {}
        Contact c;
        std::wstring filename;
        State state = State::SEND_HEADER;
        HANDLE hFile = NULL;
    };
    struct SendData {
        std::deque<QueueItem> queue_;
    };
    struct ReceiveData {
        enum class State { RECEIVE_HEADER, RECEIVE_DATA_OR_TRAILER };
        State state = State::RECEIVE_HEADER;
        HANDLE hReceiveFile = NULL;
    };
    using Map = std::unordered_map<Contact, std::unique_ptr<SendData>>;

    void DoWriteLoop();
    void DoWriteLoopImpl(Map::iterator iter);
    bool SendBufferToContact(const Contact& c, MessageType type, Buffer::UniquePtr buffer);
    void OnMessageReceived(const Contact& c, Buffer::UniquePtr message);

    Logger& log;
    SocketThreadApi* socketThread_;
    std::wstring receivePath_;

    Map corked_;
    Map uncorked_;
    Map paused_;

    std::unordered_map<Contact, ReceiveData> receive_;
};
