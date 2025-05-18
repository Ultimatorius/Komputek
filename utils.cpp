#include "utils.h"
#include "file_scanner.h" // MFT_RECORD_HEADER için (veya bunu common.h'a taşı)
#include <iostream> // std::wcerr için
#include <algorithm> // std::min için

std::wstring filetime_to_wstring(uint64_t ft_ul) {
    if (ft_ul == 0) return L"";
    FILETIME ft = *reinterpret_cast<FILETIME*>(&ft_ul);
    SYSTEMTIME st_utc, st_local;
    if (!FileTimeToSystemTime(&ft, &st_utc)) return L"<error_FileTimeToSystemTime>";
    if (!SystemTimeToTzSpecificLocalTime(nullptr, &st_utc, &st_local)) {
        st_local = st_utc; // Başarısız olursa UTC kullan
    }

    std::wstringstream ss;
    ss << std::setfill(L'0') << std::setw(4) << st_local.wYear << L"-"
       << std::setfill(L'0') << std::setw(2) << st_local.wMonth << L"-"
       << std::setfill(L'0') << std::setw(2) << st_local.wDay << L" "
       << std::setfill(L'0') << std::setw(2) << st_local.wHour << L":"
       << std::setfill(L'0') << std::setw(2) << st_local.wMinute << L":"
       << std::setfill(L'0') << std::setw(2) << st_local.wSecond;
    return ss.str();
}

std::wstring format_filesize(uint64_t size) {
    const double kb = 1024.0;
    const double mb = kb * 1024.0;
    const double gb = mb * 1024.0;
    std::wstringstream ss;
    ss << std::fixed << std::setprecision(1);

    if (size >= gb) ss << (size / gb) << L" GB";
    else if (size >= mb) ss << (size / mb) << L" MB";
    else if (size >= kb) ss << (size / kb) << L" KB";
    else ss << size << L" B";
    return ss.str();
}

uint64_t get_mft_ref_number_only(uint64_t full_ref) {
    return full_ref & 0x0000FFFFFFFFFFFFULL;
}

bool apply_fixup(BYTE* buffer, const MFT_RECORD_HEADER* header) {
    if (!header || header->FixupOffset == 0 || header->FixupCount == 0 ||
        header->FixupOffset >= MFT_RECORD_SIZE - sizeof(USHORT) * header->FixupCount || // Fixup array taşmamalı
        header->FixupCount > (MFT_RECORD_SIZE / 512) +1 ) { // Max fixup sayısı
        // std::wcerr << L"Geçersiz fixup bilgisi." << std::endl;
        return false;
    }

    USHORT* fixup_array = reinterpret_cast<USHORT*>(buffer + header->FixupOffset);
    USHORT fixup_signature = fixup_array[0];

    for (USHORT i = 1; i < header->FixupCount; ++i) {
        DWORD position_to_fix = (i * 512) - sizeof(USHORT);
        if (position_to_fix >= MFT_RECORD_SIZE - sizeof(USHORT) +1) { // Sınır kontrolü
            // std::wcerr << L"Fixup pozisyonu MFT kayıt boyutunu aşıyor: " << position_to_fix << std::endl;
            return false; // Hatalı pozisyon
        }
        USHORT* value_at_position = reinterpret_cast<USHORT*>(buffer + position_to_fix);
        if (*value_at_position != fixup_signature) {
            // std::wcerr << L"Hata: Fixup imzası eşleşmedi! Poz: " << position_to_fix
            //           << L", Beklenen: " << fixup_signature << L", Bulunan: " << *value_at_position << std::endl;
            return false;
        }
        *value_at_position = fixup_array[i];
    }
    return true;
}

std::vector<std::pair<uint64_t, uint64_t>> parse_data_runs(
    const BYTE* run_list_start, ULONG run_list_length, uint64_t& total_clusters_in_runs) {
    std::vector<std::pair<uint64_t, uint64_t>> cluster_runs_vec;
    total_clusters_in_runs = 0;
    const BYTE* p = run_list_start;
    const BYTE* end_of_run_list = run_list_start + run_list_length;
    uint64_t current_lcn_offset_sum = 0;

    while (p < end_of_run_list && *p != 0x00) {
        if (p + 1 > end_of_run_list) { /*std::wcerr << L"Data run header okunamadı." << std::endl;*/ break; }
        BYTE header_byte = *p++;
        int length_field_bytes = header_byte & 0x0F;
        int offset_field_bytes = (header_byte >> 4) & 0x0F;

        if (length_field_bytes == 0 || length_field_bytes > 8 || offset_field_bytes > 8) {
            //std::wcerr << L"Geçersiz data run boyutları." << std::endl;
            break; 
        }
        if (p + length_field_bytes + offset_field_bytes > end_of_run_list) {
            //std::wcerr << L"Data run veri okuma taşması." << std::endl;
            break;
        }

        uint64_t run_length = 0;
        memcpy(&run_length, p, length_field_bytes);
        p += length_field_bytes;

        int64_t run_offset = 0; // İşaretli olmalı
        if (offset_field_bytes > 0) {
            // İşaretli ofset için son byte'ın en yüksek bitine bakılır
            BYTE last_offset_byte = *(p + offset_field_bytes - 1);
            bool is_negative = (last_offset_byte & 0x80) != 0;
            
            memcpy(&run_offset, p, offset_field_bytes);
            p += offset_field_bytes;

            if (is_negative) { // Negatifse, kalan bitleri 1 ile doldur (işaret genişletme)
                for (int k = offset_field_bytes; k < sizeof(run_offset); ++k) {
                    reinterpret_cast<BYTE*>(&run_offset)[k] = 0xFF;
                }
            }
        }
        
        current_lcn_offset_sum = static_cast<uint64_t>(static_cast<int64_t>(current_lcn_offset_sum) + run_offset);

        if (run_length > 0) {
            cluster_runs_vec.push_back({current_lcn_offset_sum, run_length});
            total_clusters_in_runs += run_length;
        }
    }
    return cluster_runs_vec;
}

std::wstring widen(const std::string& str) {
    if (str.empty()) return std::wstring();
    int size_needed = MultiByteToWideChar(CP_UTF8, 0, &str[0], (int)str.size(), NULL, 0);
    std::wstring wstrTo(size_needed, 0);
    MultiByteToWideChar(CP_UTF8, 0, &str[0], (int)str.size(), &wstrTo[0], size_needed);
    return wstrTo;
}

std::string narrow(const std::wstring& wstr) {
    if (wstr.empty()) return std::string();
    int size_needed = WideCharToMultiByte(CP_UTF8, 0, &wstr[0], (int)wstr.size(), NULL, 0, NULL, NULL);
    std::string strTo(size_needed, 0);
    WideCharToMultiByte(CP_UTF8, 0, &wstr[0], (int)wstr.size(), &strTo[0], size_needed, NULL, NULL);
    return strTo;
}