#include "file_scanner.h"
#include "utils.h"
#include "database_manager.h"

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
#include <sqlite3.h>

// RAII için Handle sarmalayıcı
struct SafeHandle {
    HANDLE handle = INVALID_HANDLE_VALUE;
    ~SafeHandle() {
        if (handle != INVALID_HANDLE_VALUE && handle != nullptr) {
            CloseHandle(handle);
        }
    }
    operator HANDLE() const { return handle; }
    HANDLE* operator&() { return &handle; } // CreateFileW gibi fonksiyonlarla kullanım için
};

// NTFS Volume bilgisi almak için yapı
struct NTFS_VOLUME_DATA_BUFFER {
    LARGE_INTEGER MftStartLcn; // $MFT'nin başlangıç LCN'si
    LARGE_INTEGER MftValidDataLength; // $MFT'nin geçerli veri uzunluğu
    ULONG BytesPerCluster; // Cluster başına byte sayısı
    ULONG BytesPerSector; // Sector başına byte sayısı
    UCHAR ClustersPerFileRecordSegment; // Her FRS için cluster sayısı
};

// MFT Öznitelik Başlığı Ortak Kısım
#pragma pack(push, 1) // Yapı hizalamasını 1 byte yap
struct ATTRIBUTE_HEADER_COMMON {
    AttributeType Type;       // Öznitelik türü
    ULONG Length;             // Başlık + veri uzunluğu
    UCHAR NonResidentFlag;    // 0: Resident, 1: Non-Resident
    UCHAR NameLength;         // Öznitelik adı uzunluğu (karakter sayısı)
    USHORT NameOffset;        // Öznitelik adının başlangıç ofseti
    USHORT Flags;             // (Sıkıştırılmış, Şifreli, Seyrek)
    USHORT AttributeId;       // Bu FRS içindeki benzersiz ID
};
#pragma pack(pop)

// Resident Öznitelik Başlığı
struct ATTRIBUTE_HEADER_RESIDENT {
    ATTRIBUTE_HEADER_COMMON Common;
    ULONG ValueLength;        // Veri (value) uzunluğu
    USHORT ValueOffset;       // Verinin başlangıç ofseti
    UCHAR IndexedFlag;        // (Genellikle $INDEX_ROOT için)
    UCHAR Padding;            // Hizalama için
    // Veri (Value) bu başlıktan ValueOffset kadar sonra başlar
};

// Non-Resident Öznitelik Başlığı
#pragma pack(push, 1)
struct ATTRIBUTE_HEADER_NON_RESIDENT {
    ATTRIBUTE_HEADER_COMMON Common;
    ULONGLONG StartingVCN;    // Veri akışındaki ilk Virtual Cluster Number (VCN)
    ULONGLONG LastVCN;        // Veri akışındaki son VCN
    USHORT DataRunsOffset;    // Data Run listesinin başlangıç ofseti
    USHORT CompressionUnit;   // Sıkıştırma birimi boyutu (2^n byte)
    ULONG Padding;            // Hizalama
    ULONGLONG AllocatedSize;  // Veri akışı için ayrılmış disk alanı (byte)
    ULONGLONG RealSize;       // Veri akışının gerçek boyutu (byte)
    ULONGLONG InitializedSize;// Veri akışının başlatılmış kısmı (seyrek dosyalar için)
    // Data Runs listesi bu başlıktan DataRunsOffset kadar sonra başlar
};
#pragma pack(pop)

// $STANDARD_INFORMATION özniteliği verisi
#pragma pack(push, 1)
struct STANDARD_INFORMATION {
    ULONGLONG CreationTime;       // Dosya oluşturma zamanı (FILETIME)
    ULONGLONG AlteredTime;        // Dosya son değişiklik zamanı (FILETIME)
    ULONGLONG MFTChangedTime;     // MFT kaydının son değişiklik zamanı (FILETIME)
    ULONGLONG ReadTime;           // Dosya son okunma zamanı (FILETIME)
    ULONG DosFilePermissions;     // DOS öznitelikleri (eski)
    ULONG MaxNumberOfVersions;    // (Kullanılmıyor)
    ULONG VersionNumber;          // (Kullanılmıyor)
    ULONG ClassId;                // (Kullanılmıyor)
    ULONG OwnerId;                // Sahibin Security ID'si (Vista+)
    ULONG SecurityId;             // Güvenlik tanımlayıcısının ID'si ($SECURE içindeki)
    ULONGLONG QuotaCharged;       // Kota kullanımı
    USN Usn;                      // Son USN (Update Sequence Number)
};
#pragma pack(pop)

