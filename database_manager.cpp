#include "database_manager.h"
#include "utils.h" // Gerekirse
#include <iostream> // std::wcout, std::wcerr için
#include <chrono>   // Zaman ölçümü için

// Bu global sayaçlar file_scanner modülüne taşındı ve orada yönetiliyor.
// extern long long g_total_files_found; // file_scanner.cpp içinde static std::atomic oldu
// extern long long g_total_dirs_found;  // file_scanner.cpp içinde static std::atomic oldu


DatabaseManager::DatabaseManager(const std::string& db_filename) : db_path_(db_filename) {
    // open_db() çağrısı main veya ihtiyaç duyulan yerde yapılabilir.
}

DatabaseManager::~DatabaseManager() {
    close_db();
}

bool DatabaseManager::open_db() {
    std::lock_guard<std::mutex> lock(db_mutex_);
    if (db_handle_) {
        std::wcout << L"Veritabanı zaten açık: " << widen(db_path_) << std::endl;
        return true;
    }

    if (sqlite3_open(db_path_.c_str(), &db_handle_)) {
        std::wcerr << L"Veritabanı açılamadı (" << widen(db_path_) << L"): " << sqlite3_errmsg16(db_handle_) << std::endl;
        db_handle_ = nullptr;
        return false;
    }
    std::wcout << L"Veritabanı bağlantısı kuruldu: " << widen(db_path_) << std::endl;

    // Performans ayarları
    char* err_msg = nullptr;
    sqlite3_exec(db_handle_, "PRAGMA journal_mode = WAL;", nullptr, nullptr, &err_msg);
    sqlite3_exec(db_handle_, "PRAGMA synchronous = NORMAL;", nullptr, nullptr, &err_msg);
    sqlite3_exec(db_handle_, "PRAGMA temp_store = MEMORY;", nullptr, nullptr, &err_msg);
    sqlite3_exec(db_handle_, "PRAGMA cache_size = -10000;", nullptr, nullptr, &err_msg); // 10MB önbellek
    sqlite3_exec(db_handle_, "PRAGMA busy_timeout = 5000;", nullptr, nullptr, &err_msg); // 5 saniye bekleme süresi
    if (err_msg) {
        std::wcerr << L"PRAGMA ayarlanırken hata: " << sqlite3_errmsg16(db_handle_) << std::endl;
        sqlite3_free(err_msg);
        // Hata durumunda devam edilebilir ama loglamak iyi olur.
    }
    return true;
}

void DatabaseManager::close_db() {
    std::lock_guard<std::mutex> lock(db_mutex_);
    if (db_handle_) {
        sqlite3_wal_checkpoint_v2(db_handle_, nullptr, SQLITE_CHECKPOINT_TRUNCATE, nullptr, nullptr);
        sqlite3_close(db_handle_);
        db_handle_ = nullptr;
        std::wcout << L"Veritabanı bağlantısı kapatıldı: " << widen(db_path_) << std::endl;
    }
}

bool DatabaseManager::init_schema() {
    if (!db_handle_) {
        std::wcerr << L"Veritabanı açık değil, şema oluşturulamıyor." << std::endl;
        return false;
    }
    std::lock_guard<std::mutex> lock(db_mutex_); // DB'ye erişimi kilitle

    char* err_msg = nullptr;

    const char* create_files_table_query = R"(
        CREATE TABLE IF NOT EXISTS files (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            drive TEXT NOT NULL,
            path TEXT NOT NULL,
            name TEXT NOT NULL,
            extension TEXT,
            size INTEGER DEFAULT 0,
            creation_time INTEGER DEFAULT 0,
            modification_time INTEGER DEFAULT 0,
            access_time INTEGER DEFAULT 0,
            mft_change_time INTEGER DEFAULT 0,
            mft_ref INTEGER UNIQUE NOT NULL,
            parent_mft_ref INTEGER NOT NULL,
            attributes INTEGER DEFAULT 0,
            is_directory INTEGER NOT NULL DEFAULT 0,
            is_deleted INTEGER NOT NULL DEFAULT 0
        );
    )";

    const char* create_fts_table_query = R"(
        CREATE VIRTUAL TABLE IF NOT EXISTS fts_files USING fts5(
            fts_content,
            content='files',
            content_rowid='id',
            tokenize='unicode61 remove_diacritics 2'
        );
    )";

    const char* create_fts_triggers_query = R"(
        CREATE TRIGGER IF NOT EXISTS files_ai AFTER INSERT ON files BEGIN
            INSERT INTO fts_files (rowid, fts_content) VALUES (new.id, new.path || '\' || new.name);
        END;
        CREATE TRIGGER IF NOT EXISTS files_ad AFTER DELETE ON files BEGIN
            DELETE FROM fts_files WHERE rowid = old.id;
        END;
        CREATE TRIGGER IF NOT EXISTS files_au AFTER UPDATE ON files BEGIN
            UPDATE fts_files SET fts_content = new.path || '\' || new.name WHERE rowid = old.id;
        END;
    )";

    if (sqlite3_exec(db_handle_, create_files_table_query, nullptr, nullptr, &err_msg) != SQLITE_OK) {
        std::wcerr << L"Ana tablo oluşturulurken hata: " << sqlite3_errmsg16(db_handle_) << std::endl;
        sqlite3_free(err_msg);
        return false;
    }
    if (sqlite3_exec(db_handle_, create_fts_table_query, nullptr, nullptr, &err_msg) != SQLITE_OK) {
        std::wcerr << L"FTS tablosu oluşturulurken hata: " << sqlite3_errmsg16(db_handle_) << std::endl;
        std::wcerr << L"Muhtemel Neden: SQLite sürümünüz FTS5'i desteklemiyor olabilir veya FTS5 derleme sırasında etkinleştirilmemiş olabilir." << std::endl;
        sqlite3_free(err_msg);
        return false;
    }
    if (sqlite3_exec(db_handle_, create_fts_triggers_query, nullptr, nullptr, &err_msg) != SQLITE_OK) {
        std::wcerr << L"FTS tetikleyicileri oluşturulurken hata: " << sqlite3_errmsg16(db_handle_) << std::endl;
        sqlite3_free(err_msg);
        return false;
    }

    sqlite3_exec(db_handle_, "CREATE INDEX IF NOT EXISTS idx_files_path ON files (path);", nullptr, nullptr, &err_msg);
    sqlite3_exec(db_handle_, "CREATE INDEX IF NOT EXISTS idx_files_name ON files (name);", nullptr, nullptr, &err_msg);
    sqlite3_exec(db_handle_, "CREATE INDEX IF NOT EXISTS idx_files_parent ON files (parent_mft_ref);", nullptr, nullptr, &err_msg);
    sqlite3_exec(db_handle_, "CREATE INDEX IF NOT EXISTS idx_files_size ON files (size);", nullptr, nullptr, &err_msg);
    sqlite3_exec(db_handle_, "CREATE INDEX IF NOT EXISTS idx_files_modtime ON files (modification_time);", nullptr, nullptr, &err_msg);
    sqlite3_exec(db_handle_, "CREATE INDEX IF NOT EXISTS idx_files_name_size ON files (name, size);", nullptr, nullptr, &err_msg);

    std::wcout << L"Veritabanı şeması hazırlandı." << std::endl;
    return true;
}

