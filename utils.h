#pragma once
#include <windows.h>
#include <sqlite3.h>
#include <vector>
#include <string>
#include <map>
#include <mutex>
#include <thread>
#include <chrono>
#include <cstdint>
#include <filesystem>

// Unicode karakter kodlaması için yardımcı fonksiyonlar
std::wstring filetime_to_wstring(uint64_t ft_ul);
std::wstring format_filesize(uint64_t size);
uint64_t get_mft_ref_number_only(uint64_t full_ref);
bool apply_fixup(BYTE* buffer, const MFT_RECORD_HEADER* header);
std::vector<std::pair<uint64_t, uint64_t>> parse_data_runs(const BYTE* run_list_start, ULONG run_list_length, uint64_t& total_clusters);