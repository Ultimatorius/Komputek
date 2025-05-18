#pragma once

#include "common.h" // FileData için
#include <sqlite3.h>
#include <vector>
#include <string>
#include <mutex>
#include <cstdint>
#include <memory> // std::unique_ptr için

class DatabaseManager {
private:
    sqlite3* db_handle_ = nullptr;
    std::mutex db_mutex_;
    std::string db_path_;

public:
    DatabaseManager(const std::string& db_filename = DB_FILENAME);
    ~DatabaseManager();

    DatabaseManager(const DatabaseManager&) = delete;
    DatabaseManager& operator=(const DatabaseManager&) = delete;

    bool open_db();
    void close_db();
    bool is_open() const { return db_handle_ != nullptr; }
    sqlite3* get_db_handle() const { return db_handle_; } // Gerekirse, dikkatli kullanılmalı

    bool init_schema();
    bool write_entries(const std::vector<FileData>& file_entries);

    // Yedekleme/Geri Yükleme için arkadaş fonksiyonlar veya public metotlar
    friend bool backup_db(DatabaseManager& db_manager, const std::string& backup_file_path);
    friend bool restore_db(DatabaseManager& db_manager, const std::string& backup_file_path);
};