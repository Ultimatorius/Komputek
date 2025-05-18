#include "file_scanner.h"
#include "utils.h"
// database_manager.h zaten file_scanner.h içinde include edildi (DatabaseManager argümanı için)

#include <windows.h>
#include <winioctl.h>
#include <iostream>
#include <sstream>
#include <iomanip>
#include <map>
#include <set>
#include <algorithm>
#include <thread>
#include <mutex> // Bu mutex artık DatabaseManager içinde
#include <chrono>
#include <clocale> // _wsetlocale için
#include <filesystem> // std::filesystem::path işlemleri için gerekebilir
// #include <sqlite3.h> // Artık DatabaseManager üzerinden yönetiliyor

// Toplam bulunan dosya/dizin sayısı (istatistik için, thread-safe)
static std::atomic<long long> g_total_files_found_atomic(0);
static std::atomic<long long> g_total_dirs_found_atomic(0);


// RAII için Handle sarmalayıcı
struct SafeHandle {
    HANDLE handle = INVALID_HANDLE_VALUE;
    ~SafeHandle() {
        if (handle != INVALID_HANDLE_VALUE && handle != nullptr) {
            CloseHandle(handle);
        }
    }
    operator HANDLE() const { return handle; }
    HANDLE* operator&() { return &handle; }
};

// NTFS Attribute Başlıkları ve Yapıları (file_scanner.h'a taşınabilir veya burada kalabilir)
#pragma pack(push, 1)
struct ATTRIBUTE_HEADER_COMMON {
    AttributeType Type;
    ULONG Length;
    UCHAR NonResidentFlag;
    UCHAR NameLength;
    USHORT NameOffset;
    USHORT Flags;
    USHORT AttributeId;
};

struct ATTRIBUTE_HEADER_RESIDENT {
    ATTRIBUTE_HEADER_COMMON Common;
    ULONG ValueLength;
    USHORT ValueOffset;
    UCHAR IndexedFlag;
    UCHAR Padding;
};

struct ATTRIBUTE_HEADER_NON_RESIDENT {
    ATTRIBUTE_HEADER_COMMON Common;
    ULONGLONG StartingVCN;
    ULONGLONG LastVCN;
    USHORT DataRunsOffset;
    USHORT CompressionUnit;
    ULONG Padding;
    ULONGLONG AllocatedSize;
    ULONGLONG RealSize;
    ULONGLONG InitializedSize;
};

struct STANDARD_INFORMATION {
    ULONGLONG CreationTime;
    ULONGLONG AlteredTime; // FileAlteredTime
    ULONGLONG MFTChangedTime; // MFTAlteredTime
    ULONGLONG ReadTime; // FileReadTime
    ULONG DosFilePermissions; // Deprecated, use FileFlags in FILE_NAME
    ULONG MaxNumberOfVersions;
    ULONG VersionNumber;
    ULONG ClassId;
    ULONG OwnerId; // Vista+
    ULONG SecurityId; // Vista+
    ULONGLONG QuotaCharged; // Vista+
    USN Usn; // Update Sequence Number, Vista+
};

struct FILE_NAME_ATTRIBUTE {
    ULONGLONG ParentDirectoryReference; // Low 6 bytes is MFT#, high 2 bytes is sequence#
    ULONGLONG CreationTime;
    ULONGLONG AlteredTime;
    ULONGLONG MFTChangedTime;
    ULONGLONG ReadTime;
    ULONGLONG AllocatedSize; // Size of the file allocated on disk
    ULONGLONG RealSize; // Actual size of the file
    ULONG FileFlags; // e.g., ReadOnly, Hidden, System, Directory, Archive, Device, Normal, Temporary, SparseFile, ReparsePoint, Compressed, Offline, NotContentIndexed, Encrypted
    ULONG ReparseTagOrEaSize; // If FileFlags has ReparsePoint, this is ReparseTag. Else, size of EAs.
    UCHAR FileNameLength; // In characters
    FileNameNamespace NameSpace;
    // WCHAR FileName[FileNameLength]; // Variable length
};
#pragma pack(pop)


bool get_ntfs_volume_data(HANDLE hDevice, NTFS_VOLUME_DATA_BUFFER_STRUCT& volume_data) {
    DWORD bytes_returned = 0;
    if (!DeviceIoControl(hDevice, FSCTL_GET_NTFS_VOLUME_DATA, nullptr, 0, &volume_data, sizeof(volume_data), &bytes_returned, nullptr)) {
        std::wcerr << L"Hata: NTFS Volume bilgileri alınamadı. Hata Kodu: " << GetLastError() << std::endl;
        return false;
    }
    return true;
}

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
        if (!GetOverlappedResult(hDevice, &overlapped, &bytes_read, TRUE)) {
             std::wcerr << L"Hata: Bekleyen cluster okuma işlemi başarısız (LCN: " << start_lcn << L"). Hata Kodu: " << GetLastError() << std::endl;
            return false;
        }
    }

    if (bytes_read != bytes_to_read) {
        std::wcerr << L"Uyarı: Cluster okurken beklenenden az byte okundu (LCN: " << start_lcn << L"). İstenen: " << bytes_to_read << L", Okunan: " << bytes_read << std::endl;
        buffer.resize(bytes_read);
    }
    return true;
}