// $FILE_NAME özniteliği verisi
#pragma pack(push, 1)
struct FILE_NAME_ATTRIBUTE {
    ULONGLONG ParentDirectoryReference; // Ebeveyn dizinin MFT referansı (sequence no dahil)
    ULONGLONG CreationTime;           // Dosya oluşturma zamanı
    ULONGLONG AlteredTime;            // Dosya son değişiklik zamanı
    ULONGLONG MFTChangedTime;         // MFT kaydının son değişiklik zamanı
    ULONGLONG ReadTime;               // Dosya son okunma zamanı
    ULONGLONG AllocatedSize;          // Dosya için ayrılan boyut
    ULONGLONG RealSize;               // Dosyanın gerçek boyutu
    ULONG FileFlags;                  // Dosya öznitelikleri (Read-only, Hidden, System vb.)
    ULONG ReparseTagOrEaSize;         // EA veya Reparse Point bilgisi
    UCHAR FileNameLength;             // Dosya adı uzunluğu (karakter sayısı)
    FileNameNamespace NameSpace;      // Ad alanı (Win32, DOS, POSIX)
    // WCHAR FileName[FileNameLength]; // Değişken uzunluklu dosya adı (Unicode)
};
#pragma pack(pop)

// NTFS Volume bilgisi almak için fonksiyon
bool get_ntfs_volume_data(HANDLE hDevice, NTFS_VOLUME_DATA_BUFFER& volume_data) {
    DWORD bytes_returned = 0;
    if (!DeviceIoControl(hDevice, FSCTL_GET_NTFS_VOLUME_DATA, nullptr, 0, &volume_data, sizeof(volume_data), &bytes_returned, nullptr)) {
        std::wcerr << L"Hata: NTFS Volume bilgileri alınamadı. Hata Kodu: " << GetLastError() << std::endl;
        return false;
    }
    return true;
}

// Verilen LCN ve cluster sayısına göre diskten veri okur
// Dikkat: Bu fonksiyon basitlik için sektör boyutunu cluster boyutuyla aynı varsayar.
// Gerçek disk okuması daha karmaşık olabilir.
bool read_clusters(HANDLE hDevice, uint64_t start_lcn, uint64_t num_clusters, DWORD bytes_per_cluster, std::vector<BYTE>& buffer) {
    LARGE_INTEGER offset;
    offset.QuadPart = start_lcn * bytes_per_cluster;
    DWORD bytes_to_read = static_cast<DWORD>(num_clusters * bytes_per_cluster);
    buffer.resize(bytes_to_read);

    OVERLAPPED overlapped = {0};
    overlapped.Offset = offset.LowPart;
    overlapped.OffsetHigh = offset.HighPart;

    DWORD bytes_read = 0;
    if (!ReadFile(hDevice, buffer.data(), bytes_to_read, &bytes_read, &overlapped)) {
        DWORD error = GetLastError();
        if (error != ERROR_IO_PENDING) {
             std::wcerr << L"Hata: Cluster okunamadı (LCN: " << start_lcn << L", Sayı: " << num_clusters << L"). Hata Kodu: " << error << std::endl;
            return false;
        }
        // IO Pending ise, işlemin bitmesini bekle
        if (!GetOverlappedResult(hDevice, &overlapped, &bytes_read, TRUE)) {
             std::wcerr << L"Hata: Bekleyen cluster okuma işlemi başarısız (LCN: " << start_lcn << L"). Hata Kodu: " << GetLastError() << std::endl;
            return false;
        }
    }

    if (bytes_read != bytes_to_read) {
        std::wcerr << L"Uyarı: Cluster okurken beklenenden az byte okundu (LCN: " << start_lcn << L"). İstenen: " << bytes_to_read << L", Okunan: " << bytes_read << std::endl;
        buffer.resize(bytes_read); // Okunan kadarını al
        // return false; // Tam okuma yapılamadıysa hata verebiliriz.
    }
    return true;
}

