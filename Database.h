#pragma once

#include "Logger.h"
#include <string>
#include <vector>
#include "lib/sqlite3.h"

class Database {
public:
    struct Contact {
        int id;
        std::string pubkey;
        std::wstring name;
        std::string host;
    };
    Database(Logger& logger)
        : log(logger)
    {}
    ~Database();
    bool OpenOrCreate(const std::wstring& path);
    void GetKeys(std::string* pub, std::string* priv);
    std::vector<Contact> GetContacts();
    void AddContact(const std::string& pubkey, const std::wstring& name);
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