std::vector<std::pair<uint64_t, uint64_t>> find_mft_data_runs(HANDLE hDevice, const NTFS_VOLUME_DATA_BUFFER_STRUCT& vol_data) {
    std::vector<std::pair<uint64_t, uint64_t>> mft_runs_list;
    uint64_t total_mft_clusters_from_runs = 0;

    std::vector<BYTE> mft_record_0_buffer(MFT_RECORD_SIZE);
    LARGE_INTEGER mft_start_offset;
    mft_start_offset.QuadPart = vol_data.MftStartLcn.QuadPart * vol_data.BytesPerCluster;

    OVERLAPPED overlapped_mft0 = {0};
    overlapped_mft0.Offset = mft_start_offset.LowPart;
    overlapped_mft0.OffsetHigh = mft_start_offset.HighPart;

    DWORD bytes_read_mft0 = 0;
    if (!ReadFile(hDevice, mft_record_0_buffer.data(), MFT_RECORD_SIZE, &bytes_read_mft0, &overlapped_mft0)) {
         DWORD error = GetLastError();
        if (error != ERROR_IO_PENDING) {
            std::wcerr << L"Hata: MFT Kayıt 0 okunamadı. Hata Kodu: " << error << std::endl;
            return mft_runs_list;
        }
         if (!GetOverlappedResult(hDevice, &overlapped_mft0, &bytes_read_mft0, TRUE)) {
             std::wcerr << L"Hata: Bekleyen MFT Kayıt 0 okuma işlemi başarısız. Hata Kodu: " << GetLastError() << std::endl;
             return mft_runs_list;
         }
    }
     if (bytes_read_mft0 < MFT_RECORD_SIZE) {
         std::wcerr << L"Hata: MFT Kayıt 0 okunurken eksik byte okundu." << std::endl;
         return mft_runs_list;
     }

    MFT_RECORD_HEADER* header = reinterpret_cast<MFT_RECORD_HEADER*>(mft_record_0_buffer.data());
    if (strncmp(header->Signature, "FILE", 4) != 0) {
        std::wcerr << L"Hata: MFT Kayıt 0'ın imzası geçersiz ('FILE' değil)." << std::endl;
        return mft_runs_list;
    }

    if (!apply_fixup(mft_record_0_buffer.data(), header)) { // apply_fixup utils.cpp'de
         std::wcerr << L"Hata: MFT Kayıt 0 fixup uygulanamadı." << std::endl;
        return mft_runs_list;
    }

    BYTE* p_attr = mft_record_0_buffer.data() + header->FirstAttributeOffset;
    BYTE* record_end = mft_record_0_buffer.data() + header->UsedSize;

    while (p_attr < record_end) {
        ATTRIBUTE_HEADER_COMMON* attr_header = reinterpret_cast<ATTRIBUTE_HEADER_COMMON*>(p_attr);
        if (attr_header->Type == AttributeType::END_OF_ATTRIBUTES) break;
        if (attr_header->Length == 0) {
            std::wcerr << L"Hata: MFT Kayıt 0'da sıfır uzunluklu öznitelik." << std::endl;
            break;
        }

        if (attr_header->Type == AttributeType::DATA) {
            if (attr_header->NonResidentFlag == 1) {
                ATTRIBUTE_HEADER_NON_RESIDENT* nr_header = reinterpret_cast<ATTRIBUTE_HEADER_NON_RESIDENT*>(p_attr);
                BYTE* run_list_ptr = p_attr + nr_header->DataRunsOffset;
                ULONG run_list_len = nr_header->Common.Length - nr_header->DataRunsOffset;
                mft_runs_list = parse_data_runs(run_list_ptr, run_list_len, total_mft_clusters_from_runs); // parse_data_runs utils.cpp'de
                std::wcout << L"Bilgi: $MFT için " << mft_runs_list.size() << L" adet data run bulundu. Toplam Cluster: " << total_mft_clusters_from_runs << std::endl;
                 if (total_mft_clusters_from_runs * vol_data.BytesPerCluster < vol_data.MftValidDataLength.QuadPart) {
                      std::wcerr << L"Uyarı: Data run'lardan hesaplanan MFT boyutu, volume bilgisindeki MFT boyutundan küçük!" << std::endl;
                 }
            } else {
                 std::wcerr << L"Uyarı: MFT Kayıt 0'daki $DATA özniteliği beklenmedik şekilde resident." << std::endl;
            }
            break;
        }
        p_attr += attr_header->Length;
    }
    if (mft_runs_list.empty()) {
         std::wcerr << L"Hata: MFT Kayıt 0 içinde $DATA özniteliği veya data run'ları bulunamadı!" << std::endl;
    }
    return mft_runs_list;
}