// $MFT dosyasının diskteki konumunu ($DATA özniteliğindeki data run'lardan) bulur
std::vector<std::pair<uint64_t, uint64_t>> find_mft_data_runs(HANDLE hDevice, const NTFS_VOLUME_DATA_BUFFER& vol_data) {
    std::vector<std::pair<uint64_t, uint64_t>> mft_runs; // {start_lcn, num_clusters}
    uint64_t total_mft_clusters = 0;

    // MFT'nin kendisi genellikle MFT Kayıt 0'dır. Bu kaydı okuyalım.
    std::vector<BYTE> mft_record_0_buffer(MFT_RECORD_SIZE);
    LARGE_INTEGER mft_start_offset;
    mft_start_offset.QuadPart = vol_data.MftStartLcn.QuadPart * vol_data.BytesPerCluster;

    OVERLAPPED overlapped = {0};
    overlapped.Offset = mft_start_offset.LowPart;
    overlapped.OffsetHigh = mft_start_offset.HighPart;

    DWORD bytes_read = 0;
    if (!ReadFile(hDevice, mft_record_0_buffer.data(), MFT_RECORD_SIZE, &bytes_read, &overlapped)) {
         DWORD error = GetLastError();
        if (error != ERROR_IO_PENDING) {
            std::wcerr << L"Hata: MFT Kayıt 0 okunamadı. Hata Kodu: " << error << std::endl;
            return mft_runs;
        }
         if (!GetOverlappedResult(hDevice, &overlapped, &bytes_read, TRUE)) {
             std::wcerr << L"Hata: Bekleyen MFT Kayıt 0 okuma işlemi başarısız. Hata Kodu: " << GetLastError() << std::endl;
             return mft_runs;
         }
    }
     if (bytes_read < MFT_RECORD_SIZE) {
         std::wcerr << L"Hata: MFT Kayıt 0 okunurken eksik byte okundu." << std::endl;
         return mft_runs;
     }

    // MFT Kayıt Başlığını al
    MFT_RECORD_HEADER* header = reinterpret_cast<MFT_RECORD_HEADER*>(mft_record_0_buffer.data());
    if (strncmp(header->Signature, "FILE", 4) != 0) {
        std::wcerr << L"Hata: MFT Kayıt 0'ın imzası geçersiz ('FILE' değil)." << std::endl;
        return mft_runs;
    }

    // Fixup uygula
    if (!apply_fixup(mft_record_0_buffer.data(), header)) {
         std::wcerr << L"Hata: MFT Kayıt 0 fixup uygulanamadı." << std::endl;
        return mft_runs;
    }

    // Öznitelikleri tara ve $DATA'yı bul
    BYTE* p_attr = mft_record_0_buffer.data() + header->FirstAttributeOffset;
    BYTE* record_end = mft_record_0_buffer.data() + header->UsedSize;

    while (p_attr < record_end) {
        ATTRIBUTE_HEADER_COMMON* attr_header = reinterpret_cast<ATTRIBUTE_HEADER_COMMON*>(p_attr);

        if (attr_header->Type == AttributeType::END_OF_ATTRIBUTES) {
            break; // Öznitelik listesinin sonu
        }
         if (attr_header->Length == 0) {
            std::wcerr << L"Hata: MFT Kayıt 0'da sıfır uzunluklu öznitelik bulundu." << std::endl;
            break; // Hatalı durum
        }

        if (attr_header->Type == AttributeType::DATA) {
            // $DATA özniteliği bulundu. Non-resident olmalı.
            if (attr_header->NonResidentFlag == 0) {
                 std::wcerr << L"Uyarı: MFT Kayıt 0'daki $DATA özniteliği beklenmedik şekilde resident." << std::endl;
                 // Resident $DATA'dan MFT'nin tamamını okumak zor. Şimdilik bu durumu atlıyoruz.
            } else {
                ATTRIBUTE_HEADER_NON_RESIDENT* nr_header = reinterpret_cast<ATTRIBUTE_HEADER_NON_RESIDENT*>(p_attr);
                 BYTE* run_list = p_attr + nr_header->DataRunsOffset;
                 ULONG run_list_length = nr_header->Common.Length - nr_header->DataRunsOffset;
                mft_runs = parse_data_runs(run_list, run_list_length, total_mft_clusters);
                std::wcout << L"Bilgi: $MFT için " << mft_runs.size() << L" adet data run bulundu. Toplam Cluster: " << total_mft_clusters << std::endl;
                 // MFT'nin boyutunu da kontrol edebiliriz: vol_data.MftValidDataLength
                 if (total_mft_clusters * vol_data.BytesPerCluster < vol_data.MftValidDataLength.QuadPart) {
                      std::wcerr << L"Uyarı: Data run'lardan hesaplanan MFT boyutu, volume bilgisindeki MFT boyutundan küçük!" << std::endl;
                 }
            }
            break; // $DATA bulundu, döngüden çık
        }

        p_attr += attr_header->Length; // Sonraki özniteliğe geç
    }

     if (mft_runs.empty()) {
         std::wcerr << L"Hata: MFT Kayıt 0 içinde $DATA özniteliği veya data run'ları bulunamadı!" << std::endl;
     }

    return mft_runs;
}

