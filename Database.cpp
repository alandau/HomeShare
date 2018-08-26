#include "Database.h"
#include "lib/win/raii.h"
#include "lib/win/encoding.h"
#include "lib/sodium.h"

enum { CURRENT_DB_VERSION = 1 };

Database::~Database() {
    if (db) {
        sqlite3_close(db);
    }
}

bool Database::OpenOrCreate(const std::wstring& path) {
    sqlite3* db;
    int res = sqlite3_open(Utf16ToUtf8(path).c_str(), &db);
    if (res != SQLITE_OK) {
        // db might still be non-null even on error
        if (db) {
            sqlite3_close(db);
        }
        return false;
    }
    this->db = db;
    int curver = queryInt("PRAGMA user_version;");
    if (curver == 0) {
        // Newly created database
        initSchema();
    } else if (curver < CURRENT_DB_VERSION) {
        upgradeDb(curver);
    }
    return true;
}

void Database::GetKeys(std::string* pub, std::string* priv) {
    Stmt stmt = createStatement("SELECT pubkey, privkey FROM keys LIMIT 1");
    int res = sqlite3_step(stmt.get());
    if (res != SQLITE_ROW) {
        log.f(L"No keys in database: {}", res);
        return;
    }
    const char* buf = (const char *)sqlite3_column_blob(stmt.get(), 0);
    pub->assign(buf, sqlite3_column_bytes(stmt.get(), 0));
    
    buf = (const char *)sqlite3_column_blob(stmt.get(), 1);
    priv->assign(buf, sqlite3_column_bytes(stmt.get(), 1));
}

Database::Stmt Database::createStatement(const char* sql) {
    sqlite3_stmt* stmt;
    int res = sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr);
    if (res != SQLITE_OK) {
        log.f(L"Can't prepare statement: {}", res);
        return Stmt(nullptr);
    }
    return Stmt(stmt);
}

int Database::queryInt(const char* sql) {
    Stmt stmt = createStatement(sql);
    int res = sqlite3_step(stmt.get());
    if (res != SQLITE_ROW) {
        log.f(L"No rows: {}", res);
        return 0;
    }
    return sqlite3_column_int(stmt.get(), 0);
}

void Database::queryExec(const char* sql) {
    int res = sqlite3_exec(db, sql, nullptr, nullptr, nullptr);
    if (res != SQLITE_OK) {
        log.f(L"Can't exec sql: {}: {}", res, Utf8ToUtf16(sql));
    }
}

void Database::initSchema() {
    const char *sql = "PRAGMA user_version = {}; "
        "CREATE TABLE keys (pubkey BLOB, privkey BLOB); "
        "CREATE TABLE contacts(id INTEGER PRIMARY KEY, name TEXT, pubkey BLOB, staticip TEXT); "
        "CREATE TABLE settings(key TEXT, value);";
    queryExec(fmt::format(sql, CURRENT_DB_VERSION).c_str());

    unsigned char pub[crypto_sign_PUBLICKEYBYTES], priv[crypto_sign_SECRETKEYBYTES];
    crypto_sign_keypair(pub, priv);
    
    Stmt stmt = createStatement("INSERT INTO keys (pubkey, privkey) VALUES (?,?)");
    sqlite3_bind_blob(stmt.get(), 1, pub, crypto_sign_PUBLICKEYBYTES, SQLITE_STATIC);
    sqlite3_bind_blob(stmt.get(), 2, priv, crypto_sign_SECRETKEYBYTES, SQLITE_STATIC);
    int res = sqlite3_step(stmt.get());
    if (res != SQLITE_DONE) {
        log.f(L"Can't create keypair: {}", res);
    }
}

void Database::upgradeDb(int oldver) {
    log.f(L"Can't upgrade database from version {} to {}", oldver, CURRENT_DB_VERSION);
}

std::vector<Database::Contact> Database::GetContacts() {
    Stmt stmt = createStatement("SELECT id, pubkey, name, staticip FROM contacts");
    int res = sqlite3_step(stmt.get());
    if (res != SQLITE_ROW && res != SQLITE_DONE) {
        log.e(L"Can't query contacts in database: {}", res);
        return {};
    }
    std::vector<Contact> vec;
    while (res == SQLITE_ROW) {
        Contact c;
        c.id = sqlite3_column_int(stmt.get(), 0);
        c.pubkey = (const char *)sqlite3_column_text(stmt.get(), 1);
        c.name = (const wchar_t *)sqlite3_column_text16(stmt.get(), 2);
        const char* host = (const char *)sqlite3_column_text(stmt.get(), 3);
        c.host = host ? host : "";
        vec.push_back(std::move(c));
        res = sqlite3_step(stmt.get());
    }
    return vec;
}

void Database::AddContact(const std::string& pubkey, const std::wstring& name) {
    Stmt stmt = createStatement("INSERT INTO contacts (pubkey, name) VALUES (?,?)");
    sqlite3_bind_blob(stmt.get(), 1, pubkey.data(), pubkey.size(), SQLITE_STATIC);
    sqlite3_bind_text16(stmt.get(), 2, name.c_str(), -1, SQLITE_STATIC);
    int res = sqlite3_step(stmt.get());
    if (res != SQLITE_DONE) {
        log.e(L"Can't add contact: {}", res);
    }
}