bool parse_mft_record(const BYTE* buffer, uint64_t mft_ref, FileData& file_data, const std::wstring& drive_letter_prefix) {
    const MFT_RECORD_HEADER* header = reinterpret_cast<const MFT_RECORD_HEADER*>(buffer);

    file_data.mft_reference_number = mft_ref;
    file_data.drive = drive_letter_prefix; // Sadece "C:", "D:" gibi, sonuna "\" reconstruct_paths ekleyecek
    file_data.is_deleted = !(header->Flags & 0x0001);
    file_data.is_directory = (header->Flags & 0x0002);

    bool found_std_info = false;
    bool found_file_name_attr = false;

    const BYTE* p_attr = buffer + header->FirstAttributeOffset;
    const BYTE* record_end = buffer + header->UsedSize; // UsedSize daha güvenli

    while (p_attr < record_end && p_attr + sizeof(ATTRIBUTE_HEADER_COMMON) <= record_end ) {
        const ATTRIBUTE_HEADER_COMMON* attr_header_common = reinterpret_cast<const ATTRIBUTE_HEADER_COMMON*>(p_attr);

        if (attr_header_common->Type == AttributeType::END_OF_ATTRIBUTES) break;
        if (attr_header_common->Length == 0) {
            // std::wcerr << L"Uyarı: MFT Ref " << mft_ref << " sıfır uzunluklu öznitelik, atlanıyor." << std::endl;
            break; // Hatalı durumu sonlandır
        }
         // Güvenlik kontrolü: öznitelik uzunluğu kaydın sonunu aşmamalı
        if (p_attr + attr_header_common->Length > record_end) {
           // std::wcerr << L"Uyarı: MFT Ref " << mft_ref << " öznitelik uzunluğu kayıt sonunu aşıyor." << std::endl;
            break;
        }


        switch (attr_header_common->Type) {
            case AttributeType::STANDARD_INFORMATION: {
                if (!attr_header_common->NonResidentFlag) {
                    const ATTRIBUTE_HEADER_RESIDENT* res_header = reinterpret_cast<const ATTRIBUTE_HEADER_RESIDENT*>(p_attr);
                    if (res_header->ValueOffset < attr_header_common->Length &&
                        res_header->ValueLength == sizeof(STANDARD_INFORMATION)) {
                        const STANDARD_INFORMATION* std_info = reinterpret_cast<const STANDARD_INFORMATION*>(p_attr + res_header->ValueOffset);
                        file_data.creation_time = std_info->CreationTime;
                        file_data.modification_time = std_info->AlteredTime;
                        file_data.access_time = std_info->ReadTime;
                        file_data.mft_change_time = std_info->MFTChangedTime;
                        // file_data.file_attributes |= std_info->DosFilePermissions; // FILE_NAME içindeki daha iyi
                        found_std_info = true;
                    }
                }
                break;
            }
            case AttributeType::FILE_NAME: {
                if (!attr_header_common->NonResidentFlag) {
                    const ATTRIBUTE_HEADER_RESIDENT* res_header = reinterpret_cast<const ATTRIBUTE_HEADER_RESIDENT*>(p_attr);
                    if (res_header->ValueOffset < attr_header_common->Length) {
                        const FILE_NAME_ATTRIBUTE* fn_attr = reinterpret_cast<const FILE_NAME_ATTRIBUTE*>(p_attr + res_header->ValueOffset);

                        if (fn_attr->NameSpace == FileNameNamespace::WIN32 || fn_attr->NameSpace == FileNameNamespace::WIN32_DOS || fn_attr->NameSpace == FileNameNamespace::POSIX) {
                            // POSIX'i de alalım, bazen sadece o oluyor. Win32 tercih edilir.
                            // Eğer zaten bir Win32 adı bulunduysa POSIX'i alma.
                            if (found_file_name_attr && fn_attr->NameSpace == FileNameNamespace::POSIX && !file_data.name.empty()) {
                                // continue to next attribute if we already have a win32 name
                            } else {
                                int name_len_bytes = fn_attr->FileNameLength * sizeof(wchar_t);
                                if (res_header->ValueOffset + sizeof(FILE_NAME_ATTRIBUTE) + name_len_bytes <= res_header->Common.Length) {
                                    const wchar_t* filename_ptr = reinterpret_cast<const wchar_t*>(reinterpret_cast<const BYTE*>(fn_attr) + sizeof(FILE_NAME_ATTRIBUTE));
                                    file_data.name.assign(filename_ptr, fn_attr->FileNameLength);

                                    if (!file_data.is_directory) {
                                        size_t dot_pos = file_data.name.rfind(L'.');
                                        if (dot_pos != std::wstring::npos && dot_pos > 0 && dot_pos < file_data.name.length() -1) { // Sadece . değil ve sonda değil
                                            file_data.extension = file_data.name.substr(dot_pos);
                                        } else {
                                            file_data.extension.clear();
                                        }
                                    } else {
                                        file_data.extension.clear();
                                    }
                                    file_data.parent_mft_reference_number = get_mft_ref_number_only(fn_attr->ParentDirectoryReference);
                                    file_data.file_attributes = fn_attr->FileFlags;
                                    if (!file_data.is_directory) { // Dizin boyutu $DATA'dan değil, içeriğinden gelir.
                                        file_data.size = fn_attr->RealSize; // $FILE_NAME içindeki boyut daha güvenilir olabilir
                                    }


                                    if (!found_std_info) { // $STANDARD_INFORMATION yoksa buradan al
                                        file_data.creation_time = fn_attr->CreationTime;
                                        file_data.modification_time = fn_attr->AlteredTime;
                                        file_data.access_time = fn_attr->ReadTime;
                                        file_data.mft_change_time = fn_attr->MFTChangedTime;
                                    }
                                    found_file_name_attr = true;
                                     // En iyi adı (Win32) bulduysak diğer FILE_NAME'leri atlayabiliriz (performans)
                                    if (fn_attr->NameSpace == FileNameNamespace::WIN32 || fn_attr->NameSpace == FileNameNamespace::WIN32_DOS) {
                                        // Found the best name, we can potentially break from processing more FILE_NAME attributes
                                        // but sometimes multiple FILE_NAME attributes exist (e.g. hard links, or short/long names)
                                        // For simplicity, we take the first good one.
                                    }
                                }
                            }
                        }
                    }
                }
                break;
            }
            case AttributeType::DATA: {
                if (!file_data.is_directory) { // Dizinlerin $DATA'sı genellikle metadata içerir, gerçek boyutu değil.
                    if (attr_header_common->NonResidentFlag == 0) {
                        const ATTRIBUTE_HEADER_RESIDENT* res_header = reinterpret_cast<const ATTRIBUTE_HEADER_RESIDENT*>(p_attr);
                        // Sadece isimsiz $DATA özniteliğini al (varsayılan veri akışı)
                        if (res_header->Common.NameLength == 0) {
                           file_data.size = res_header->ValueLength;
                        }
                    } else {
                        const ATTRIBUTE_HEADER_NON_RESIDENT* nr_header = reinterpret_cast<const ATTRIBUTE_HEADER_NON_RESIDENT*>(p_attr);
                        if (nr_header->Common.NameLength == 0) {
                            file_data.size = nr_header->RealSize;
                        }
                    }
                }
                break;
            }
            case AttributeType::ATTRIBUTE_LIST: {
                // std::wcerr << L"Uyarı: MFT Ref " << mft_ref << L" için $ATTRIBUTE_LIST bulundu (işlenmedi)." << std::endl;
                break; // İşlenmiyor
            }
            default:
                break;
        }
        p_attr += attr_header_common->Length;
    }

    if (!found_file_name_attr && mft_ref > 15) { // İlk 16 kayıt NTFS meta dosyalarıdır
        // Silinmiş veya özel MFT kayıtları ($MFT, $LogFile vs.) için $FILE_NAME olmaması normaldir.
        // if (!file_data.is_deleted) {
        //    std::wcerr << L"Uyarı: MFT Ref " << mft_ref << L" için $FILE_NAME özniteliği bulunamadı." << std::endl;
        // }
        return false; // $FILE_NAME olmadan kaydı geçersiz sayalım (çok temel meta dosyalar hariç)
    }
    return true;
}