// MFT Kaydını ayrıştırır ve FileData nesnesini doldurur
// buffer: MFT_RECORD_SIZE boyutunda, fixup uygulanmış MFT kaydı
// mft_ref: Bu kaydın MFT referans numarası
// file_data: Doldurulacak FileData nesnesi
// drive_letter: Sürücü harfi (örn: C:)
// returns: Başarılı olursa true
bool parse_mft_record(const BYTE* buffer, uint64_t mft_ref, FileData& file_data, const std::wstring& drive_letter) {
    const MFT_RECORD_HEADER* header = reinterpret_cast<const MFT_RECORD_HEADER*>(buffer);

    // Temel bilgileri ata
    file_data.mft_reference_number = mft_ref;
    file_data.drive = drive_letter + L"\\";
    file_data.is_deleted = !(header->Flags & 0x0001); // Kullanımda değilse silinmiş kabul edilir
    file_data.is_directory = (header->Flags & 0x0002);

    bool found_std_info = false;
    bool found_file_name = false;

    // Öznitelikleri tara
    const BYTE* p_attr = buffer + header->FirstAttributeOffset;
    const BYTE* record_end = buffer + header->UsedSize;

    while (p_attr < record_end) {
        const ATTRIBUTE_HEADER_COMMON* attr_header = reinterpret_cast<const ATTRIBUTE_HEADER_COMMON*>(p_attr);

        if (attr_header->Type == AttributeType::END_OF_ATTRIBUTES) {
            break; // Liste sonu
        }
        if (attr_header->Length == 0) break; // Hatalı durum

        // İlgilendiğimiz öznitelikleri işle
        switch (attr_header->Type) {
            case AttributeType::STANDARD_INFORMATION: {
                if (!attr_header->NonResidentFlag) { // Resident olmalı
                    const ATTRIBUTE_HEADER_RESIDENT* res_header = reinterpret_cast<const ATTRIBUTE_HEADER_RESIDENT*>(p_attr);
                    if (res_header->ValueOffset < attr_header->Length && // Güvenlik kontrolü
                        res_header->ValueLength == sizeof(STANDARD_INFORMATION))
                    {
                        const STANDARD_INFORMATION* std_info = reinterpret_cast<const STANDARD_INFORMATION*>(p_attr + res_header->ValueOffset);
                        file_data.creation_time = std_info->CreationTime;
                        file_data.modification_time = std_info->AlteredTime;
                        file_data.access_time = std_info->ReadTime;
                        file_data.mft_change_time = std_info->MFTChangedTime;
                        // file_data.file_attributes |= std_info->DosFilePermissions; // Eski DOS öznitelikleri, FILE_NAME içindekiler daha güvenilir
                        found_std_info = true;
                    }
                }
                break;
            }
            case AttributeType::FILE_NAME: {
                if (!attr_header->NonResidentFlag) { // Resident olmalı
                    const ATTRIBUTE_HEADER_RESIDENT* res_header = reinterpret_cast<const ATTRIBUTE_HEADER_RESIDENT*>(p_attr);
                    if (res_header->ValueOffset < attr_header->Length) // Güvenlik kontrolü
                    {
                        const FILE_NAME_ATTRIBUTE* fn_attr = reinterpret_cast<const FILE_NAME_ATTRIBUTE*>(p_attr + res_header->ValueOffset);

                        // Sadece Win32 veya Win32&DOS ad alanını tercih et (uzun dosya adı)
                        if (fn_attr->NameSpace == FileNameNamespace::WIN32 || fn_attr->NameSpace == FileNameNamespace::WIN32_DOS)
                        {
                             // Dosya adını al
                             int name_len_bytes = fn_attr->FileNameLength * sizeof(wchar_t);
                             if (res_header->ValueOffset + sizeof(FILE_NAME_ATTRIBUTE) + name_len_bytes <= res_header->Common.Length) // Güvenlik kontrolü
                             {
                                 const wchar_t* filename_ptr = reinterpret_cast<const wchar_t*>(reinterpret_cast<const BYTE*>(fn_attr) + sizeof(FILE_NAME_ATTRIBUTE));
                                 file_data.name.assign(filename_ptr, fn_attr->FileNameLength);

                                 // Uzantıyı ayıkla
                                 if (!file_data.is_directory) {
                                     size_t dot_pos = file_data.name.rfind(L'.');
                                     if (dot_pos != std::wstring::npos && dot_pos > 0) {
                                         file_data.extension = file_data.name.substr(dot_pos);
                                     } else {
                                         file_data.extension.clear();
                                     }
                                 } else {
                                     file_data.extension.clear(); // Dizinlerin uzantısı olmaz
                                 }

                                 // Ebeveyn referansı ve öznitelikler
                                 file_data.parent_mft_reference_number = get_mft_ref_number_only(fn_attr->ParentDirectoryReference);
                                 file_data.file_attributes = fn_attr->FileFlags; // Bu öznitelikler daha güvenilir

                                // Boyut bilgisi ($STANDARD_INFORMATION yoksa buradan al)
                                 if (!found_std_info) {
                                     file_data.creation_time = fn_attr->CreationTime;
                                     file_data.modification_time = fn_attr->AlteredTime;
                                     file_data.access_time = fn_attr->ReadTime;
                                     file_data.mft_change_time = fn_attr->MFTChangedTime;
                                 }
                                 found_file_name = true;
                             }
                        }
                        // Diğer ad alanları (DOS, POSIX) şimdilik atlanıyor.
                    }
                }
                break;
            }
             case AttributeType::DATA: {
                 // Dosya boyutunu buradan almak daha doğru
                 if (attr_header->NonResidentFlag == 0) { // Resident veri
                     const ATTRIBUTE_HEADER_RESIDENT* res_header = reinterpret_cast<const ATTRIBUTE_HEADER_RESIDENT*>(p_attr);
                     file_data.size = res_header->ValueLength;
                 } else { // Non-resident veri
                     const ATTRIBUTE_HEADER_NON_RESIDENT* nr_header = reinterpret_cast<const ATTRIBUTE_HEADER_NON_RESIDENT*>(p_attr);
                     file_data.size = nr_header->RealSize;
                 }
                 break;
            }
             case AttributeType::ATTRIBUTE_LIST: {
                 // Bu öznitelik, diğer özniteliklerin başka MFT kayıtlarına
                 // yayıldığını gösterir. İşlenmesi MFT okumayı çok daha
                 // karmaşık hale getirir. Bu örnekte işlenmemektedir.
                 // std::wcerr << L"Uyarı: MFT Ref " << mft_ref << L" için $ATTRIBUTE_LIST bulundu (işlenmedi)." << std::endl;
                 break;
             }
            // Diğer öznitelik türleri (VOLUME_NAME, INDEX_ROOT vb.) şimdilik işlenmiyor.
            default:
                break;
        }

        p_attr += attr_header->Length; // Sonraki özniteliğe geç
    } // while (öznitelik döngüsü)

    // Gerekli temel bilgiler bulunamadıysa başarısız say
    if (!found_file_name) {
        // $FILE_NAME zorunludur (silinmiş veya meta-dosya değilse).
        // Silinmiş veya özel MFT kayıtları ($MFT, $LogFile vs.) için normaldir.
        if (!file_data.is_deleted && mft_ref > 15) { // İlk 16 kayıt NTFS meta dosyalarıdır
           // std::wcerr << L"Uyarı: MFT Ref " << mft_ref << L" için $FILE_NAME özniteliği bulunamadı." << std::endl;
        }
        return false; // $FILE_NAME yoksa geçersiz kayıt kabul edelim (meta dosyalar hariç)
    }

    return true; // Başarıyla ayrıştırıldı
}

