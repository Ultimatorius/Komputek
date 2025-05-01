#include "database_manager.h"
#include "utils.h"
#include "file_scanner.h"

// SQLite başlık dosyası
#include <sqlite3.h>

// Veritabanı bağlantısı (global - basitlik için, daha iyi tasarımlarda sınıf üyesi olabilir)
sqlite3* g_db = nullptr;
// Veritabanı işlemleri için mutex (paralel tarama durumunda)
std::mutex g_db_mutex;
// Toplam bulunan dosya/dizin sayısı (istatistik için)
long long g_total_files_found = 0;
long long g_total_dirs_found = 0;

// Dosya Adı Ad Alanları (FileName Namespace)
enum class FileNameNamespace : BYTE {
    POSIX = 0,      // Büyük/küçük harf duyarlı
    WIN32 = 1,      // Büyük/küçük harf duyarsız (genellikle kullanılır)
    DOS = 2,      // 8.3 formatı
    WIN32_DOS = 3 // Hem Win32 hem de DOS adı mevcut
};

// Dosya/Dizin bilgilerini saklamak için yapı
struct FileData {
    uint64_t id = 0; // Veritabanı ID'si
    std::wstring drive; // Sürücü harfi (örn: C:\)
    std::wstring path; // Tam yol (hesaplanacak)
    std::wstring name; // Dosya/Dizin adı
    std::wstring extension; // Dosya uzantısı (varsa)
    uint64_t size = 0; // Dosya boyutu (byte)
    uint64_t creation_time = 0; // FILETIME formatında
    uint64_t modification_time = 0; // FILETIME formatında
    uint64_t access_time = 0; // FILETIME formatında
    uint64_t mft_change_time = 0; // $STANDARD_INFORMATION içindeki MFT değişim zamanı
    uint64_t mft_reference_number = 0; // MFT Kayıt Numarası
    uint64_t parent_mft_reference_number = 5; // Ebeveyn MFT Kayıt Numarası (Kök dizin için 5 varsayılır)
    DWORD file_attributes = 0; // Windows dosya öznitelikleri
    bool is_directory = false;
    bool is_deleted = false; // MFT kaydı kullanımda değilse

    // FTS için birleştirilmiş metin (yol + isim)
    std::wstring fts_content() const {
        return path + L"\\" + name;
    }
};