void reconstruct_paths(std::map<uint64_t, FileData>& ref_to_data, const std::wstring& drive_root_path_prefix) {
    std::wcout << L"Dosya yolları oluşturuluyor (" << drive_root_path_prefix << L")..." << std::endl;
    auto start_time = std::chrono::high_resolution_clock::now();
    long long paths_constructed = 0;

    // Kök dizini bul (MFT Ref 5)
    const uint64_t ROOT_MFT_REF = 5; // NTFS root directory MFT entry number

    std::function<std::wstring(uint64_t, std::vector<uint64_t>&)> build_path_recursive =
        [&](uint64_t current_mft_ref, std::vector<uint64_t>& visited_refs) -> std::wstring {
        if (ref_to_data.find(current_mft_ref) == ref_to_data.end()) {
            return L"<unknown_parent_ref_" + std::to_wstring(current_mft_ref) + L">";
        }

        // Döngüsel bağımlılık kontrolü
        if (std::find(visited_refs.begin(), visited_refs.end(), current_mft_ref) != visited_refs.end()) {
            return L"<cyclic_dependency_at_" + std::to_wstring(current_mft_ref) + L">";
        }
        visited_refs.push_back(current_mft_ref);

        FileData& current_entry = ref_to_data[current_mft_ref];

        // Kök dizin özel durumu
        if (current_mft_ref == ROOT_MFT_REF) {
            visited_refs.pop_back();
            return L""; // Kökün üstü yok, sadece sürücü harfi olacak
        }

        // Ebeveynin yolu
        std::wstring parent_full_path = build_path_recursive(current_entry.parent_mft_reference_number, visited_refs);
        
        visited_refs.pop_back(); // Geri dönüşte temizle

        if (parent_full_path.rfind(L"<unknown", 0) == 0 || parent_full_path.rfind(L"<cyclic", 0) == 0) { // Hatalı ebeveyn yolu
             return parent_full_path + L"\\" + current_entry.name; // Hatayı yay
        }
        
        // Kendi tam yolu = ebeveyn_yolu + \ + kendi_adı
        // Ebeveyn kök ise (parent_full_path boş string ise), sadece kendi adını ekle.
        // Değilse, araya \ ekle.
        if (parent_full_path.empty()) {
             return current_entry.name;
        } else {
             return parent_full_path + L"\\" + current_entry.name;
        }
    };

    for (auto& pair_entry : ref_to_data) {
        FileData& file_entry = pair_entry.second;
        std::vector<uint64_t> visited_for_this_path; // Her dosya için temiz ziyaret listesi

        if (file_entry.mft_reference_number == ROOT_MFT_REF) {
            file_entry.path = drive_root_path_prefix; // Örn: C:\
        } else {
            std::wstring relative_path = build_path_recursive(file_entry.mft_reference_number, visited_for_this_path);
            if (relative_path.rfind(L"<unknown", 0) == 0 || relative_path.rfind(L"<cyclic", 0) == 0) {
                 file_entry.path = drive_root_path_prefix + relative_path; // Hatalı yolu göster
            } else {
                 file_entry.path = drive_root_path_prefix; // Sürücü harfi
                 // build_path_recursive zaten tam yolu (kök hariç) döndürecek.
                 // Bu yüzden, build_path_recursive'in ebeveyn adını da içermesi gerekiyor.
                 // Şöyle yapalım:
                 // reconstruct_paths'in görevi, her FileData nesnesinin `path` alanını doldurmak.
                 // `path` alanı, dosyanın tam yolu olacak (sürücü dahil).
                 // `name` alanı sadece dosya/dizin adı.
                 // build_path_recursive fonksiyonu, verilen MFT ref için *sadece dosya adlarından oluşan göreceli yolu* döndürmeli.
                 // Kök dizinin path'i drive_root_path_prefix (C:\)
                 // Diğerlerinin path'i drive_root_path_prefix + build_path_recursive(parent) + name
                 // Hayır, daha basiti: build_path_recursive, ebeveyninin adını değil, ebeveyninin *tam yolunu* almalı.

                 // Yeniden düzenlenmiş `reconstruct_paths` ve `build_path_recursive` gerekli.
                 // Şimdiki `reconstruct_paths` içindeki loop ve `build_path_recursive` yerine
                 // her bir file_data'nın path'ini şu şekilde set edelim:
                 // file_data.path = drive_root_path_prefix + (ebeveyn_path_stringi) + file_data.name
                 // ebeveyn_path_stringi'ni bulmak için ebeveynin FileData'sına bakılır.

                 // Geçici olarak basit bir yol oluşturma:
                 // Bu kısım daha robust hale getirilmeli. Veritabanına yazmadan önce yolların doğru olması önemli.
                 // Mevcut `parse_mft_record` zaten `file_data.name` ve `file_data.parent_mft_reference_number`'ı set ediyor.
                 // `reconstruct_paths` bu bilgiyi kullanarak `file_data.path`'ı doldurmalı.

                 // Basitleştirilmiş yol oluşturma (rekürsif olmayan, tüm ebeveynlerin map'te olduğu varsayımıyla):
                 std::wstring current_full_path = L"";
                 uint64_t current_parent_ref = file_entry.parent_mft_reference_number;
                 std::vector<std::wstring> path_parts;
                 path_parts.push_back(file_entry.name);

                 int depth_limit = 0; // Döngüsel bağımlılık için basit bir koruma
                 while(current_parent_ref != ROOT_MFT_REF && current_parent_ref != 0 && depth_limit < 255) { // 0 geçersiz ref
                    auto it_parent = ref_to_data.find(current_parent_ref);
                    if (it_parent != ref_to_data.end()) {
                        path_parts.push_back(it_parent->second.name);
                        current_parent_ref = it_parent->second.parent_mft_reference_number;
                    } else {
                        path_parts.push_back(L"<unknown_parent_" + std::to_wstring(current_parent_ref) + L">");
                        break; // Ebeveyn bulunamadı
                    }
                    depth_limit++;
                 }
                 if (depth_limit >= 255) path_parts.push_back(L"<path_too_deep>");

                 std::reverse(path_parts.begin(), path_parts.end());
                 
                 // file_entry.path = drive_root_path_prefix; // C:\
                 // for(const auto& part : path_parts) {
                 //    if (!file_entry.path.empty() && file_entry.path.back() != L'\\') file_entry.path += L'\\';
                 //    file_entry.path += part;
                 // }
                 // Path'i sadece ebeveyn yolu olarak ayarlayalım, FTS trigger'ı path || name yapar.
                 // Ya da path'i tam yol (isim hariç) yapalım.
                 // FTS için: path = "C:\Windows", name = "System32" -> FTS content = "C:\Windows\System32"
                 // Bu yüzden FileData.path, dosyanın bulunduğu dizinin yolu olmalı.
                 file_entry.path = drive_root_path_prefix.substr(0, drive_root_path_prefix.length()-1); // C: -> C
                 for(size_t i = 0; i < path_parts.size() -1; ++i) { // Son kısım (kendi adı) hariç
                    file_entry.path += L"\\" + path_parts[i];
                 }
                 if (path_parts.empty() && file_entry.mft_reference_number == ROOT_MFT_REF) { // Kök dizin için
                     file_entry.path = drive_root_path_prefix.substr(0, drive_root_path_prefix.length()-1); // "C:"
                 } else if (path_parts.size() == 1 && file_entry.parent_mft_reference_number == ROOT_MFT_REF) { // Kökün hemen altı
                     file_entry.path = drive_root_path_prefix.substr(0, drive_root_path_prefix.length()-1); // "C:"
                 }

            } // else (not ROOT_MFT_REF)
        } // for (auto& pair_entry : ref_to_data)
        paths_constructed++; // Her giriş için bir yol oluşturuldu (veya denendi)
    }


    auto end_time = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);
    std::wcout << L"✅ " << paths_constructed << L" adet dosya yolu oluşturma işlemi tamamlandı (" << duration.count() << L" ms)." << std::endl;
}


