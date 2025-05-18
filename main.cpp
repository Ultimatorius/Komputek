//============================================================================
// File Indexer - MFT Parser ve Search Tool
// Author: KOMPUTEK (Düzenlenmiş Versiyon)
// Date: 2025-05-14
// Platform: Windows (Requires Administrator Privileges)
// Dependencies: SQLite3 Library
//============================================================================
#include "common.h" // Sabitler için
#include "utils.h"
#include "database_manager.h"
#include "file_scanner.h"
#include "search_engine.h"
#include "tree_view.h"
#include "duplicate_finder.h"
#include "index_manager.h"

#include <iostream> // std::wcout, std::cin
#include <string>   // std::string
#include <vector>   // std::vector
#include <algorithm> // std::sort
#include <iomanip>  // std::setw
#include <clocale>  // _wsetlocale

void display_main_menu() {
    std::wcout << L"\n===== Ana Menü =====\n"
               << L"1. Tüm Sürücüleri Tara (MFT Oku ve İndeksle)\n"
               << L"2. Dosya Ara (FTS5)\n"
               << L"3. Ağaç Görünümünü Göster\n"
               << L"4. Yinelenen Dosyaları Bul (İsim + Boyut)\n"
               << L"5. İndeksi Kaydet (Yedekle)\n"
               << L"6. İndeksi Geri Yükle\n"
               << L"7. Dosyaları Listele (Sıralı)\n"
               << L"0. Çıkış\n"
               << L"Lütfen bir seçim yapın: ";
}

void list_files_sorted(DatabaseManager& db_manager) {
    if (!db_manager.is_open()) {
        std::wcerr << L"Hata: Dosyaları listelemek için veritabanı açık değil." << std::endl;
        return;
    }

    std::wcout << L"\nDosyaları Nasıl Sıralamak İstersiniz?\n"
               << L"1. Ada Göre (A-Z)\n"
               << L"2. Boyuta Göre (Büyükten Küçüğe)\n"
               << L"3. Değiştirilme Tarihine Göre (Yeniden Eskiye)\n"
               << L"Seçiminiz: ";
    int sort_choice;
    std::cin >> sort_choice;
    std::cin.ignore(10000, '\n'); // Tamponu temizle

    std::string order_by_clause;
    switch (sort_choice) {
        case 1: order_by_clause = "ORDER BY name COLLATE NOCASE ASC, path COLLATE NOCASE ASC"; break;
        case 2: order_by_clause = "ORDER BY size DESC, name COLLATE NOCASE ASC"; break;
        case 3: order_by_clause = "ORDER BY modification_time DESC, name COLLATE NOCASE ASC"; break;
        default: std::wcout << L"Geçersiz sıralama seçimi." << std::endl; return;
    }

    const char* query_template = "SELECT path, name, size, modification_time, is_directory FROM files WHERE is_deleted = 0 ";
    std::string full_query = std::string(query_template) + order_by_clause + " LIMIT 100;"; // İlk 100 sonucu göster

    sqlite3_stmt* stmt = nullptr;
    std::unique_lock<std::mutex> lock(const_cast<std::mutex&>(db_manager.*(&DatabaseManager::db_mutex_)));

    if (sqlite3_prepare_v2(db_manager.get_db_handle(), full_query.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
        std::wcerr << L"Sıralı listeleme sorgusu hazırlanamadı: " << sqlite3_errmsg16(db_manager.get_db_handle()) << std::endl;
        return;
    }
    lock.unlock();

    std::wcout << L"\n--------------------------------------------------------------------------------\n";
    std::wcout << L"Boyut      | Son Değişiklik    | Tip | Yol\n";
    std::wcout << L"--------------------------------------------------------------------------------\n";
    
    int count = 0;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        count++;
        std::wstring path_w = widen(reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0)));
        std::wstring name_w = widen(reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1)));
        uint64_t size_val = sqlite3_column_int64(stmt, 2);
        uint64_t mod_time_val = sqlite3_column_int64(stmt, 3);
        bool is_dir = sqlite3_column_int(stmt, 4);

        std::wcout << std::setw(10) << std::left << format_filesize(size_val) << L" | "
                   << std::setw(18) << std::left << filetime_to_wstring(mod_time_val) << L" | "
                   << std::setw(3) << std::left << (is_dir ? L"DZN" : L"DSY") << L" | "
                   << path_w << (path_w.empty() || path_w.back() == L'\\' ? L"" : L"\\") << name_w
                   << std::endl;
    }
    sqlite3_finalize(stmt);

    if (count == 0) {
        std::wcout << L"Listelenecek dosya bulunamadı." << std::endl;
    } else {
         std::wcout << L"--------------------------------------------------------------------------------\n";
         if (count >=100) std::wcout << L"(İlk 100 sonuç gösteriliyor)\n";
    }
}


