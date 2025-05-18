#pragma once

#include "common.h" // MFT_RECORD_HEADER vb. için file_scanner.h'a taşınabilir
#include "file_scanner.h" // MFT_RECORD_HEADER için (veya bunu common.h'a taşı)
#include <windows.h>
#include <string>
#include <vector>
#include <cstdint>
#include <iomanip> // std::setfill, std::setw için
#include <sstream> // std::wstringstream için

// Yardımcı fonksiyonlar
std::wstring filetime_to_wstring(uint64_t ft_ul);
std::wstring format_filesize(uint64_t size);
uint64_t get_mft_ref_number_only(uint64_t full_ref); // MFT Referansından sequence no ayıkla

// MFT Kaydındaki Fixup Array'i uygular
bool apply_fixup(BYTE* buffer, const MFT_RECORD_HEADER* header);

// Data Run'ları ayrıştırır
std::vector<std::pair<uint64_t, uint64_t>> parse_data_runs(
    const BYTE* run_list_start,
    ULONG run_list_length,
    uint64_t& total_clusters_in_runs
);

// std::string'i std::wstring'e çevir (basit implementasyon)
std::wstring widen(const std::string& str);
// std::wstring'i std::string'e çevir (basit implementasyon, veri kaybı olabilir)
std::string narrow(const std::wstring& wstr);