#pragma once

#include "lib/win/MessageThread.h"
#include "lib/Buffer.h"
#include "SocketThread.h"
#include "Logger.h"
#include <deque>
#include <memory>

class DiskThread : public MessageThread {
public:
    DiskThread(Logger* logger, SocketThreadApi* socketThread);
    void Enqueue(const Contact& c, const std::wstring& filename);

private:
    struct QueueItem {
        QueueItem(const Contact& c, const std::wstring& filename)
            : c(c)
            , filename(filename)
        {}
        Contact c;
        std::wstring filename;
        HANDLE hFile = NULL;
        Buffer* buffer;
    };
    struct Queue {
        std::deque<QueueItem> queue_;
    };
    using Map = std::unordered_map<Contact, std::unique_ptr<Queue>>;

    void DoWriteLoop();
    void DoWriteLoopImpl(Map::iterator iter);
    bool SendBufferToContact(const Contact& c, Buffer* buffer);

    Logger& log;
    SocketThreadApi* socketThread_;

    Map corked_;
    Map uncorked_;
    Map paused_;
};
