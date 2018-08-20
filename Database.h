#pragma once

#include "Logger.h"
#include <string>
#include "lib/sqlite3.h"

class Database {
public:
    Database(Logger& logger)
        : log(logger)
    {}
    ~Database();
    bool OpenOrCreate(const std::wstring& path);
    void GetKeys(std::string* pub, std::string* priv);
private:
    class Stmt {
    public:
        Stmt(sqlite3_stmt* stmt)
            : stmt(stmt)
        {}
        Stmt(const Stmt&) = delete;
        Stmt(Stmt&& other)
            : stmt(other.stmt)
        {
            other.stmt = nullptr;
        }
        ~Stmt() {
            sqlite3_finalize(stmt);
        }
        sqlite3_stmt* get() {
            return stmt;
        }
    private:
        sqlite3_stmt* stmt = nullptr;
    };

    sqlite3* db = nullptr;
    Logger& log;

    Stmt createStatement(const char* sql);
    void queryExec(const char* sql);
    int queryInt(const char* sql);

    void initSchema();
    void upgradeDb(int oldver);
};
