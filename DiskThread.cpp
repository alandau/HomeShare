#include "DiskThread.h"
#include "proto/file.h"
#include "proto/Serializer.h"
#include "lib/win/encoding.h"
#include "lib/win/raii.h"
#include <ShlObj.h>

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
        size_t index = filename.rfind(L'\\');
        std::wstring relativeFilename = index == std::wstring::npos ? filename : filename.substr(index + 1);
        if (paused_.find(c) != paused_.end()) {
            paused_[c]->queue_.emplace_back(c, filename, relativeFilename);
        } else if (corked_.find(c) != corked_.end()) {
            corked_[c]->queue_.emplace_back(c, filename, relativeFilename);
        } else {
            if (uncorked_.find(c) == uncorked_.end()) {
                uncorked_[c] = std::make_unique<SendData>();
            }
            uncorked_[c]->queue_.emplace_back(c, filename, relativeFilename);
            DoWriteLoop();
        }
    });
}

void DiskThread::Enqueue(const Contact& c, const std::wstring& dir, const std::vector<std::wstring>& files) {
    RunInThread([this, c, dir, files] {
        bool callDoWriteLoop = false;
        SendData* sendData;
        if (paused_.find(c) != paused_.end()) {
            sendData = paused_[c].get();
        } else if (corked_.find(c) != corked_.end()) {
            sendData = corked_[c].get();
        } else {
            if (uncorked_.find(c) == uncorked_.end()) {
                uncorked_[c] = std::make_unique<SendData>();
            }
            sendData = uncorked_[c].get();
            callDoWriteLoop = true;
        }

        uint32_t count = files.size();
        uint64_t size = 0;
        for (const std::wstring& name : files) {
            std::wstring filename = dir + L"\\" + name;
            HANDLE hFile = CreateFile(filename.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);

            if (hFile == INVALID_HANDLE_VALUE) {
                log.e(L"Can't open file {}", filename);
                continue;
            }
            LARGE_INTEGER liSize;
            GetFileSizeEx(hFile, &liSize);
            CloseHandle(hFile);

            size += liSize.QuadPart;
        }
        sendData->queue_.emplace_back(c, count, size);
        for (const std::wstring& name : files) {
            std::wstring filename = dir + L"\\" + name;
            sendData->queue_.emplace_back(c, std::move(filename), name, true);
        }

        log.i(L"Enqueued {} files, {} bytes", count, size);

        if (callDoWriteLoop) {
            DoWriteLoop();
        }
    });
}