int main() {
    _wsetlocale(LC_ALL, L"tr_TR.UTF-8"); // Veya sadece "Turkish" veya sistem locale'i
    SetConsoleOutputCP(CP_UTF8); // UTF-8 için 65001, Türkçe için 1254
    SetConsoleCP(CP_UTF8);       // Windows'ta UTF-8 konsol desteği için bu ayarlar önemli.
                                 // `chcp 65001` komutu da terminalde gerekebilir.
                                 // Eğer Türkçe karakterler sorun çıkarırsa 1254'e dönülebilir.

    std::wcout << L"===== Gelişmiş Dosya İndeksleyici ve Arama Aracı =====" << std::endl;
    std::wcout << L"          (NTFS MFT Okuma ve SQLite FTS5 Tabanlı)" << std::endl;
    std::wcout << L"UYARI: Bu programın düzgün çalışması için YÖNETİCİ olarak çalıştırılması gerekir!" << std::endl;

    DatabaseManager db_manager(DB_FILENAME); // DB_FILENAME common.h'dan geliyor
    if (!db_manager.open_db()) {
        std::wcerr << L"Kritik Hata: Veritabanı bağlantısı kurulamadı. Program sonlandırılıyor." << std::endl;
        return 1;
    }
    if (!db_manager.init_schema()) {
        std::wcerr << L"Kritik Hata: Veritabanı şeması hazırlanamadı. Program sonlandırılıyor." << std::endl;
        // db_manager.close_db(); // Destructor halleder
        return 1;
    }

    int choice = -1;
    while (choice != 0) {
        display_main_menu();
        
        // Giriş işlemini daha güvenli hale getirelim
        std::wstring input_line;
        std::getline(std::wcin >> std::ws, input_line); // Önce satırı oku
        try {
            choice = std::stoi(input_line); // Sonra sayıya çevir
        } catch (const std::invalid_argument& ia) {
            choice = -1; // Geçersiz giriş
        } catch (const std::out_of_range& oor) {
            choice = -1; // Sayı çok büyük/küçük
        }


        switch (choice) {
            case 1:
                scan_all_drives(db_manager);
                break;
            case 2:
                search_files(db_manager);
                break;
            case 3:
                display_tree_view(db_manager);
                break;
            case 4:
                find_duplicates(db_manager);
                break;
            case 5: {
                std::string backup_file_str = DEFAULT_BACKUP_FILENAME; // common.h'dan
                std::wcout << L"Yedek dosya adını girin (varsayılan: " << widen(backup_file_str) << L"): ";
                std::wstring backup_input_w;
                std::getline(std::wcin >> std::ws, backup_input_w);
                if (!backup_input_w.empty()) backup_file_str = narrow(backup_input_w);
                save_index(db_manager, backup_file_str);
                break;
            }
            case 6: {
                std::string backup_file_str = DEFAULT_BACKUP_FILENAME;
                std::wcout << L"Geri yüklenecek dosya adını girin (varsayılan: " << widen(backup_file_str) << L"): ";
                 std::wstring backup_input_w;
                std::getline(std::wcin >> std::ws, backup_input_w);
                if (!backup_input_w.empty()) backup_file_str = narrow(backup_input_w);
                load_index(db_manager, backup_file_str);
                break;
            }
            case 7:
                list_files_sorted(db_manager);
                break;
            case 0:
                std::wcout << L"Programdan çıkılıyor..." << std::endl;
                break;
            default:
                std::wcout << L"Geçersiz seçim. Lütfen 0-7 arasında bir sayı girin." << std::endl;
                break;
        }
        if (choice != 0) {
             std::wcout << L"\nDevam etmek için Enter tuşuna basın...";
             std::wcin.ignore(10000,'\n'); // Eğer önceki girişten kalan varsa temizle
             // std::getline(std::wcin, input_line); // Kullanıcının Enter'a basmasını bekle
        }
    }

    // db_manager.close_db(); // DatabaseManager destructor'ı bunu otomatik yapacak

    std::wcout << L"Hoşçakalın!" << std::endl;
    return 0;
}