void scan_mft_for_drive(const std::wstring& drive_letter, DatabaseManager& db_manager) {
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
        FILE_FLAG_NO_BUFFERING | FILE_FLAG_OVERLAPPED,
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

    NTFS_VOLUME_DATA_BUFFER_STRUCT vol_data = {0};
    if (!get_ntfs_volume_data(hDevice, vol_data)) return;

    std::wcout << L"NTFS Volume Bilgileri Alındı:" << std::endl;
    std::wcout << L"  Bytes Per Cluster: " << vol_data.BytesPerCluster << std::endl;
    std::wcout << L"  MFT Record Size (hesaplanan): ";
    DWORD calculated_mft_record_size = 0;
    if (vol_data.ClustersPerFileRecordSegment > 0) { // Pozitif ise 2^N * ClusterSize
         calculated_mft_record_size = (1 << vol_data.ClustersPerFileRecordSegment) * vol_data.BytesPerCluster;
    } else { // Negatif ise (örn: -10 -> 1KB), -N byte demek
         calculated_mft_record_size = -static_cast<signed char>(vol_data.ClustersPerFileRecordSegment); // Örnek bir yorum, gerçek implementasyon farklı olabilir
                                                                                                        // Genellikle MFT_RECORD_SIZE sabiti kullanılır.
                                                                                                        // Veya $MFT dosyasının ilk kaydından öğrenilir.
                                                                                                        // Şimdilik MFT_RECORD_SIZE sabitine güveniyoruz.
    }
    std::wcout << MFT_RECORD_SIZE << L" (sabit)" << std::endl;


    auto mft_runs = find_mft_data_runs(hDevice, vol_data);
    if (mft_runs.empty()) {
        std::wcerr << L"Hata: $MFT data run'ları bulunamadı. Tarama iptal edildi." << std::endl;
        return;
    }

    std::map<uint64_t, FileData> found_files_map;
    uint64_t current_mft_ref = 0;
    long long records_processed = 0;
    long long records_valid = 0;
    long long read_errors = 0;
    long long parse_errors = 0;

    auto read_start_time = std::chrono::high_resolution_clock::now();
    std::wcout << L"MFT Kayıtları okunuyor ve ayrıştırılıyor..." << std::endl;

    std::vector<BYTE> record_buffer(MFT_RECORD_SIZE);
    std::vector<BYTE> cluster_buffer;

    std::wstring drive_prefix = drive_letter + L":"; // Örn: "C:" (reconstruct_paths'e "\\"" ekleyecek)

    for (const auto& run : mft_runs) {
        uint64_t start_lcn = run.first;
        uint64_t num_clusters_in_run = run.second;

        if (!read_clusters(hDevice, start_lcn, num_clusters_in_run, vol_data.BytesPerCluster, cluster_buffer)) {
            read_errors++;
            continue;
        }

        size_t num_records_in_buffer = cluster_buffer.size() / MFT_RECORD_SIZE;
        for (size_t i = 0; i < num_records_in_buffer; ++i) {
            memcpy(record_buffer.data(), cluster_buffer.data() + i * MFT_RECORD_SIZE, MFT_RECORD_SIZE);
            MFT_RECORD_HEADER* header = reinterpret_cast<MFT_RECORD_HEADER*>(record_buffer.data());

            if (strncmp(header->Signature, "FILE", 4) != 0 && strncmp(header->Signature, "BAAD", 4) != 0) {
                current_mft_ref++; // MFT ref'i yine de ilerlet
                continue;
            }
            if (strncmp(header->Signature, "BAAD", 4) == 0) { // Bozuk sektör
                // std::wcerr << L"Uyarı: BAAD imzalı MFT kaydı (Ref: " << current_mft_ref << "), atlanıyor." << std::endl;
                current_mft_ref++;
                parse_errors++;
                continue;
            }


            if (!apply_fixup(record_buffer.data(), header)) {
                // std::wcerr << L"Uyarı: Fixup başarısız MFT Ref " << current_mft_ref << ", atlanıyor." << std::endl;
                parse_errors++;
                current_mft_ref++;
                continue;
            }

            FileData file_data;
            if (parse_mft_record(record_buffer.data(), current_mft_ref, file_data, drive_prefix)) {
                if (!file_data.is_deleted && !file_data.name.empty()) { // Silinmemiş ve adı olanları al
                    found_files_map[current_mft_ref] = file_data;
                    records_valid++;
                    if (file_data.is_directory) g_total_dirs_found_atomic++; else g_total_files_found_atomic++;
                }
            } else {
                // parse_errors++; // Her zaman hata değil (meta-dosya, silinmiş vb.)
            }
            records_processed++;
            current_mft_ref++;

            if (records_processed % 10000 == 0) {
                std::wcout << L"\rİşlenen MFT Kayıtları: " << records_processed
                           << L", Geçerli: " << records_valid
                           << L", Okuma Hata: " << read_errors
                           << L", Ayrıştırma Hata: " << parse_errors << L"  " << std::flush;
            }
        }
    }
    auto read_end_time = std::chrono::high_resolution_clock::now();
    auto read_duration = std::chrono::duration_cast<std::chrono::milliseconds>(read_end_time - read_start_time);
    std::wcout << L"\rİşlenen MFT Kayıtları: " << records_processed
               << L", Geçerli: " << records_valid
               << L", Okuma Hata: " << read_errors
               << L", Ayrıştırma Hata: " << parse_errors << std::endl;
    std::wcout << L"MFT Okuma ve Ayrıştırma tamamlandı (" << read_duration.count() << L" ms)." << std::endl;

    reconstruct_paths(found_files_map, drive_prefix + L"\\"); // Örn: "C:\\"

    std::vector<FileData> file_entries_vec;
    file_entries_vec.reserve(found_files_map.size());
    for (const auto& pair : found_files_map) {
        if (pair.second.path.rfind(L"<unknown",0) != 0 && pair.second.path.rfind(L"<cyclic",0) !=0 && pair.second.path.rfind(L"<path_too_deep>",0) !=0) {
             file_entries_vec.push_back(pair.second);
        } else {
            // std::wcerr << L"Hatalı yola sahip girdi atlanıyor: MFT Ref " << pair.first << L" Path: " << pair.second.path << L" Name: " << pair.second.name << std::endl;
        }
    }

    if (db_manager.is_open()) {
        db_manager.write_entries(file_entries_vec);
    } else {
        std::wcerr << L"Veritabanı kapalı, " << drive_letter << L": sürücüsü için veriler yazılamadı." << std::endl;
    }

    auto total_end_time = std::chrono::high_resolution_clock::now();
    auto total_duration = std::chrono::duration_cast<std::chrono::seconds>(total_end_time - total_start_time);
    std::wcout << L"✅ " << drive_letter << L": Sürücüsü taraması tamamlandı (" << total_duration.count() << L" saniye)." << std::endl;
}

