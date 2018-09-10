#pragma once

#include "lib/win/MessageThread.h"
#include "lib/Buffer.h"
#include "SocketThread.h"
#include "Logger.h"
#include "proto/file.h"
#include "lib/crypto.h"
#include <deque>
#include <memory>

struct ProgressUpdate {
    std::chrono::steady_clock::time_point timestamp;
    uint64_t doneBytes = 0;
    uint64_t totalBytes = 0;
    uint32_t doneFiles = 0;
    uint32_t totalFiles = 0;
};

class DiskThread : public MessageThread {
public:
    DiskThread(Logger* logger, SocketThreadApi* socketThread, const std::wstring& receivePath);
    void Enqueue(const Contact& c, const std::wstring& filename);
    void Enqueue(const Contact& c, const std::wstring& dir, const std::vector<std::wstring>& files);
    void setProgressUpdateCb(std::function<void(const Contact& c, const ProgressUpdate& up)> cb);

private:
    enum { MAX_BUFFERS_TO_SEND = 10 };
    struct QueueItem {
        enum class State { SEND_HEADER, SEND_DATA, SEND_TRAILER, SEND_FILE_LIST_HEADER };
        QueueItem(const Contact& c, const std::wstring& filename, bool dontUpdateSizes = false)
            : c(c)
            , filename(filename)
            , dontUpdateSizes(dontUpdateSizes)
        {}
        QueueItem(const Contact& c, uint32_t count, uint64_t size)
            : c(c)
            , count(count)
            , size(size)
            , state(State::SEND_FILE_LIST_HEADER)
        {}
        Contact c;
        std::wstring filename;
        bool dontUpdateSizes;
        uint32_t count;     // only used by SEND_FILE_LIST_HEADER
        uint64_t size;      // file size for 1 file, total size for SEND_FILE_LIST_HEADER
        State state = State::SEND_HEADER;
        HANDLE hFile = NULL;
        GenericHash hash;
    };
    struct SendData {
        std::deque<QueueItem> queue_;
    };
    struct ReceiveData {
        enum class State { RECEIVE_HEADER, RECEIVE_DATA_OR_TRAILER };
        State state = State::RECEIVE_HEADER;
        HANDLE hReceiveFile = NULL;
        uint64_t receivedCount;
        uint64_t receiveSize;
        GenericHash hash;
        std::wstring receiveFilename;
    };
    using Map = std::unordered_map<Contact, std::unique_ptr<SendData>>;

    void DoWriteLoop();
    void DoWriteLoopImpl(Map::iterator iter);
    bool SendBufferToContact(const Contact& c, MessageType type, Buffer::UniquePtr buffer);
    void OnMessageReceived(const Contact& c, Buffer::UniquePtr message);

    HANDLE GetReceiveFile(const std::wstring& origFilename, std::wstring& filename);

    void MaybeSendProgressUpdate(const Contact& c, bool force = false);

    Logger& log;
    SocketThreadApi* socketThread_;
    std::wstring receivePath_;

    Map corked_;
    Map uncorked_;
    Map paused_;

    std::unordered_map<Contact, ReceiveData> receive_;

    std::unordered_map<Contact, ProgressUpdate> progressMap_;
    std::function<void(const Contact& c, const ProgressUpdate& up)> progressUpdateCb_;
};