bool DatabaseManager::write_entries(const std::vector<FileData>& file_entries) {
    if (!db_handle_ || file_entries.empty()) {
        return false;
    }
    std::lock_guard<std::mutex> lock(db_mutex_);

    std::wcout << L"📥 " << file_entries.size() << L" adet girdi veritabanına yazılıyor..." << std::endl;
    auto start_time = std::chrono::high_resolution_clock::now();

    char* err_msg = nullptr;
    if (sqlite3_exec(db_handle_, "BEGIN TRANSACTION;", nullptr, nullptr, &err_msg) != SQLITE_OK) {
        std::wcerr << L"Transaction başlatılamadı: " << sqlite3_errmsg16(db_handle_) << std::endl;
        sqlite3_free(err_msg);
        return false;
    }

    const char* insert_sql = R"(
        INSERT INTO files (drive, path, name, extension, size, creation_time, modification_time,
                           access_time, mft_change_time, mft_ref, parent_mft_ref, attributes,
                           is_directory, is_deleted)
        VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
        ON CONFLICT(mft_ref) DO UPDATE SET
            drive=excluded.drive,
            path=excluded.path,
            name=excluded.name,
            extension=excluded.extension,
            size=excluded.size,
            creation_time=excluded.creation_time,
            modification_time=excluded.modification_time,
            access_time=excluded.access_time,
            mft_change_time=excluded.mft_change_time,
            parent_mft_ref=excluded.parent_mft_ref,
            attributes=excluded.attributes,
            is_directory=excluded.is_directory,
            is_deleted=excluded.is_deleted;
    )";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_handle_, insert_sql, -1, &stmt, nullptr) != SQLITE_OK) {
        std::wcerr << L"SQLite Prepare Hatası: " << sqlite3_errmsg16(db_handle_) << std::endl;
        sqlite3_exec(db_handle_, "ROLLBACK;", nullptr, nullptr, nullptr);
        return false;
    }

    int success_count = 0;
    for (const auto& file : file_entries) {
        sqlite3_bind_text16(stmt, 1, file.drive.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text16(stmt, 2, file.path.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text16(stmt, 3, file.name.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text16(stmt, 4, file.extension.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(stmt, 5, file.size);
        sqlite3_bind_int64(stmt, 6, file.creation_time);
        sqlite3_bind_int64(stmt, 7, file.modification_time);
        sqlite3_bind_int64(stmt, 8, file.access_time);
        sqlite3_bind_int64(stmt, 9, file.mft_change_time);
        sqlite3_bind_int64(stmt, 10, file.mft_reference_number);
        sqlite3_bind_int64(stmt, 11, file.parent_mft_reference_number);
        sqlite3_bind_int(stmt, 12, file.file_attributes);
        sqlite3_bind_int(stmt, 13, file.is_directory ? 1 : 0);
        sqlite3_bind_int(stmt, 14, file.is_deleted ? 1 : 0);

        if (sqlite3_step(stmt) != SQLITE_DONE) {
            std::wcerr << L"Veri eklenirken/güncellenirken hata (MFT Ref: " << file.mft_reference_number << L"): "
                       << sqlite3_errmsg16(db_handle_) << std::endl;
            sqlite3_exec(db_handle_, "ROLLBACK;", nullptr, nullptr, nullptr);
            sqlite3_finalize(stmt); // finalize before returning
            return false;
        }
        success_count++;
        sqlite3_reset(stmt);
    }

    if (sqlite3_exec(db_handle_, "COMMIT;", nullptr, nullptr, &err_msg) != SQLITE_OK) {
        std::wcerr << L"Transaction tamamlanırken hata: " << sqlite3_errmsg16(db_handle_) << std::endl;
        sqlite3_free(err_msg);
        sqlite3_finalize(stmt); // finalize before returning
        return false;
    }

    auto end_time = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);
    std::wcout << L"✅ " << success_count << L" girdi başarıyla veritabanına yazıldı/güncellendi (" << duration.count() << L" ms)." << std::endl;

    sqlite3_finalize(stmt);
    return true;
}