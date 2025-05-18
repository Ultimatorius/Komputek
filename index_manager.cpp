#include "index_manager.h"
#include "utils.h" // widen için
#include <iostream>
#include <fstream> // Dosya kopyalama için (basit yöntem)
#include <sqlite3.h>

// SQLite online backup API'sini kullanan helper fonksiyon
bool backup_db_sqlite_api(sqlite3* pInMemory, /* Kaynak DB handle */
                          const char *zFilename /* Hedef dosya adı */ ) {
    int rc;                     /* Function return code */
    sqlite3 *pFile;             /* Database connection opened on zFilename */
    sqlite3_backup *pBackup;    /* Backup object used to copy data */

    /* Open the database file identified by zFilename. Exit early if this fails. */
    rc = sqlite3_open(zFilename, &pFile);
    if( rc==SQLITE_OK ){
        /* If the database was opened successfully, proceed */
        pBackup = sqlite3_backup_init(pFile, "main", pInMemory, "main");
        if( pBackup ){
            (void)sqlite3_backup_step(pBackup, -1); // Hepsini kopyala
            (void)sqlite3_backup_finish(pBackup);
        }
        rc = sqlite3_errcode(pFile);
    }
    (void)sqlite3_close(pFile);
    return rc == SQLITE_OK;
}


void save_index(DatabaseManager& db_manager, const std::string& backup_file_path) {
    if (!db_manager.is_open()) {
        std::wcerr << L"Veritabanı açık değil. İndeks kaydedilemiyor." << std::endl;
        return;
    }

    std::wcout << L"İndeks '" << widen(backup_file_path) << L"' dosyasına kaydediliyor..." << std::endl;
    
    // DatabaseManager içindeki mutex'i kilitlemek önemli
    std::unique_lock<std::mutex> lock(const_cast<std::mutex&>(db_manager.*(&DatabaseManager::db_mutex_)));

    bool success = backup_db_sqlite_api(db_manager.get_db_handle(), backup_file_path.c_str());
    
    lock.unlock(); // İşlem bitti, kilidi bırak

    if (success) {
        std::wcout << L"✅ İndeks başarıyla kaydedildi: " << widen(backup_file_path) << std::endl;
    } else {
        std::wcerr << L"❌ İndeks kaydedilirken hata oluştu. SQLite Hata Kodu: " 
                   //<< sqlite3_errcode(db_manager.get_db_handle()) // Bu, kaynak DB'nin son hatasını verir, yedekleme hatasını değil.
                   << L" (Detaylar için konsol çıktısını kontrol edin)." << std::endl;
    }
}

void load_index(DatabaseManager& db_manager, const std::string& backup_file_path) {
    std::wcout << L"İndeks '" << widen(backup_file_path) << L"' dosyasından geri yükleniyor..." << std::endl;

    // Mevcut veritabanını kapat
    db_manager.close_db(); // Bu zaten kendi içinde mutex kullanır.

    // Basit dosya kopyalama yöntemi (daha güvenli olabilir)
    std::ifstream src(backup_file_path, std::ios::binary);
    if (!src) {
        std::wcerr << L"❌ Yedek dosyası açılamadı: " << widen(backup_file_path) << std::endl;
        // Eski veritabanını tekrar açmayı deneyebiliriz.
        if (!db_manager.open_db()) { /* Log error */ }
        return;
    }

    std::ofstream dst(DB_FILENAME, std::ios::binary | std::ios::trunc);
    if (!dst) {
        std::wcerr << L"❌ Hedef veritabanı dosyası oluşturulamadı/yazılamadı: " << widen(DB_FILENAME) << std::endl;
        src.close();
        if (!db_manager.open_db()) { /* Log error */ }
        return;
    }

    dst << src.rdbuf();
    src.close();
    dst.close();

    // Yeni veritabanını aç ve şemayı kontrol et/oluştur
    if (db_manager.open_db()) {
        if (db_manager.init_schema()) { // Şema yoksa oluşturur, varsa bir şey yapmaz (CREATE IF NOT EXISTS)
            std::wcout << L"✅ İndeks başarıyla geri yüklendi ve veritabanı açıldı." << std::endl;
        } else {
            std::wcerr << L"❌ İndeks geri yüklendi ancak veritabanı şeması başlatılamadı." << std::endl;
        }
    } else {
        std::wcerr << L"❌ İndeks geri yüklendi ancak veritabanı açılamadı." << std::endl;
    }
}