// Tam dosya yollarını oluşturur (ebeveyn referanslarını kullanarak)
// Bu fonksiyon, `all_files` map'indeki tüm girdilerin path alanını doldurur.
// `ref_to_data`: MFT Referans Numarası -> FileData map'i
void reconstruct_paths(std::map<uint64_t, FileData>& ref_to_data) {
    std::wcout << L"Dosya yolları oluşturuluyor..." << std::endl;
    auto start_time = std::chrono::high_resolution_clock::now();
    int paths_constructed = 0;

    // Kök dizini bul (genellikle MFT Ref 5)
    uint64_t root_ref = 5;
    if (ref_to_data.count(root_ref)) {
        ref_to_data[root_ref].path = L""; // Kök dizinin yolu boştur (sadece sürücü harfi vardır)
        paths_constructed++;
    } else {
        std::wcerr << L"Hata: Kök dizin (MFT Ref 5) bulunamadı! Yollar oluşturulamayacak." << std::endl;
        return;
    }

    // Her dosya için yolu bulmaya çalış
    // Basit bir reküristif yaklaşım yerine, her dosyanın ebeveyninin yolu
    // zaten hesaplanmış mı diye kontrol eden ve gerektiğinde hesaplayan
    // bir yaklaşım daha verimli olabilir.
    std::function<std::wstring(uint64_t)> get_path =
        [&](uint64_t current_ref) -> std::wstring
    {
        if (!ref_to_data.count(current_ref)) {
            return L"<unknown_parent>"; // Ebeveyn bulunamadı
        }

        FileData& current_data = ref_to_data[current_ref];

        // Yol zaten hesaplandıysa döndür
        if (!current_data.path.empty() || current_ref == root_ref) {
            return current_data.path;
        }

        // Ebeveynin yolunu reküristif olarak al
        std::wstring parent_path = get_path(current_data.parent_mft_reference_number);

        // Kendi yolunu oluştur (parent_path + \ + kendi_adı)
        // Kök dizinin hemen altındaysa sadece \isim ekle
        if (current_data.parent_mft_reference_number == root_ref) {
             current_data.path = L""; // Kök altı için başlangıçta \ yok
        } else {
             current_data.path = parent_path + L"\\" + ref_to_data[current_data.parent_mft_reference_number].name;
        }

        paths_constructed++;
        return current_data.path;
    };

    // Şimdi tüm dosyaların path alanını dolduralım
    for (auto& pair : ref_to_data) {
        if (pair.first == root_ref) {
            pair.second.path = pair.second.drive; // Kök dizin yolu sadece sürücü harfi
        } else {
            std::wstring full_path = get_path(pair.first);
            // Tam yolu oluştur (sürücü harfi + full_path)
            pair.second.path = pair.second.drive.substr(0, 2) + full_path;
            paths_constructed++;
        }
    }

    auto end_time = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);
    std::wcout << L"✅ " << paths_constructed << L" adet dosya yolu oluşturuldu (" << duration.count() << L" ms)." << std::endl;
}