// Veritabanını başlatır ve tabloları oluşturur
bool init_db() {
    std::lock_guard<std::mutex> lock(g_db_mutex); // Global DB'ye erişimi kilitle

    if (g_db) {
        std::wcout << L"Veritabanı zaten açık." << std::endl;
        return true; // Zaten açıksa tekrar açma
    }

    if (sqlite3_open(DB_FILENAME, &g_db)) {
        std::wcerr << L"Veritabanı açılamadı: " << sqlite3_errmsg16(g_db) << std::endl;
        g_db = nullptr;
        return false;
    }
    std::wcout << L"Veritabanı bağlantısı kuruldu: " << DB_FILENAME << std::endl;

    // Performans ayarları
    char* err_msg = nullptr;
    sqlite3_exec(g_db, "PRAGMA journal_mode = WAL;", nullptr, nullptr, &err_msg);
    sqlite3_exec(g_db, "PRAGMA synchronous = NORMAL;", nullptr, nullptr, &err_msg);
    sqlite3_exec(g_db, "PRAGMA temp_store = MEMORY;", nullptr, nullptr, &err_msg);
    sqlite3_exec(g_db, "PRAGMA cache_size = -10000;", nullptr, nullptr, &err_msg); // 10MB önbellek
    sqlite3_exec(g_db, "PRAGMA busy_timeout = 5000;", nullptr, nullptr, &err_msg); // 5 saniye bekleme süresi

    // Ana dosya tablosu
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
            mft_ref INTEGER UNIQUE NOT NULL, -- MFT referansı benzersiz olmalı
            parent_mft_ref INTEGER NOT NULL,
            attributes INTEGER DEFAULT 0,
            is_directory INTEGER NOT NULL DEFAULT 0,
            is_deleted INTEGER NOT NULL DEFAULT 0
        );
    )";

    // Full-Text Search (FTS5) tablosu (dosya yolu ve adı için)
    // content='files' : files tablosu silindiğinde FTS girdileri de silinir
    // content_rowid='id' : files.id ile eşleşir
    // tokenize='unicode61 remove_diacritics 2' : Unicode desteği, aksanları kaldır, noktalama işaretlerini ayırıcı yapma
    const char* create_fts_table_query = R"(
        CREATE VIRTUAL TABLE IF NOT EXISTS fts_files USING fts5(
            fts_content,                 -- İndekslenecek metin (yol + isim)
            content='files',             -- Ana tablo
            content_rowid='id',          -- Ana tablonun ID'si
            tokenize='unicode61 remove_diacritics 2'
        );
    )";

    // FTS tablosunu tetikleyicilerle otomatik güncel tutma
    const char* create_fts_triggers_query = R"(
        -- Dosya eklendiğinde FTS'e ekle
        CREATE TRIGGER IF NOT EXISTS files_ai AFTER INSERT ON files BEGIN
            INSERT INTO fts_files (rowid, fts_content) VALUES (new.id, new.path || '\' || new.name);
        END;
        -- Dosya silindiğinde FTS'den sil (aslında FTS kendi hallediyor content= sayesinde ama garanti olsun)
        CREATE TRIGGER IF NOT EXISTS files_ad AFTER DELETE ON files BEGIN
            DELETE FROM fts_files WHERE rowid = old.id;
        END;
        -- Dosya güncellendiğinde FTS'i güncelle
        CREATE TRIGGER IF NOT EXISTS files_au AFTER UPDATE ON files BEGIN
            UPDATE fts_files SET fts_content = new.path || '\' || new.name WHERE rowid = old.id;
        END;
    )";


    if (sqlite3_exec(g_db, create_files_table_query, nullptr, nullptr, &err_msg) != SQLITE_OK) {
        std::wcerr << L"Ana tablo oluşturulurken hata: " << sqlite3_errmsg16(g_db) << std::endl;
        sqlite3_free(err_msg);
        sqlite3_close(g_db);
        g_db = nullptr;
        return false;
    }
    if (sqlite3_exec(g_db, create_fts_table_query, nullptr, nullptr, &err_msg) != SQLITE_OK) {
        std::wcerr << L"FTS tablosu oluşturulurken hata: " << sqlite3_errmsg16(g_db) << std::endl;
        std::wcerr << L"Muhtemel Neden: SQLite sürümünüz FTS5'i desteklemiyor olabilir veya FTS5 derleme sırasında etkinleştirilmemiş olabilir." << std::endl;
        sqlite3_free(err_msg);
        // FTS olmadan devam edilebilir ama arama yavaş olur. Şimdilik kapatıyoruz.
        sqlite3_close(g_db);
        g_db = nullptr;
        return false;
    }
    if (sqlite3_exec(g_db, create_fts_triggers_query, nullptr, nullptr, &err_msg) != SQLITE_OK) {
        std::wcerr << L"FTS tetikleyicileri oluşturulurken hata: " << sqlite3_errmsg16(g_db) << std::endl;
        sqlite3_free(err_msg);
        // Tetikleyiciler olmadan FTS çalışmaz, kapatıyoruz.
        sqlite3_close(g_db);
        g_db = nullptr;
        return false;
    }

    // İndeksler (Arama ve sıralama performansını artırır)
    // MFT Ref zaten UNIQUE olduğu için otomatik indekslidir.
    sqlite3_exec(g_db, "CREATE INDEX IF NOT EXISTS idx_files_path ON files (path);", nullptr, nullptr, &err_msg);
    sqlite3_exec(g_db, "CREATE INDEX IF NOT EXISTS idx_files_name ON files (name);", nullptr, nullptr, &err_msg);
    sqlite3_exec(g_db, "CREATE INDEX IF NOT EXISTS idx_files_parent ON files (parent_mft_ref);", nullptr, nullptr, &err_msg);
    sqlite3_exec(g_db, "CREATE INDEX IF NOT EXISTS idx_files_size ON files (size);", nullptr, nullptr, &err_msg);
    sqlite3_exec(g_db, "CREATE INDEX IF NOT EXISTS idx_files_modtime ON files (modification_time);", nullptr, nullptr, &err_msg);
    sqlite3_exec(g_db, "CREATE INDEX IF NOT EXISTS idx_files_name_size ON files (name, size);", nullptr, nullptr, &err_msg); // Yinelenen bulma için

    std::wcout << L"Veritabanı şeması hazırlandı." << std::endl;
    return true;
}

