#pragma once

#include <string>
#include <vector>
#include <cstdint> // uint64_t vb. için
#include <windows.h> // BYTE için

// Veritabanı dosya adı
const char* const DB_FILENAME = "file_index.db";
// Varsayılan indeks kaydetme/yükleme dosyası
const char* const DEFAULT_BACKUP_FILENAME = "file_index_backup.db"; // Uzantı .db olsun

// MFT Kayıt Boyutu (Genellikle 1024 byte)
constexpr DWORD MFT_RECORD_SIZE = 1024;
// Okuma tampon boyutu (Performans için ayarlanabilir) // Bu utils.h veya file_scanner.h içinde kalabilir, MFT okumaya özel.
// constexpr DWORD READ_BUFFER_SIZE = 64 * 1024; // 64 KB

// Dosya Adı Ad Alanları (FileName Namespace)
enum class FileNameNamespace : BYTE {
    POSIX = 0,      // Büyük/küçük harf duyarlı
    WIN32 = 1,      // Büyük/küçük harf duyarsız (genellikle kullanılır)
    DOS = 2,        // 8.3 formatı
    WIN32_DOS = 3   // Hem Win32 hem de DOS adı mevcut
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
        // Veritabanı trigger'ı path ve name'i birleştiriyor, bu yüzden burada sadece bir örnek
        // Aslında bu alan doğrudan FTS tablosuna yazılırken oluşturulur.
        // Eğer path '\' ile bitmiyorsa ve name boş değilse, araya '\' ekle.
        if (!path.empty() && path.back() != L'\\' && !name.empty()) {
            return path + L"\\" + name;
        }
        return path + name;
    }
};