// Belirtilen sürücünün MFT'sini okur ve ayrıştırır
void scan_mft_for_drive(const std::wstring& drive_letter) {
    std::wcout << L"\n🔍 " << drive_letter << L": Sürücüsü taranıyor (MFT Okuma)..." << std::endl;
    auto total_start_time = std::chrono::high_resolution_clock::now();

    std::wstring volume_path = L"\\\\.\\" + drive_letter + L":";
    SafeHandle hDevice;
    hDevice.handle = CreateFileW(
        volume_path.c_str(),
        GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr,
        OPEN_EXISTING,
        FILE_FLAG_NO_BUFFERING | FILE_FLAG_OVERLAPPED, // No Buffering ve Overlapped IO performans için önemli olabilir
        nullptr
    );

    if (hDevice.handle == INVALID_HANDLE_VALUE) {
        DWORD error = GetLastError();
        std::wcerr << L"Hata: Birim açılamadı (" << volume_path << L"). Hata Kodu: " << error << std::endl;
        if (error == ERROR_ACCESS_DENIED) {
             std::wcerr << L"Yönetici hakları gerekiyor!" << std::endl;
        }
        return;
    }
     std::wcout << L"Birim başarıyla açıldı: " << volume_path << std::endl;

    // NTFS Volume verilerini al
    NTFS_VOLUME_DATA_BUFFER vol_data = {0};
    if (!get_ntfs_volume_data(hDevice, vol_data)) {
        return; // Hata zaten yazdırıldı
    }
     std::wcout << L"NTFS Volume Bilgileri Alındı:" << std::endl;
     std::wcout << L"  Bytes Per Cluster: " << vol_data.BytesPerCluster << std::endl;
     std::wcout << L"  Bytes Per Sector: " << vol_data.BytesPerSector << std::endl;
     std::wcout << L"  MFT Start LCN: " << vol_data.MftStartLcn.QuadPart << std::endl;
     std::wcout << L"  MFT Valid Data Length: " << vol_data.MftValidDataLength.QuadPart << std::endl;
     std::wcout << L"  MFT Record Size: " << (1 << vol_data.ClustersPerFileRecordSegment) * vol_data.BytesPerCluster << std::endl; // Genellikle 1024

     // MFT Kayıt Boyutu standarda uymuyorsa uyarı ver
     if ((1 << vol_data.ClustersPerFileRecordSegment) * vol_data.BytesPerCluster != MFT_RECORD_SIZE) {
          std::wcerr << L"Uyarı: MFT Kayıt boyutu beklenenden farklı (" << MFT_RECORD_SIZE << L" değil). Ayrıştırma hatalı olabilir." << std::endl;
          // MFT_RECORD_SIZE'ı dinamik olarak ayarlamak daha doğru olurdu.
     }

    // $MFT'nin data run'larını bul
    auto mft_runs = find_mft_data_runs(hDevice, vol_data);
    if (mft_runs.empty()) {
        std::wcerr << L"Hata: $MFT data run'ları bulunamadı. Tarama iptal edildi." << std::endl;
        return;
    }

    // Tüm MFT kayıtlarını okuyup ayrıştır
    std::map<uint64_t, FileData> found_files_map; // MFT Ref -> FileData (Yol oluşturma için map daha uygun)
    uint64_t current_mft_ref = 0;
    long long records_processed = 0;
    long long records_valid = 0;
    long long read_errors = 0;
    long long parse_errors = 0;

     auto read_start_time = std::chrono::high_resolution_clock::now();
     std::wcout << L"MFT Kayıtları okunuyor ve ayrıştırılıyor..." << std::endl;

    std::vector<BYTE> record_buffer(MFT_RECORD_SIZE); // Her kayıt için tampon
    std::vector<BYTE> cluster_buffer; // Cluster okuma için tampon

    for (const auto& run : mft_runs) {
        uint64_t start_lcn = run.first;
        uint64_t num_clusters_in_run = run.second;

        // Bu run'daki tüm cluster'ları oku
        if (!read_clusters(hDevice, start_lcn, num_clusters_in_run, vol_data.BytesPerCluster, cluster_buffer)) {
            read_errors++;
            continue; // Bu run'ı atla
        }

        // Cluster buffer'ındaki MFT kayıtlarını işle
        size_t num_records_in_buffer = cluster_buffer.size() / MFT_RECORD_SIZE;
        for (size_t i = 0; i < num_records_in_buffer; ++i) {
             // Kayıt verisini kopyala
             memcpy(record_buffer.data(), cluster_buffer.data() + i * MFT_RECORD_SIZE, MFT_RECORD_SIZE);

             // MFT Kayıt Başlığını al
             MFT_RECORD_HEADER* header = reinterpret_cast<MFT_RECORD_HEADER*>(record_buffer.data());

             // İmza kontrolü ("FILE" veya "BAAD")
             if (strncmp(header->Signature, "FILE", 4) != 0 && strncmp(header->Signature, "BAAD", 4) != 0) {
                  // Geçersiz imza, muhtemelen boş kayıt veya bozulma
                 current_mft_ref++;
                 continue;
             }

             // Fixup uygula
             if (!apply_fixup(record_buffer.data(), header)) {
                 parse_errors++;
                 current_mft_ref++;
                 continue; // Fixup başarısızsa bu kaydı atla
             }

             // MFT Kaydını ayrıştır
             FileData file_data;
             if (parse_mft_record(record_buffer.data(), current_mft_ref, file_data, drive_letter)) {
                 // Başarılı ayrıştırma
                 if (!file_data.is_deleted && file_data.mft_reference_number > 15) { // Silinmemiş ve meta-dosya değilse
                     found_files_map[current_mft_ref] = file_data;
                     records_valid++;
                     if (file_data.is_directory) g_total_dirs_found++; else g_total_files_found++;
                 }
             } else {
                 // Ayrıştırma başarısız (örn: $FILE_NAME yok) veya silinmiş/meta-dosya
                 // parse_errors++; // $FILE_NAME olmaması her zaman hata değil.
             }

             records_processed++;
             current_mft_ref++; // Sonraki MFT referansına geç

             // İlerleme göstergesi (her 10000 kayıtta bir)
             if (records_processed % 10000 == 0) {
                 std::wcout << L"\rİşlenen MFT Kayıtları: " << records_processed
                            << L", Geçerli: " << records_valid
                            << L", Okuma Hata: " << read_errors
                            << L", Ayrıştırma Hata: " << parse_errors << L"  " << std::flush;
             }

        } // for (records in buffer)
    } // for (runs)

     auto read_end_time = std::chrono::high_resolution_clock::now();
     auto read_duration = std::chrono::duration_cast<std::chrono::milliseconds>(read_end_time - read_start_time);
     std::wcout << L"\rİşlenen MFT Kayıtları: " << records_processed
                << L", Geçerli: " << records_valid
                << L", Okuma Hata: " << read_errors
                << L", Ayrıştırma Hata: " << parse_errors << std::endl;
     std::wcout << L"MFT Okuma ve Ayrıştırma tamamlandı (" << read_duration.count() << L" ms)." << std::endl;

    // Yolları oluştur
    reconstruct_paths(found_files_map);

    // Map'i veritabanına yazılacak vektöre dönüştür
    std::vector<FileData> file_entries_vec;
    file_entries_vec.reserve(found_files_map.size());
    for (const auto& pair : found_files_map) {
        // Bilinmeyen yola sahip olanları atlayabiliriz
        if (pair.second.path != L"<unknown_path>") {
             file_entries_vec.push_back(pair.second);
        }
    }

    // Veritabanına yaz
    write_to_db(file_entries_vec);

     auto total_end_time = std::chrono::high_resolution_clock::now();
     auto total_duration = std::chrono::duration_cast<std::chrono::seconds>(total_end_time - total_start_time);
     std::wcout << L"✅ " << drive_letter << L": Sürücüsü taraması tamamlandı (" << total_duration.count() << L" saniye)." << std::endl;
}

