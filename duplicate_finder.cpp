#include "duplicate_finder.h"
#include "utils.h" // format_filesize, widen için
#include <iostream>
#include <sqlite3.h>
#include <vector>
#include <map>      // Sonuçları gruplamak için

void find_duplicates(DatabaseManager& db_manager) {
    if (!db_manager.is_open()) {
        std::wcerr << L"Hata: Yinelenen dosyaları bulmak için veritabanı açık değil." << std::endl;
        return;
    }

    std::wcout << L"\nYinelenen dosyalar (isim + boyut eşleşmesi) aranıyor..." << std::endl;

    // Aynı isim ve boyuta sahip dosyaları bul (sadece dosyalar, dizinler değil)
    // GROUP_CONCAT ile yolları birleştir.
    const char* query = "SELECT name, size, GROUP_CONCAT(path || '\\' || name, CHAR(10)) as paths_concat, COUNT(*) as count "
                        "FROM files "
                        "WHERE is_directory = 0 AND is_deleted = 0 " // Sadece silinmemiş dosyalar
                        "GROUP BY name, size "
                        "HAVING COUNT(*) > 1 "
                        "ORDER BY size DESC, name;";

    sqlite3_stmt* stmt = nullptr;
    std::unique_lock<std::mutex> lock(const_cast<std::mutex&>(db_manager.*(&DatabaseManager::db_mutex_)));

    if (sqlite3_prepare_v2(db_manager.get_db_handle(), query, -1, &stmt, nullptr) != SQLITE_OK) {
        std::wcerr << L"Yinelenen dosya sorgusu hazırlanamadı: " << sqlite3_errmsg16(db_manager.get_db_handle()) << std::endl;
        return;
    }
    lock.unlock();

    std::wcout << L"--------------------------------------------------------------------------------\n";
    std::wcout << L"Boyut      | Adı                  | Kopya Sayısı | Yollar\n";
    std::wcout << L"--------------------------------------------------------------------------------\n";
    
    int found_groups = 0;
    int step_result;
    while ((step_result = sqlite3_step(stmt)) == SQLITE_ROW) {
        found_groups++;
        std::wstring name_w = widen(reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0)));
        uint64_t size_val = sqlite3_column_int64(stmt, 1);
        std::wstring paths_w = widen(reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2)));
        int count_val = sqlite3_column_int(stmt, 3);

        std::wcout << std::setw(10) << std::left << format_filesize(size_val) << L" | "
                   << std::setw(20) << std::left << name_w.substr(0,20) << L" | "
                   << std::setw(12) << std::left << count_val << L" | \n";
        
        // Yolları ayırıp alt alta yazdır
        std::wstringstream ss_paths(paths_w);
        std::wstring path_line;
        while(std::getline(ss_paths, path_line, L'\n')) { // CHAR(10) ile ayırdık
             std::wcout << L"                                               | " << path_line << std::endl;
        }
        std::wcout << L"--------------------------------------------------------------------------------\n";
    }

    if (step_result != SQLITE_DONE) {
        std::wcerr << L"Yinelenen dosya arama sırasında hata: " << sqlite3_errmsg16(db_manager.get_db_handle()) << std::endl;
    }
    sqlite3_finalize(stmt);

    if (found_groups == 0) {
        std::wcout << L"Yinelenen dosya grubu bulunamadı (isim+boyut eşleşmesi)." << std::endl;
    } else {
        std::wcout << found_groups << L" adet yinelenen dosya grubu bulundu." << std::endl;
    }
     std::wcout << L"--------------------------------------------------------------------------------\n";
}