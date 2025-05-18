#pragma once

#include "common.h" // FileData, FileNameNamespace, MFT_RECORD_SIZE için
#include "database_manager.h" // DatabaseManager sınıfı için
#include <windows.h>
#include <vector>
#include <string>
#include <map>
#include <atomic> // std::atomic için

// Okuma tampon boyutu (Performans için ayarlanabilir)
constexpr DWORD READ_BUFFER_SIZE = 64 * 1024; // 64 KB


// NTFS Volume bilgisi
struct NTFS_VOLUME_DATA_BUFFER_STRUCT { // İsim çakışmasını önlemek için _STRUCT eklendi (windows.h ile olabilir)
    LARGE_INTEGER MftStartLcn;
    LARGE_INTEGER MftValidDataLength;
    ULONG BytesPerCluster;
    ULONG BytesPerSector;
    UCHAR ClustersPerFileRecordSegment;
};

// MFT Öznitelik Türü Kodları
enum class AttributeType : DWORD {
    STANDARD_INFORMATION = 0x10,
    ATTRIBUTE_LIST = 0x20,
    FILE_NAME = 0x30,
    OBJECT_ID = 0x40, // Vista and later
    SECURITY_DESCRIPTOR = 0x50,
    VOLUME_NAME = 0x60,
    VOLUME_INFORMATION = 0x70,
    DATA = 0x80,
    INDEX_ROOT = 0x90,
    INDEX_ALLOCATION = 0xA0,
    BITMAP = 0xB0,
    REPARSE_POINT = 0xC0, // Also known as SYMBOLIC_LINK or MOUNT_POINT
    EA_INFORMATION = 0xD0,
    EA = 0xE0, // Extended Attributes
    LOGGED_UTILITY_STREAM = 0x100, // $LOGGED_UTILITY_STREAM (used by EFS)
    END_OF_ATTRIBUTES = 0xFFFFFFFF
};

// MFT Kayıt Başlığı
#pragma pack(push, 1)
struct MFT_RECORD_HEADER {
    CHAR Signature[4];
    USHORT FixupOffset;
    USHORT FixupCount;
    ULONGLONG LogFileSequenceNumber;
    USHORT SequenceNumber;
    USHORT LinkCount;
    USHORT FirstAttributeOffset;
    USHORT Flags; // 0x0001: In use, 0x0002: Directory
    ULONG UsedSize;
    ULONG AllocatedSize;
    ULONGLONG BaseRecordReference; // 0 if base record
    USHORT NextAttributeId;
    //USHORT Padding; // Align to 4-byte boundary (Win2K)
    //ULONG MFTRecordNumber; // This is specific to WinXP and later, not part of standard header always
};
#pragma pack(pop)

bool parse_mft_record(const BYTE* buffer, uint64_t mft_ref, FileData& file_data, const std::wstring& drive_letter_prefix);
void reconstruct_paths(std::map<uint64_t, FileData>& ref_to_data, const std::wstring& drive_root_path);
void scan_mft_for_drive(const std::wstring& drive_letter, DatabaseManager& db_manager);
std::vector<std::wstring> get_available_ntfs_drives();
void scan_all_drives(DatabaseManager& db_manager);

// Global sayaçlar file_scanner.cpp içinde static std::atomic olarak tanımlanacak.
// extern std::atomic<long long> g_total_files_found_atomic;
// extern std::atomic<long long> g_total_dirs_found_atomic;