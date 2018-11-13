#pragma once

#include <string>
#include <optional>
#include "../lib/Buffer.h"

template <class T>
class deprecated {};

class Serializer {
public:
    template <class T>
    Buffer::UniquePtr serialize(T t);

    template <class T>
    bool deserialize(T& t, Buffer* buf);
};

struct SizeHelper {
    size_t getSize() const {
        return size_;
    }
    void operator()(int, uint8_t) { size_ += 2; }
    void operator()(int, uint16_t) { size_ += 3; }
    void operator()(int, uint32_t) { size_ += 5; }
    void operator()(int, uint64_t) { size_ += 9; }
    void operator()(int, const std::string & s) { size_ += 5 + s.size(); }
    template <class T>
    void operator ()(int id, const std::optional<T>& v) { size_ += 2; if (v) { (*this)(id, *v); } }
    template <class T>
    void operator ()(int id, const deprecated<T>& v) { }
private:
    size_t size_ = 0;
};

struct SerializeHelper {
    SerializeHelper(Buffer* b)
        : b(b)
    {}
    void operator()(int id, bool v) { i(id); w((uint8_t)v); }
    void operator()(int id, uint8_t v) { i(id); w(v); }
    void operator()(int id, uint16_t v) { i(id); w(v); }
    void operator()(int id, uint32_t v) { i(id); w(v); }
    void operator()(int id, uint64_t v) { i(id); w(v); }
    void operator()(int id, const std::string& v) {
        i(id);
        w((uint32_t)v.size());
        memcpy(b->writeData(), v.c_str(), v.size());
        b->adjustWritePos(v.size());
    }
    template <class T>
    void operator()(int id, const std::optional<T>& v) {
        i(id);
        w((uint8_t)(v ? 1 : 0));
        if (v) {
            w(*v);
        }
    }
    template <class T>
    void operator()(int id, const deprecated<T>& v) {
    }
private:
    Buffer * b;
    void i(int id) {
        w((uint8_t)id);
    }
    template <class T>
    void w(T v) {
        memcpy(b->writeData(), &v, sizeof(v));
        b->adjustWritePos(sizeof(v));
    }
};

template <class T>
Buffer::UniquePtr Serializer::serialize(T t) {
    SizeHelper sizeHelper;
    t.visit(sizeHelper);
    size_t size = sizeHelper.getSize() + 1;
    Buffer::UniquePtr buffer(Buffer::create(size));
    SerializeHelper serHelper(buffer.get());
    t.visit(serHelper);
    *buffer->writeData() = 0;   // id=0 marks end of struct
    buffer->adjustWritePos(1);
    return buffer;
}

struct DeserializeHelper {
    DeserializeHelper(Buffer* buf)
        : b(buf)
    {}
    void operator()(int id, bool& v) { readHelper(id, v); }
    void operator()(int id, uint8_t& v) { readHelper(id, v); }
    void operator()(int id, uint16_t& v) { readHelper(id, v); }
    void operator()(int id, uint32_t& v) { readHelper(id, v); }
    void operator()(int id, uint64_t& v) { readHelper(id, v); }
    void operator()(int id, std::string& v) { readHelper(id, v); }
    template <class T>
    void operator()(int id, std::optional<T>& v) { readHelper(id, v); }
    template <class T>
    void operator()(int id, deprecated<T>& v) { readHelper(id, v); }
private:
    Buffer* b;

    template <class T>
    void readHelper(int id, T& v) {
        while (1) {
            int realId = readId();
            if (realId == 0) {
                b->adjustReadPos(-1);
                return;
            }
            if (realId == id) {
                readImpl(v);
                return;
            }
            if (realId < id) {
                skip<T>();
            } else if (realId > id) {
                b->adjustReadPos(-1);
                return;
            }
        }
    }

    int readId() {
        uint8_t id;
        readImpl(id);
        return id;
    }

    template <class T>
    void skip() {
        T t;
        readImpl(t);
    }

    void readImpl(bool& v) { uint8_t val; readImpl(val); v = val; }
    void readImpl(uint8_t& v) { b->ensureHasReadData(sizeof(v)); memcpy(&v, b->readData(), sizeof(v)); b->adjustReadPos(sizeof(v)); }
    void readImpl(uint16_t& v) { b->ensureHasReadData(sizeof(v)); memcpy(&v, b->readData(), sizeof(v)); b->adjustReadPos(sizeof(v)); }
    void readImpl(uint32_t& v) { b->ensureHasReadData(sizeof(v)); memcpy(&v, b->readData(), sizeof(v)); b->adjustReadPos(sizeof(v)); }
    void readImpl(uint64_t& v) { b->ensureHasReadData(sizeof(v)); memcpy(&v, b->readData(), sizeof(v)); b->adjustReadPos(sizeof(v)); }
    void readImpl(std::string& v) {
        uint32_t size;
        readImpl(size);
        b->ensureHasReadData(size);
        v.resize(size);
        memcpy(&v[0], b->readData(), size);
        b->adjustReadPos(size);
    }
    template <class T>
    void readImpl(std::optional<T>& v) {
        uint8_t present;
        readImpl(present);
        if (present) {
            v.emplace();
            readImpl(*v);
        } else {
            v.reset();
        }
    }
    template <class T>
    void readImpl(deprecated<T>& v) {
        T t;
        readImpl(t);
    }
};

template <class T>
bool Serializer::deserialize(T& t, Buffer* buf) {
    try {
        DeserializeHelper deserHelper(buf);
        t.visit(deserHelper);
        buf->adjustReadPos(1);
        return true;
    } catch (...) {
        return false;
    }
}