// Veritabanı bağlantısını kapatır
void close_db() {
    std::lock_guard<std::mutex> lock(g_db_mutex);
    if (g_db) {
        // Bekleyen WAL checkpoint'ini yap (veriyi ana db dosyasına yaz)
        sqlite3_wal_checkpoint_v2(g_db, nullptr, SQLITE_CHECKPOINT_TRUNCATE, nullptr, nullptr);
        sqlite3_close(g_db);
        g_db = nullptr;
        std::wcout << L"Veritabanı bağlantısı kapatıldı." << std::endl;
    }
}

// Toplu halde FileData nesnelerini veritabanına yazar
// Hata durumunda rollback yapar.
// NOT: Bu fonksiyon FTS tetikleyicilerine güvenir.
bool write_to_db(const std::vector<FileData>& file_entries) {
    if (!g_db || file_entries.empty()) {
        return false;
    }

    // Veritabanı işlemi için kilitle
    std::lock_guard<std::mutex> lock(g_db_mutex);

    std::wcout << L"📥 " << file_entries.size() << L" adet girdi veritabanına yazılıyor..." << std::endl;
    auto start_time = std::chrono::high_resolution_clock::now();

    char* err_msg = nullptr;
    // Transaction başlat
    if (sqlite3_exec(g_db, "BEGIN TRANSACTION;", nullptr, nullptr, &err_msg) != SQLITE_OK) {
        std::wcerr << L"Transaction başlatılamadı: " << sqlite3_errmsg16(g_db) << std::endl;
        sqlite3_free(err_msg);
        return false;
    }

    // Hazırlanmış sorgu (performans için)
    // ON CONFLICT(mft_ref) DO UPDATE: Eğer aynı MFT referansı zaten varsa, eski kaydı güncelle.
    // Bu, tekrar tarama yapıldığında verilerin güncellenmesini sağlar.
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

    // Hazırlama fonksiyonu (wchar_t için, UTF-8 daha standart)
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(g_db, insert_sql, -1, &stmt, nullptr) != SQLITE_OK) {
        std::wcerr << L"SQLite Prepare Hatası: " << sqlite3_errmsg16(g_db) << std::endl;
        sqlite3_exec(g_db, "ROLLBACK;", nullptr, nullptr, nullptr); // Hata durumunda geri al
        return false;
    }

    int success_count = 0;
    for (const auto& file : file_entries) {
        // UTF-16 wstring'leri bind etmek için sqlite3_bind_text16 kullanılır
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
                       << sqlite3_errmsg16(g_db) << std::endl;
            // Tek bir hata tüm işlemi geri almasın diye devam edebiliriz, ama transaction'ı bozabilir.
            // Şimdilik işlemi geri alıp çıkalım.
            sqlite3_exec(g_db, "ROLLBACK;", nullptr, nullptr, nullptr);
            return false;
        }
        success_count++;
        sqlite3_reset(stmt); // Statement'ı sonraki kullanım için resetle
    }

    // Transaction'ı bitir (Commit)
    if (sqlite3_exec(g_db, "COMMIT;", nullptr, nullptr, &err_msg) != SQLITE_OK) {
        std::wcerr << L"Transaction tamamlanırken hata: " << sqlite3_errmsg16(g_db) << std::endl;
        sqlite3_free(err_msg);
        // Commit hatası ciddi bir sorundur, ancak burada sadece loglayıp geçiyoruz.
        return false; // Başarısız kabul edelim
    }

    auto end_time = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);
    std::wcout << L"✅ " << success_count << L" girdi başarıyla veritabanına yazıldı/güncellendi (" << duration.count() << L" ms)." << std::endl;

    sqlite3_finalize(stmt);
    return true;
}