#include "DiskThread.h"
#include "proto/file.h"
#include "proto/Serializer.h"
#include "lib/win/encoding.h"
#include "lib/win/raii.h"

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
    if (!uncorked_.empty()) {
        RunInThread([this] {
            if (!uncorked_.empty()) {
                DoWriteLoopImpl(uncorked_.begin());
            }
        });
    }
}

void DiskThread::DoWriteLoopImpl(Map::iterator iter) {
    enum { MAX_CHUNK = 65536 };
    std::deque<QueueItem>& queue = iter->second->queue_;
    if (queue.empty()) {
        uncorked_.erase(iter);
        return;
    }
    SCOPE_EXIT {
        DoWriteLoop();
    };
    const Contact& c = iter->first;
    QueueItem& item = queue.front();

    if (item.state == QueueItem::State::SEND_HEADER) {
        HANDLE hFile = CreateFile(item.filename.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
            NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, NULL);

        if (hFile == INVALID_HANDLE_VALUE) {
            log.e(L"Can't open file {}", item.filename);
            queue.pop_front();
            return;
        }
        log.i(L"Starting to send '{}'", item.filename);

        item.hFile = hFile;
        item.state = QueueItem::State::SEND_DATA;
        LARGE_INTEGER size;
        GetFileSizeEx(hFile, &size);
        SendFileHeader header;
        header.name = Utf16ToUtf8(item.filename);
        size_t backslash = header.name.rfind('\\');
        if (backslash != std::string::npos) {
            header.name = header.name.substr(backslash + 1);
        }
        header.size = size.QuadPart;
        Buffer::UniquePtr buffer = Serializer().serialize(header);
        if (SendBufferToContact(c, SENDFILE_HEADER, std::move(buffer))) {
            return;
        }
    }

    if (item.state == QueueItem::State::SEND_DATA) {
        int numBuffers = 0;
        while (true) {
            Buffer::UniquePtr buffer(Buffer::create(MAX_CHUNK));
            DWORD count;
            bool success = ReadFile(item.hFile, buffer->writeData(),
                buffer->writeSize(), &count, NULL);
            if (!success) {
                log.e(L"Error reading from file '{}'", item.filename);
                CloseHandle(item.hFile);
                queue.pop_front();
                //CloseSocket(s);
                return;
            }
            if (count == 0) {
                // EOF
                log.i(L"Finished sending file '{}'", item.filename);
                CloseHandle(item.hFile);
                item.state = QueueItem::State::SEND_TRAILER;
                //CloseSocket(s);
                return;
            }
            buffer->adjustWritePos(count);
            bool shouldCork = SendBufferToContact(c, SENDFILE_DATA, std::move(buffer));
            numBuffers++;
            if (shouldCork || numBuffers >= MAX_BUFFERS_TO_SEND) {
                return;
            }
        }
    }

    if (item.state == QueueItem::State::SEND_TRAILER) {
        SendFileTrailer trailer;
        trailer.checksum = 12345;
        Buffer::UniquePtr buffer = Serializer().serialize(trailer);
        // Ignore possible corking, since this is the last buffer
        SendBufferToContact(c, SENDFILE_TRAILER, std::move(buffer));
        queue.pop_front();
    }
}

bool DiskThread::SendBufferToContact(const Contact& c, MessageType type, Buffer::UniquePtr buffer) {
    Header header;
    header.streamId = 5555;
    header.type = type;
    uint8_t* buf = buffer->prependHeader(sizeof(header));
    memcpy(buf, &header, sizeof(header));
    bool shouldCork = socketThread_->SendBuffer(c, buffer.release());
    if (shouldCork) {
        corked_[c] = std::move(uncorked_[c]);
        uncorked_.erase(c);
    }
    return shouldCork;
}

void DiskThread::OnMessageReceived(const Contact& c, Buffer::UniquePtr message) {
    ReceiveData& data = receive_[c];

    Header header;
    memcpy(&header, message->buffer(), sizeof(header));
    message->adjustReadPos(sizeof(header));

    if (header.streamId != 5555) {
        log.e(L"Bad streamId {}", header.streamId);
        return;
    }
    if (data.state == ReceiveData::State::RECEIVE_HEADER) {
        if (header.type != SENDFILE_HEADER) {
            log.e(L"Expected type SENDFILE_HEADER, got {}", header.type);
            return;
        }
        SendFileHeader fileHeader = Serializer().deserialize<SendFileHeader>(message.get());
        log.i(L"Receiving file '{}' of size {}", Utf8ToUtf16(fileHeader.name), fileHeader.size);

        std::wstring filename = receivePath_ + L"\\ReceivedFile.txt";
        HANDLE hFile = CreateFile(filename.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
            NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);

        if (hFile == INVALID_HANDLE_VALUE) {
            log.e(L"Can't create file {}", filename);
            return;
        }

        data.hReceiveFile = hFile;
        data.state = ReceiveData::State::RECEIVE_DATA_OR_TRAILER;
    } else if (data.state == ReceiveData::State::RECEIVE_DATA_OR_TRAILER) {
        if (header.type == SENDFILE_DATA) {
            while (message->readSize() != 0) {
                DWORD count;
                if (!WriteFile(data.hReceiveFile, message->readData(), message->readSize(), &count, NULL)) {
                    log.e(L"Error writing to file being received");
                    CloseHandle(data.hReceiveFile);
                    data.hReceiveFile = NULL;
                    return;
                }
                message->adjustReadPos(count);
            }
        } else if (header.type == SENDFILE_TRAILER) {
            SendFileTrailer fileTrailer = Serializer().deserialize<SendFileTrailer>(message.get());
            log.i(L"Finished receiving file, checksum = {}", fileTrailer.checksum);
            CloseHandle(data.hReceiveFile);
            data.state = ReceiveData::State::RECEIVE_HEADER;
            data.hReceiveFile = NULL;
        } else {
            log.e(L"Expected type SENDFILE_DATA or SENDFILE_TRAILER, got {}", header.type);
            CloseHandle(data.hReceiveFile);
            return;
        }
    }

}