void DiskThread::setProgressUpdateCb(std::function<void(const Contact& c, const ProgressUpdate& up)> cb) {
    progressUpdateCb_ = std::move(cb);
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
    SCOPE_EXIT{
        DoWriteLoop();
    };
    const Contact& c = iter->first;
    QueueItem& item = queue.front();

    if (item.state == QueueItem::State::SEND_FILE_LIST_HEADER) {
        SendFileListHeader header;
        header.count = item.count;
        header.size = item.size;
        Buffer::UniquePtr buffer = Serializer().serialize(header);
        SendBufferToContact(c, SENDFILE_LIST, std::move(buffer));

        progressMap_[c].send.totalBytes += item.size;
        progressMap_[c].send.totalFiles += item.count;
        MaybeSendProgressUpdate(c, true);

        queue.pop_front();
        return;
    }

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

        if (!item.dontUpdateSizes) {
            progressMap_[c].send.totalBytes += size.QuadPart;
            progressMap_[c].send.totalFiles++;
            MaybeSendProgressUpdate(c, true);
        }


        SendFileHeader header;
        header.name = Utf16ToUtf8(item.relativeFilename);
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
            item.hash.update(buffer->writeData(), count);
            buffer->adjustWritePos(count);
            progressMap_[c].send.doneBytes += count;
            MaybeSendProgressUpdate(c);
            bool shouldCork = SendBufferToContact(c, SENDFILE_DATA, std::move(buffer));
            numBuffers++;
            if (shouldCork || numBuffers >= MAX_BUFFERS_TO_SEND) {
                return;
            }
        }
    }

    if (item.state == QueueItem::State::SEND_TRAILER) {
        SendFileTrailer trailer;
        trailer.checksum = item.hash.result();
        Buffer::UniquePtr buffer = Serializer().serialize(trailer);
        // Ignore possible corking, since this is the last buffer
        progressMap_[c].send.doneFiles++;
        MaybeSendProgressUpdate(c, true);
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
    Header header;
    memcpy(&header, message->buffer(), sizeof(header));
    message->adjustReadPos(sizeof(header));

    if (header.streamId != 5555) {
        log.e(L"Bad streamId {}", header.streamId);
        return;
    }

    ReceiveData& data = receive_[c];
    if (data.state == ReceiveData::State::RECEIVE_HEADER) {
        if (header.type == SENDFILE_LIST) {
            SendFileListHeader fileListHeader;
            if (!Serializer().deserialize(fileListHeader, message.get())) {
                log.e(L"Can't deserialize SendFileListHeader");
                return;
            }
            if (fileListHeader.count > 0) {
                data.receiveDir = makeReceiveDir();
                data.filelistCount = fileListHeader.count;
                data.filelistCountDone = 0;
                ProgressUpdate::Stats& stats = progressMap_[c].recv;
                stats.totalFiles += fileListHeader.count;
                stats.totalBytes += fileListHeader.size;
                MaybeSendProgressUpdate(c, true);
                log.i(L"Going to receive {} files, {} bytes", fileListHeader.count, fileListHeader.size);
            }
            return;
        }

        if (header.type != SENDFILE_HEADER) {
            log.e(L"Expected type SENDFILE_HEADER, got {}", header.type);
            return;
        }
        SendFileHeader fileHeader;
        if (!Serializer().deserialize(fileHeader, message.get())) {
            log.e(L"Can't deserialize SendFileHeader");
            return;
        }
        std::wstring origFilename = Utf8ToUtf16(fileHeader.name);
        log.i(L"Receiving file '{}' of size {}", origFilename, fileHeader.size);

        if (data.filelistCountDone == data.filelistCount) {
            data.receiveDir.clear();
            data.filelistCount++;
            ProgressUpdate::Stats& stats = progressMap_[c].recv;
            stats.totalFiles++;
            stats.totalBytes += fileHeader.size;
            MaybeSendProgressUpdate(c, true);
        }

        std::wstring filename;
        HANDLE hFile = GetReceiveFile(data.receiveDir, origFilename, filename);

        if (hFile == INVALID_HANDLE_VALUE) {
            log.e(L"Can't create file {}", origFilename);
            return;
        }

        data.hReceiveFile = hFile;
        data.receiveFilename = filename;
        data.receivedCount = 0;
        data.receiveSize = fileHeader.size;
        data.hash.reset();
        data.state = ReceiveData::State::RECEIVE_DATA_OR_TRAILER;
    } else if (data.state == ReceiveData::State::RECEIVE_DATA_OR_TRAILER) {
        if (header.type == SENDFILE_DATA) {
            data.hash.update(message->readData(), message->readSize());
            data.receivedCount += message->readSize();
            progressMap_[c].recv.doneBytes += message->readSize();
            while (message->readSize() != 0) {
                DWORD count;
                if (!WriteFile(data.hReceiveFile, message->readData(), message->readSize(), &count, NULL)) {
                    log.e(L"Error writing to file being received '{}'", data.receiveFilename);
                    CloseHandle(data.hReceiveFile);
                    data.hReceiveFile = NULL;
                    return;
                }
                message->adjustReadPos(count);
            }
            MaybeSendProgressUpdate(c);
        } else if (header.type == SENDFILE_TRAILER) {
            CloseHandle(data.hReceiveFile);
            data.hReceiveFile = NULL;
            SendFileTrailer fileTrailer;
            if (!Serializer().deserialize(fileTrailer, message.get())) {
                log.e(L"Can't deserialize SendFileTrailer");
                return;
            }
            std::string dataHash = data.hash.result();
            if (data.receivedCount != data.receiveSize) {
                log.e(L"Bad size for file '{}', expected {}, received {} bytes",
                    data.receiveFilename, data.receiveSize, data.receivedCount);
            } else if (dataHash != fileTrailer.checksum) {
                log.e(L"Corrupt file '{}', expected hash {}, actual {}",
                    data.receiveFilename, keyToDisplayStr(fileTrailer.checksum), keyToDisplayStr(dataHash));
            } else {
                log.i(L"Finished receiving file '{}', checksum OK", data.receiveFilename);
                // The move will fail if the destination file exists, and the .part file will live on.
                // This is better than overwriting an existing file
                MoveFile((data.receiveFilename + L".part").c_str(), data.receiveFilename.c_str());
            }
            data.filelistCountDone++;
            if (data.filelistCountDone == data.filelistCount) {
                data.receiveDir.clear();
            }
            progressMap_[c].recv.doneFiles++;
            MaybeSendProgressUpdate(c, true);
            data.state = ReceiveData::State::RECEIVE_HEADER;
        } else {
            log.e(L"Expected type SENDFILE_DATA or SENDFILE_TRAILER, got {}", header.type);
            CloseHandle(data.hReceiveFile);
            data.hReceiveFile = NULL;
            return;
        }
    }

}

