#pragma once
#include <windows.h>
#include <vector>
#include <string>
#include <map>
#include <sqlite3.h>

// MFT Kayıt Boyutu (Genellikle 1024 byte)
constexpr DWORD MFT_RECORD_SIZE = 1024;
// Okuma tampon boyutu (Performans için ayarlanabilir)
constexpr DWORD READ_BUFFER_SIZE = 64 * 1024; // 64 KB
// Veritabanı dosya adı
const char* const DB_FILENAME = "file_index.db";
// Varsayılan indeks kaydetme/yükleme dosyası
const char* const DEFAULT_BACKUP_FILENAME = "file_index.db.backup";

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

// NTFS Volume bilgisi
struct NTFS_VOLUME_DATA_BUFFER {
    LARGE_INTEGER MftStartLcn; // $MFT'nin başlangıç LCN'si
    LARGE_INTEGER MftValidDataLength; // $MFT'nin geçerli veri uzunluğu
    ULONG BytesPerCluster; // Cluster başına byte sayısı
    ULONG BytesPerSector; // Sector başına byte sayısı
    UCHAR ClustersPerFileRecordSegment; // Her FRS için cluster sayısı
};

// MFT Öznitelik Türü Kodları (Attribute Type Codes)
enum class AttributeType : DWORD {
    STANDARD_INFORMATION = 0x10,
    ATTRIBUTE_LIST = 0x20,
    FILE_NAME = 0x30,
    OBJECT_ID = 0x40,
    SECURITY_DESCRIPTOR = 0x50,
    VOLUME_NAME = 0x60,
    VOLUME_INFORMATION = 0x70,
    DATA = 0x80,
    INDEX_ROOT = 0x90,
    INDEX_ALLOCATION = 0xA0,
    BITMAP = 0xB0,
    REPARSE_POINT = 0xC0,
    EA_INFORMATION = 0xD0,
    EA = 0xE0,
    LOGGED_UTILITY_STREAM = 0x100,
    END_OF_ATTRIBUTES = 0xFFFFFFFF // Liste sonu işareti
};

// MFT Kayıt Başlığı (File Record Segment Header)
#pragma pack(push, 1)
struct MFT_RECORD_HEADER {
    CHAR Signature[4];        // "FILE" veya "BAAD"
    USHORT FixupOffset;       // Fixup array'in ofseti
    USHORT FixupCount;         // Fixup array'deki giriş sayısı (başlık dahil)
    ULONGLONG LogFileSequenceNumber; // $LogFile içindeki LSN
    USHORT SequenceNumber;    // Bu kaydın kaç kez yeniden kullanıldığı
    USHORT LinkCount;         // Dosyaya işaret eden dizin sayısı
    USHORT FirstAttributeOffset; // İlk öznitelik başlığının ofseti
    USHORT Flags;             // 1: Kayıt kullanımda, 2: Bu bir dizin
    ULONG UsedSize;           // Kaydın kullanılan boyutu
    ULONG AllocatedSize;      // Kaydın ayrılmış boyutu (genellikle 1024)
    ULONGLONG BaseRecordReference; // Temel kayıt referansı (eğer bu bir uzantıysa)
    USHORT NextAttributeId;   // Bir sonraki atanacak öznitelik ID'si
};
#pragma pack(pop)

// MFT Kaydını ayrıştırır ve FileData nesnesini doldurur
// buffer: MFT_RECORD_SIZE boyutunda, fixup uygulanmış MFT kaydı
// mft_ref: Bu kaydın MFT referans numarası
// file_data: Doldurulacak FileData nesnesi
// drive_letter: Sürücü harfi (örn: C:)
// returns: Başarılı olursa true
bool parse_mft_record(const BYTE* buffer, uint64_t mft_ref, FileData& file_data, const std::wstring& drive_letter);

// Tam dosya yollarını oluşturur (ebeveyn referanslarını kullanarak)
// Bu fonksiyon, `all_files` map'indeki tüm girdilerin path alanını doldurur.
// `ref_to_data`: MFT Referans Numarası -> FileData map'i
void reconstruct_paths(std::map<uint64_t, FileData>& ref_to_data);

// Belirtilen sürücünün MFT'sini okur ve ayrıştırır
void scan_mft_for_drive(const std::wstring& drive_letter);

// Tüm uygun sürücüleri bulur
std::vector<std::wstring> get_available_ntfs_drives();

// Tüm uygun sürücüleri tara
void scan_all_drives();

// Dosya yollarını oluşturur (ebeveyn referanslarını kullanarak)
// Bu fonksiyon, `all_files` map'indeki tüm girdilerin path alanını doldurur.
// `ref_to_data`: MFT Referans Numarası -> FileData map'i
void reconstruct_paths(std::map<uint64_t, FileData>& ref_to_data);