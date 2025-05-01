#include "search_engine.h"
#include "database_manager.h"
#include "utils.h"

#include <sqlite3.h>
#include <windows.h>
#include <winioctl.h>
#include <iostream>
#include <sstream>
#include <iomanip>
#include <map>
#include <set>
#include <algorithm>
#include <thread>
#include <mutex>
#include <chrono>
#include <cstdint>
#include <clocale>
#include <filesystem>

// Dosya arama fonksiyonu
void search_files() {
    if (!g_db) {
        std::wcerr << L"Hata: Arama için veritabanı açık değil." << std::endl;
        return;
    }

    std::wstring search_term;
    std::wcout << L"\nAranacak metni girin (birden fazla kelime girebilirsiniz): ";
    std::getline(std::wcin >> std::ws, search_term); // Satır oku, baştaki boşlukları atla

    if (search_term.empty()) {
        std::wcout << L"Arama terimi boş." << std::endl;
        return;
    }

    // Basit FTS sorgusu oluşturma: Girilen her kelimeyi AND ile bağla gibi.
    // SQLite FTS5, varsayılan olarak kelimeleri AND ile birleştirir.
    // Örnek: "belge resim" -> fts_files MATCH 'belge resim'
    // Daha gelişmiş: "belge NEAR resim" veya "belge OR resim" gibi operatörler eklenebilir.
    // Şimdilik basit eşleşme yapalım.
    std::string fts_query = "SELECT f.path, f.name, f.size, f.modification_time "
                           "FROM files f JOIN fts_files ft ON f.id = ft.rowid "
                           "WHERE ft.fts_files MATCH ? "
                           "ORDER BY rank"; // FTS rank'ine göre sırala (en alakalılar başta)

    std::wcout << L"'" << search_term << L"' için arama yapılıyor..." << std::endl;
    auto start_time