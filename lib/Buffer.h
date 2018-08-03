#pragma once

#include <stdint.h>
#include <assert.h>

class Buffer {
public:
    static int count;
    static Buffer* create(size_t capacity) {
        uint8_t* p = new uint8_t[sizeof(Buffer) + capacity];
        Buffer* b = new (p) Buffer(capacity);
        return b;
    }

    void destroy() {
        this->~Buffer();
        uint8_t* p = (uint8_t*)this;
        delete[] p;
    }
    
    // Returns a pointer to the raw buffer (of size capacity)
    uint8_t* buffer() { return buffer_; }
    uint8_t* buffer() const { return buffer_; }
    size_t capacity() const { return capacity_; }
    
    // Returns a pointer to the byte at readPos
    uint8_t* readData() const { return buffer() + readPos(); }

    // Returns a pointer to the byte at readPos
    uint8_t* writeData() { return buffer() + writePos(); }

    size_t readPos() const { assert(readPos_ <= capacity()); return readPos_; }
    size_t writePos() const { assert(writePos_ <= capacity()); return writePos_; }

    size_t readSize() const { assert(readPos() <= writePos());  return writePos() - readPos(); }
    size_t writeSize() const { assert(writePos() <= capacity()); return capacity() - writePos(); }

    void adjustReadPos(intptr_t diff) { readPos_ += diff; assert(readPos_ <= capacity()); }
    void adjustWritePos(intptr_t diff) { writePos_ += diff; assert(writePos_ <= capacity()); }

private:
    Buffer(size_t capacity)
        : buffer_((uint8_t*)(this + 1))
        , capacity_(capacity)
    {}

    uint8_t* buffer_;
    size_t capacity_;
    size_t readPos_ = 0;
    size_t writePos_ = 0;
};