// Kullanılabilir sabit NTFS sürücülerini bulur
std::vector<std::wstring> get_available_ntfs_drives() {
    std::vector<std::wstring> drives;
    DWORD buffer_size = GetLogicalDriveStringsW(0, nullptr);
    if (buffer_size == 0) {
        std::wcerr << L"Hata: Mantıksal sürücü listesi alınamadı. Hata Kodu: " << GetLastError() << std::endl;
        return drives;
    }

    std::vector<wchar_t> buffer(buffer_size);
    if (GetLogicalDriveStringsW(buffer_size, buffer.data()) == 0) {
         std::wcerr << L"Hata: Mantıksal sürücü listesi alınamadı (adım 2). Hata Kodu: " << GetLastError() << std::endl;
        return drives;
    }

    wchar_t* current_drive = buffer.data();
    while (*current_drive) {
        std::wstring drive_path(current_drive); // C:\, D:\ vb.
        // Sadece sabit diskleri (DRIVE_FIXED) al
        if (GetDriveTypeW(drive_path.c_str()) == DRIVE_FIXED) {
            // Dosya sisteminin NTFS olup olmadığını kontrol et
            wchar_t fs_name[MAX_PATH] = {0};
            if (GetVolumeInformationW(drive_path.c_str(), nullptr, 0, nullptr, nullptr, nullptr, fs_name, MAX_PATH)) {
                if (_wcsicmp(fs_name, L"NTFS") == 0) {
                    // Sadece sürücü harfini al (C, D vb.)
                    drives.push_back(drive_path.substr(0, 1));
                } else {
                     std::wcout << L"Bilgi: Sürücü " << drive_path << L" NTFS değil (" << fs_name << L"), atlanıyor." << std::endl;
                }
            } else {
                std::wcerr << L"Uyarı: Sürücü bilgisi alınamadı: " << drive_path << L", Hata Kodu: " << GetLastError() << std::endl;
            }
        }
        current_drive += drive_path.length() + 1; // Sonraki sürücüye geç
    }
    return drives;
}

