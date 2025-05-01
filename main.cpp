//============================================================================
// File Indexer - MFT Parser ve Search Tool
// Author: KOMPUTEK
// Date: 2025-05-01
// Platform: Windows (Requires Administrator Privileges)
// Dependencies: SQLite3 Library
//
// Description:
// Bu program, NTFS dosya sistemlerindeki Master File Table'ı ($MFT)
// doğrudan okuyarak dosya ve dizin bilgilerini ayrıştırır. Bu bilgileri
// bir SQLite veritabanında saklar ve hızlı arama (Full-Text Search ile),
// ağaç yapısında görüntüleme, yinelenen dosya bulma (isim+boyut) gibi
// işlevler sunar. "Everything" ve "Virtual Volume View" gibi araçlardan
// ilham alınmıştır.
//
// ÖNEMLİ: Bu programın MFT'ye erişebilmesi için YÖNETİCİ HAKLARIYLA
// çalıştırılması gerekmektedir!
//
// Derleme (örnek g++):
// g++ this_file.cpp -o FileIndexer.exe -lsqlite3 -static
//============================================================================
#include "utils.h"
#include "database_manager.h"
#include "file_scanner.h"
#include "search_engine.h"
#include "tree_view.h"
#include "duplicate_finder.h"
#include "index_manager.h"

int main() {
    // Konsolun Unicode (UTF-16) karakterleri doğru işlemesi için ayarla
    _wsetlocale(LC_ALL, L"Turkish"); // Türkçe karakterler için locale ayarı
    SetConsoleOutputCP(1254); // Türkçe karakter kod sayfası
    SetConsoleCP(1254);

    std::wcout << L"===== Gelişmiş Dosya İndeksleyici ve Arama Aracı =====" << std::endl;
    std::wcout << L"          (NTFS MFT Okuma ve SQLite FTS5 Tabanlı)" << std::endl;
    std::wcout << L"UYARI: Bu programın düzgün çalışması için YÖNETİCİ olarak çalıştırılması gerekir!" << std::endl;

    // Veritabanını aç/başlat
    if (!init_db()) {
        std::wcerr << L"Kritik Hata: Veritabanı başlatılamadı. Program sonlandırılıyor." << std::endl;
        return 1;
    }

    int choice = -1;
    while (choice != 0) {
        display_main_menu();
        std::cin >> choice;

        // Giriş tamponunu temizle (özellikle getline öncesi)
        if (std::cin.fail()) {
            std::cin.clear(); // Hata bayraklarını temizle
            std::cin.ignore(10000, '\n'); // Tamponu temizle
            choice = -1; // Geçersiz giriş
        } else {
            std::cin.ignore(10000, '\n'); // Başarılı okumadan sonra da yeni satırı atla
        }

        switch (choice) {
            case 1:
                scan_all_drives();
                break;
            case 2:
                search_files();
                break;
            case 3:
                display_tree_view();
                break;
            case 4:
                find_duplicates();
                break;
            case 5:
                {
                    std::string backup_file;
                    std::wcout << L"Yedek dosya adını girin (varsayılan: " << DEFAULT_BACKUP_FILENAME << L"): ";
                    std::getline(std::cin >> std::ws, backup_file);
                    if (backup_file.empty()) backup_file = DEFAULT_BACKUP_FILENAME;
                    save_index(backup_file);
                }
                break;
            case 6:
                {
                    std::string backup_file;
                    std::wcout << L"Geri yüklenecek dosya adını girin (varsayılan: " << DEFAULT_BACKUP_FILENAME << L"): ";
                    std::getline(std::cin >> std::ws, backup_file);
                    if (backup_file.empty()) backup_file = DEFAULT_BACKUP_FILENAME;
                    load_index(backup_file);
                }
                break;
            case 7:
                list_files_sorted();
                break;
            case 0:
                std::wcout << L"Programdan çıkılıyor..." << std::endl;
                break;
            default:
                std::wcout << L"Geçersiz seçim. Lütfen tekrar deneyin." << std::endl;
                break;
        }
    }

    // Veritabanını kapat
    close_db();

    std::wcout << L"Hoşçakalın!" << std::endl;

    return 0;
}