#include "DiskThread.h"

DiskThread::DiskThread(Logger* logger, SocketThreadApi* socketThread, const std::wstring& receivePath)
    : log(*logger)
    , socketThread_(socketThread)
    , receivePath_(receivePath)
{
    socketThread_->setQueueEmptyCb([this](const Contact& c) {
        RunInThread([this, c] {
            auto iter = corked_.find(c);
            if (iter == corked_.end()) {
                return;
            }
            uncorked_[c] = std::move(iter->second);
            corked_.erase(iter);
            DoWriteLoop();
        });
    });

    socketThread_->setOnMessageCb([this](const Contact& c, Buffer::UniquePtr message) {
        Buffer* p = message.release();
        RunInThread([this, c, p]() {
            OnMessageReceived(c, Buffer::UniquePtr(p));
        });
    });
}

void DiskThread::Enqueue(const Contact& c, const std::wstring& filename) {
    RunInThread([this, c, filename] {
        log.i(L"Enqueued file '{}'", filename);
        if (paused_.find(c) != paused_.end()) {
            paused_[c]->queue_.emplace_back(c, filename);
        } else if (corked_.find(c) != corked_.end()) {
            corked_[c]->queue_.emplace_back(c, filename);
        } else {
            if (uncorked_.find(c) == uncorked_.end()) {
                uncorked_[c] = std::make_unique<SendData>();
            }
            uncorked_[c]->queue_.emplace_back(c, filename);
            DoWriteLoop();
        }
    });
}

void DiskThread::DoWriteLoop() {
    while (!uncorked_.empty()) {
        DoWriteLoopImpl(uncorked_.begin());
    }
}

void DiskThread::DoWriteLoopImpl(Map::iterator iter) {
    enum { MAX_CHUNK = 65536 };
    std::deque<QueueItem>& queue = iter->second->queue_;
    if (queue.empty()) {
        uncorked_.erase(iter);
        return;
    }
    const Contact& c = iter->first;
    QueueItem& item = queue.front();
    
    if (item.hFile == NULL) {
        HANDLE hFile = CreateFile(item.filename.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
            NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, NULL);

        if (hFile == INVALID_HANDLE_VALUE) {
            log.e(L"Can't open file {}", item.filename);
            queue.pop_back();
            return;
        }

        item.hFile = hFile;
    }

    while (true) {
        item.buffer = Buffer::create(MAX_CHUNK);
        DWORD count;
        bool success = ReadFile(item.hFile, item.buffer->writeData() + sizeof(uint32_t),
            item.buffer->writeSize() - sizeof(uint32_t), &count, NULL);
        if (!success) {
            log.e(L"Error reading from file '{}'", item.filename);
            CloseHandle(item.hFile);
            item.buffer->destroy();
            queue.pop_back();
            //CloseSocket(s);
            return;
        }
        if (count == 0) {
            // EOF
            log.i(L"Finished sending file '{}'", item.filename);
            CloseHandle(item.hFile);
            item.buffer->destroy();
            queue.pop_back();
            //CloseSocket(s);
            return;
        }
        memcpy(item.buffer->writeData(), &count, sizeof(uint32_t));
        item.buffer->adjustWritePos(count + sizeof(uint32_t));
        bool shouldCork = SendBufferToContact(c, item.buffer);
        item.buffer = nullptr;
        if (shouldCork) {
            corked_[c] = std::move(iter->second);
            uncorked_.erase(iter);
            return;
        }
    }
}

bool DiskThread::SendBufferToContact(const Contact& c, Buffer* buffer) {
    return socketThread_->SendBuffer(c, buffer);
}

void DiskThread::OnMessageReceived(const Contact& c, Buffer::UniquePtr message) {
    ReceiveData& data = receive_[c];
    if (data.hReceiveFile == NULL) {
        std::wstring filename = receivePath_ + L"\\ReceivedFile.txt";
        HANDLE hFile = CreateFile(filename.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
            NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);

        if (hFile == INVALID_HANDLE_VALUE) {
            log.e(L"Can't create file {}", filename);
            return;
        }
        
        data.hReceiveFile = hFile;
    }

    while (message->readSize() != 0) {
        DWORD count;
        if (!WriteFile(data.hReceiveFile, message->readData(), message->readSize(), &count, NULL)) {
            log.e(L"Error writing to file being received");
            CloseHandle(data.hReceiveFile);
            return;
        }
        message->adjustReadPos(count);
    }
}