// Tüm uygun sürücüleri tara
void scan_all_drives() {
    if (!init_db()) { // Veritabanını başlat/aç
        std::wcerr << L"Veritabanı başlatılamadığı için tarama yapılamıyor." << std::endl;
        return;
    }

    std::vector<std::wstring> drives_to_scan = get_available_ntfs_drives();

    if (drives_to_scan.empty()) {
        std::wcout << L"Taranacak uygun NTFS sürücüsü bulunamadı." << std::endl;
        return;
    }

    std::wcout << L"Tarama Başlatılıyor. Bulunan NTFS Sürücüleri: ";
    for (const auto& d : drives_to_scan) { std::wcout << d << L":\\ "; }
    std::wcout << std::endl;

    g_total_files_found = 0;
    g_total_dirs_found = 0;
    auto scan_start_time = std::chrono::high_resolution_clock::now();

    // Paralel tarama
    std::vector<std::thread> scan_threads;
    for (const auto& drive : drives_to_scan) {
        scan_threads.emplace_back(scan_mft_for_drive, drive);
    }

    // Tüm thread'leri bekle
    for (auto& t : scan_threads) {
        if (t.joinable()) {
            t.join();
        }
    }

    auto scan_end_time = std::chrono::high_resolution_clock::now();
    auto scan_duration = std::chrono::duration_cast<std::chrono::seconds>(scan_end_time - scan_start_time);
    std::wcout << L"\n✅ Tüm sürücülerin taraması tamamlandı." << std::endl;
    std::wcout << L"Toplam Süre: " << scan_duration.count() << L" saniye" << std::endl;
    std::wcout << L"Toplam Bulunan: " << g_total_files_found << L" dosya, " << g_total_dirs_found << L" dizin." << std::endl;

    // Tarama sonrası veritabanını optimize et (isteğe bağlı)
    std::wcout << L"Veritabanı optimize ediliyor (VACUUM)..." << std::endl;
    char* err_msg = nullptr;
     {
         std::lock_guard<std::mutex> lock(g_db_mutex);
         if (g_db) {
             sqlite3_exec(g_db, "VACUUM;", nullptr, nullptr, &err_msg);
         }
     }
     if (err_msg) {
         std::wcerr << L"VACUUM hatası: " << sqlite3_errmsg16(g_db) << std::endl;
         sqlite3_free(err_msg);
     } else {
         std::wcout << L"Veritabanı optimizasyonu tamamlandı." << std::endl;
     }
}