std::vector<std::wstring> get_available_ntfs_drives() {
    std::vector<std::wstring> drives;
    DWORD buffer_len = GetLogicalDriveStringsW(0, nullptr);
    if (buffer_len == 0) return drives;

    std::vector<wchar_t> buffer(buffer_len);
    if (GetLogicalDriveStringsW(buffer_len, buffer.data()) == 0) return drives;

    wchar_t* current_drive_ptr = buffer.data();
    while (*current_drive_ptr) {
        std::wstring drive_path_str(current_drive_ptr); // C:\, D:\
        if (GetDriveTypeW(drive_path_str.c_str()) == DRIVE_FIXED) {
            wchar_t fs_name_buffer[MAX_PATH] = {0};
            if (GetVolumeInformationW(drive_path_str.c_str(), nullptr, 0, nullptr, nullptr, nullptr, fs_name_buffer, MAX_PATH)) {
                if (_wcsicmp(fs_name_buffer, L"NTFS") == 0) {
                    drives.push_back(drive_path_str.substr(0, 1)); // C, D
                } else {
                    std::wcout << L"Bilgi: Sürücü " << drive_path_str << L" NTFS değil (" << fs_name_buffer << L"), atlanıyor." << std::endl;
                }
            } else {
                 std::wcerr << L"Uyarı: Sürücü bilgisi alınamadı: " << drive_path_str << L", Hata Kodu: " << GetLastError() << std::endl;
            }
        }
        current_drive_ptr += drive_path_str.length() + 1;
    }
    return drives;
}

