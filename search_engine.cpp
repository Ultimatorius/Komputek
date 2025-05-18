#include "search_engine.h"
#include "utils.h" // format_filesize, filetime_to_wstring için
#include <iostream>
#include <sqlite3.h>
#include <vector>   // Sonuçları saklamak için
#include <chrono>   // Zaman ölçümü için

// search_files FileData yapısını kullanabilir veya doğrudan sütunları alabilir.
// Şimdilik FileData'yı kullanalım.
#include "common.h" // FileData için

void search_files(DatabaseManager& db_manager) {
    if (!db_manager.is_open()) {
        std::wcerr << L"Hata: Arama için veritabanı açık değil." << std::endl;
        return;
    }

    std::wstring search_term_w;
    std::wcout << L"\nAranacak metni girin (FTS5 sorgu sözdizimi kullanılabilir, ör: 'belge NEAR/5 resim'): ";
    std::getline(std::wcin >> std::ws, search_term_w);

    if (search_term_w.empty()) {
        std::wcout << L"Arama terimi boş." << std::endl;
        return;
    }
    
    std::string search_term_n = narrow(search_term_w); // SQLite UTF-8 ile çalışır

    // FTS sorgusu. path, name, size, modification_time bilgilerini alalım.
    // İsteğe bağlı olarak rank'e göre sıralama.
    std::string fts_query_str = "SELECT f.id, f.drive, f.path, f.name, f.extension, f.size, "
                                "f.creation_time, f.modification_time, f.access_time, f.mft_change_time, "
                                "f.mft_ref, f.parent_mft_ref, f.attributes, f.is_directory, f.is_deleted "
                                "FROM files f JOIN fts_files ft ON f.id = ft.rowid "
                                "WHERE ft.fts_files MATCH ? ORDER BY rank DESC, f.path, f.name;";
                                // rank DESC yerine f.path, f.name de olabilir.

    std::wcout << L"'" << search_term_w << L"' için arama yapılıyor..." << std::endl;
    auto query_start_time = std::chrono::high_resolution_clock::now();

    sqlite3_stmt* stmt = nullptr;
    // db_manager.get_db_handle() doğrudan kullanılabilir veya DatabaseManager içinde sorgu metodu olabilir.
    // Şimdilik doğrudan kullanalım, DatabaseManager::db_mutex_ zaten dışarıdan yönetiliyor.
    // Ancak idealde DatabaseManager içinde sorgu çalıştırma metotları olmalı.
    
    // Kilitleme DatabaseManager içinde yapılmalı. Bu geçici bir çözüm:
    std::unique_lock<std::mutex> lock(const_cast<std::mutex&>(db_manager.*(&DatabaseManager::db_mutex_)));
    
    if (sqlite3_prepare_v2(db_manager.get_db_handle(), fts_query_str.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
        std::wcerr << L"Arama sorgusu hazırlanamadı: " << sqlite3_errmsg16(db_manager.get_db_handle()) << std::endl;
        return;
    }
    lock.unlock(); // prepare sonrası kilidi bırakabiliriz, step kendi senkronizasyonunu yapar (veya yapmaz, stmt kullanımı thread-safe değil)
                   // Eğer stmt'yi başka thread'ler kullanmayacaksa sorun yok.

    // Arama terimini SQL'e güvenli bir şekilde bağla
    if (sqlite3_bind_text(stmt, 1, search_term_n.c_str(), -1, SQLITE_TRANSIENT) != SQLITE_OK) {
        std::wcerr << L"Arama terimi bağlanamadı: " << sqlite3_errmsg16(db_manager.get_db_handle()) << std::endl;
        sqlite3_finalize(stmt);
        return;
    }

    std::vector<FileData> results;
    int step_result;
    while ((step_result = sqlite3_step(stmt)) == SQLITE_ROW) {
        FileData entry;
        entry.id = sqlite3_column_int64(stmt, 0);
        entry.drive = widen(reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1)));
        entry.path = widen(reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2)));
        entry.name = widen(reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3)));
        const char* ext_c = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 4));
        entry.extension = ext_c ? widen(ext_c) : L"";
        entry.size = sqlite3_column_int64(stmt, 5);
        entry.creation_time = sqlite3_column_int64(stmt, 6);
        entry.modification_time = sqlite3_column_int64(stmt, 7);
        entry.access_time = sqlite3_column_int64(stmt, 8);
        entry.mft_change_time = sqlite3_column_int64(stmt, 9);
        entry.mft_reference_number = sqlite3_column_int64(stmt, 10);
        entry.parent_mft_reference_number = sqlite3_column_int64(stmt, 11);
        entry.file_attributes = sqlite3_column_int(stmt, 12);
        entry.is_directory = sqlite3_column_int(stmt, 13);
        entry.is_deleted = sqlite3_column_int(stmt, 14); // Bu sorguda silinmişler gelmez ama yine de okuyalım
        results.push_back(entry);
    }

    if (step_result != SQLITE_DONE) {
        std::wcerr << L"Arama sırasında hata: " << sqlite3_errmsg16(db_manager.get_db_handle()) << std::endl;
    }

    sqlite3_finalize(stmt);

    auto query_end_time = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(query_end_time - query_start_time);

    if (results.empty()) {
        std::wcout << L"Sonuç bulunamadı. (" << duration.count() << L" ms)" << std::endl;
    } else {
        std::wcout << results.size() << L" sonuç bulundu (" << duration.count() << L" ms):\n";
        std::wcout << L"--------------------------------------------------------------------------------\n";
        std::wcout << L"Boyut      | Son Değişiklik    | Tip | Yol\n";
        std::wcout << L"--------------------------------------------------------------------------------\n";
        for (const auto& file : results) {
            std::wcout << std::setw(10) << std::left << format_filesize(file.size) << L" | "
                       << std::setw(18) << std::left << filetime_to_wstring(file.modification_time) << L" | "
                       << std::setw(3) << std::left << (file.is_directory ? L"DZN" : L"DSY") << L" | "
                       << file.path << (file.path.empty() || file.path.back() == L'\\' ? L"" : L"\\") << file.name
                       << std::endl;
        }
        std::wcout << L"--------------------------------------------------------------------------------\n";
    }
}