HANDLE DiskThread::GetReceiveFile(const std::wstring receiveDir, const std::wstring& origFilename, std::wstring& filename) {
    if (origFilename.empty() || origFilename.find(L':') != std::wstring::npos || origFilename[0] == L'\\') {
        return INVALID_HANDLE_VALUE;
    }
    size_t index = origFilename.rfind(L'\\');
    if (index != std::wstring::npos) {
        std::wstring dirToMake = receiveDir + L"\\" + origFilename.substr(0, index);
        int error = SHCreateDirectory(NULL, dirToMake.c_str());
        if (error != ERROR_SUCCESS && error != ERROR_ALREADY_EXISTS) {
            return INVALID_HANDLE_VALUE;
        }
    }
    for (int i = 0; i < 20; i++) {
        std::wstring tempFilename = origFilename;
        if (i != 0) {
            size_t dotPos = tempFilename.rfind(L'.');
            if (dotPos == std::wstring::npos) {
                tempFilename += L"-" + std::to_wstring(i);
            } else {
                tempFilename = tempFilename.substr(0, dotPos) + L"-" + std::to_wstring(i) + tempFilename.substr(dotPos);
            }
        }
        std::wstring candidateFilename = (receiveDir.empty() ? receivePath_ : receiveDir) + L"\\" + tempFilename;
        std::wstring candidateFilenamePart = candidateFilename + L".part";

        HANDLE hFile = CreateFile(candidateFilename.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
            NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_DELETE_ON_CLOSE, NULL);
        if (hFile == INVALID_HANDLE_VALUE) {
            continue;
        }

        HANDLE hPartFile = CreateFile(candidateFilenamePart.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
            NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
        if (hPartFile == INVALID_HANDLE_VALUE) {
            CloseHandle(hFile);
            continue;
        }

        CloseHandle(hFile);
        filename = candidateFilename;
        return hPartFile;
    }
    return INVALID_HANDLE_VALUE;
}

std::wstring DiskThread::makeReceiveDir() {
    auto now = std::chrono::system_clock::now();
    std::time_t t = std::chrono::system_clock::to_time_t(now);
    struct tm tm;
    localtime_s(&tm, &t);
    std::wstring timeStr = fmt::format(L"{}-{:02}-{:02} {:02}-{:02}-{:02}",
        tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
        tm.tm_hour, tm.tm_min, tm.tm_sec);

    std::wstring prefix = receivePath_ + L"\\" + timeStr;

    for (int i = 0; i < 20; i++) {
        std::wstring path = i == 0 ? prefix : fmt::format(L"{}-{}", prefix, i);
        if (CreateDirectory(path.c_str(), NULL)) {
            return path;
        }
    }

    return L"";
}

void DiskThread::MaybeSendProgressUpdate(const Contact& c, bool force) {
    if (!progressUpdateCb_) {
        return;
    }
    auto it = progressMap_.find(c);
    if (it == progressMap_.end()) {
        return;
    }
    ProgressUpdate& data = it->second;
    auto now = std::chrono::steady_clock::now();
    if (!force && now - data.timestamp < std::chrono::milliseconds(500)) {
        return;
    }
    data.timestamp = now;

    progressUpdateCb_(c, data);
}
