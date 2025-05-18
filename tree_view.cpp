#include "tree_view.h"
#include "utils.h" // widen
#include <iostream>
#include <sqlite3.h>
#include <vector>
#include <iomanip> // std::setw

struct TreeEntry {
    uint64_t mft_ref;
    std::wstring name;
    bool is_directory;
};

// Yardımcı fonksiyon: Belirli bir ebeveynin altındaki girişleri listeler
std::vector<TreeEntry> get_children(DatabaseManager& db_manager, uint64_t parent_mft_ref, const std::wstring& drive_prefix) {
    std::vector<TreeEntry> children;
    // Sadece belirli bir sürücü için MFT ref'ler unique'tir. Drive bilgisi de sorguya eklenebilir.
    // Şimdilik drive_prefix'i sadece isim için kullanıyoruz.
    // parent_mft_ref'e göre sıralı getirelim.
    std::string query_str = "SELECT mft_ref, name, is_directory FROM files "
                            "WHERE parent_mft_ref = ? AND drive = ? AND is_deleted = 0 "
                            "ORDER BY is_directory DESC, name COLLATE NOCASE;"; 
                            // Önce dizinler, sonra dosyalar, alfabetik

    sqlite3_stmt* stmt = nullptr;
    std::unique_lock<std::mutex> lock(const_cast<std::mutex&>(db_manager.*(&DatabaseManager::db_mutex_)));

    if (sqlite3_prepare_v2(db_manager.get_db_handle(), query_str.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
        std::wcerr << L"Ağaç görünümü sorgusu hazırlanamadı: " << sqlite3_errmsg16(db_manager.get_db_handle()) << std::endl;
        return children;
    }

    sqlite3_bind_int64(stmt, 1, parent_mft_ref);
    std::string drive_prefix_n = narrow(drive_prefix);
    sqlite3_bind_text(stmt, 2, drive_prefix_n.c_str(), -1, SQLITE_TRANSIENT);
    
    lock.unlock();


    while (sqlite3_step(stmt) == SQLITE_ROW) {
        TreeEntry entry;
        entry.mft_ref = sqlite3_column_int64(stmt, 0);
        entry.name = widen(reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1)));
        entry.is_directory = sqlite3_column_int(stmt, 2);
        children.push_back(entry);
    }
    sqlite3_finalize(stmt);
    return children;
}

// Rekürsif olarak ağacı yazdırır
void print_tree_recursive(DatabaseManager& db_manager, uint64_t parent_mft_ref, const std::wstring& drive_prefix, const std::wstring& indent, bool is_last_child) {
    std::vector<TreeEntry> children = get_children(db_manager, parent_mft_ref, drive_prefix);
    
    for (size_t i = 0; i < children.size(); ++i) {
        const auto& child = children[i];
        bool current_is_last = (i == children.size() - 1);

        std::wcout << indent;
        if (is_last_child && i == 0 && parent_mft_ref != 5 /*Kök değilse*/) { // Bu mantık tam doğru olmayabilir
             // İlk çocuk ve ebeveyni son çocuksa farklı bir çizgi
        }
        
        if (current_is_last) {
            std::wcout << L"└── ";
        } else {
            std::wcout << L"├── ";
        }
        std::wcout << child.name << (child.is_directory ? L"/" : L"") << std::endl;

        if (child.is_directory) {
            std::wstring new_indent = indent;
            if (current_is_last) {
                new_indent += L"    ";
            } else {
                new_indent += L"│   ";
            }
            print_tree_recursive(db_manager, child.mft_ref, drive_prefix, new_indent, current_is_last);
        }
    }
}


void display_tree_view(DatabaseManager& db_manager) {
    if (!db_manager.is_open()) {
        std::wcerr << L"Hata: Ağaç görünümü için veritabanı açık değil." << std::endl;
        return;
    }

    // Kullanıcıdan hangi sürücünün ağacını görmek istediğini sorabiliriz.
    // Şimdilik, veritabanındaki ilk sürücüyü veya belirli bir sürücüyü alalım.
    // Ya da tüm sürücüleri listeyelim.

    std::wcout << L"\nAğaç Görünümü:" << std::endl;

    // Veritabanından mevcut sürücüleri alalım
    std::vector<std::wstring> drives_in_db;
    const char* query_drives = "SELECT DISTINCT drive FROM files ORDER BY drive;";
    sqlite3_stmt* stmt_drives = nullptr;

    std::unique_lock<std::mutex> lock(const_cast<std::mutex&>(db_manager.*(&DatabaseManager::db_mutex_)));
    if (sqlite3_prepare_v2(db_manager.get_db_handle(), query_drives, -1, &stmt_drives, nullptr) == SQLITE_OK) {
        lock.unlock();
        while(sqlite3_step(stmt_drives) == SQLITE_ROW) {
            drives_in_db.push_back(widen(reinterpret_cast<const char*>(sqlite3_column_text(stmt_drives, 0))));
        }
    } else {
        lock.unlock();
        std::wcerr << L"Veritabanından sürücüler alınamadı: " << sqlite3_errmsg16(db_manager.get_db_handle()) << std::endl;
    }
    sqlite3_finalize(stmt_drives);

    if (drives_in_db.empty()) {
        std::wcout << L"Veritabanında gösterilecek sürücü bulunamadı. Lütfen önce tarama yapın." << std::endl;
        return;
    }

    for (const auto& drive_prefix_with_slash : drives_in_db) { // "C:\" gibi
        std::wstring drive_prefix = drive_prefix_with_slash;
        if (!drive_prefix.empty() && drive_prefix.back() == L'\\') {
             drive_prefix.pop_back(); // "C:"
        }

        std::wcout << drive_prefix << L":\\" << std::endl;
        // Kök dizin genellikle MFT referans numarası 5'tir.
        print_tree_recursive(db_manager, 5, drive_prefix, L"", true);
        std::wcout << std::endl;
    }
}