void scan_all_drives(DatabaseManager& db_manager) {
    if (!db_manager.is_open()) {
        std::wcerr << L"Veritabanı açık değil. Tarama başlatılamıyor." << std::endl;
        if (!db_manager.open_db()) { // Açmayı dene
            std::wcerr << L"Veritabanı açılamadı. Tarama iptal edildi." << std::endl;
            return;
        }
        if (!db_manager.init_schema()){ // Şemayı oluştur/kontrol et
             std::wcerr << L"Veritabanı şeması başlatılamadı. Tarama iptal edildi." << std::endl;
            return;
        }
    }


    std::vector<std::wstring> drives_to_scan = get_available_ntfs_drives();
    if (drives_to_scan.empty()) {
        std::wcout << L"Taranacak uygun NTFS sürücüsü bulunamadı." << std::endl;
        return;
    }

    std::wcout << L"Tarama Başlatılıyor. Bulunan NTFS Sürücüleri: ";
    for (const auto& d : drives_to_scan) { std::wcout << d << L":\\ "; }
    std::wcout << std::endl;

    g_total_files_found_atomic = 0;
    g_total_dirs_found_atomic = 0;
    auto scan_start_time = std::chrono::high_resolution_clock::now();

    std::vector<std::thread> scan_threads;
    for (const auto& drive : drives_to_scan) {
        scan_threads.emplace_back(scan_mft_for_drive, drive, std::ref(db_manager));
    }

    for (auto& t : scan_threads) {
        if (t.joinable()) {
            t.join();
        }
    }

    auto scan_end_time = std::chrono::high_resolution_clock::now();
    auto scan_duration = std::chrono::duration_cast<std::chrono::seconds>(scan_end_time - scan_start_time);
    std::wcout << L"\n✅ Tüm sürücülerin taraması tamamlandı." << std::endl;
    std::wcout << L"Toplam Süre: " << scan_duration.count() << L" saniye" << std::endl;
    std::wcout << L"Toplam Bulunan: " << g_total_files_found_atomic.load() << L" dosya, " << g_total_dirs_found_atomic.load() << L" dizin." << std::endl;

    std::wcout << L"Veritabanı optimize ediliyor (VACUUM)..." << std::endl;
    char* err_msg = nullptr;
    // VACUUM için db_manager üzerinden bir metot eklenebilir veya doğrudan handle kullanılabilir (dikkatli)
    if (db_manager.get_db_handle()) { // db_mutex zaten DatabaseManager içinde yönetiliyor olmalı
         // DatabaseManager'a VACUUM için özel bir metot eklemek daha iyi olurdu.
         // Şimdilik doğrudan erişim (eğer db_mutex uygunsa)
        std::lock_guard<std::mutex> lock(const_cast<std::mutex&>(db_manager.*(&DatabaseManager::db_mutex_))); // Geçici çözüm
        if (sqlite3_exec(db_manager.get_db_handle(), "VACUUM;", nullptr, nullptr, &err_msg) != SQLITE_OK) {
            std::wcerr << L"VACUUM hatası: " << sqlite3_errmsg16(db_manager.get_db_handle()) << std::endl;
            sqlite3_free(err_msg);
        } else {
            std::wcout << L"Veritabanı optimizasyonu tamamlandı." << std::endl;
        }
    }
}