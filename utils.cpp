#include "utils.h"

// FILETIME'ı okunabilir bir string'e çevirir (YYYY-MM-DD HH:MM:SS)
std::wstring filetime_to_wstring(uint64_t ft_ul) {
    if (ft_ul == 0) {
        return L""; // Geçersiz veya sıfır zaman damgası
    }
    FILETIME ft = *reinterpret_cast<FILETIME*>(&ft_ul);
    SYSTEMTIME st_utc, st_local;
    if (!FileTimeToSystemTime(&ft, &st_utc)) {
        return L"<error>";
    }
    if (!SystemTimeToTzSpecificLocalTime(nullptr, &st_utc, &st_local)) {
         // Başarısız olursa UTC kullan
        st_local = st_utc;
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

// Bayt cinsinden boyutu okunabilir bir string'e çevirir (KB, MB, GB)
std::wstring format_filesize(uint64_t size) {
    const double kb = 1024.0;
    const double mb = 1024.0 * 1024.0;
    const double gb = 1024.0 * 1024.0 * 1024.0;
    std::wstringstream ss;
    ss << std::fixed << std::setprecision(1); // Virgülden sonra 1 basamak

    if (size >= gb) {
        ss << (size / gb) << L" GB";
    } else if (size >= mb) {
        ss << (size / mb) << L" MB";
    } else if (size >= kb) {
        ss << (size / kb) << L" KB";
    } else {
        ss << size << L" B";
    }
    return ss.str();
}

// MFT Referans Numarasını ayrıştırır (Sequence Number hariç)
uint64_t get_mft_ref_number_only(uint64_t full_ref) {
    return full_ref & 0x0000FFFFFFFFFFFF;
}

// MFT Kaydındaki Fixup Array'i uygular (sektör sonu bozulmalarını düzeltir)
// Buffer, MFT_RECORD_SIZE boyutunda olmalıdır.
bool apply_fixup(BYTE* buffer, const MFT_RECORD_HEADER* header) {
    if (!header || header->FixupOffset == 0 || header->FixupCount == 0 || header->FixupOffset >= MFT_RECORD_SIZE - sizeof(USHORT)) {
        // Geçersiz fixup bilgisi veya ofset
        return false;
    }

    // Fixup array'deki ilk değer (signature), sonraki değerler değiştirilecek sektör sonlarıdır
    USHORT* fixup_array = reinterpret_cast<USHORT*>(buffer + header->FixupOffset);
    USHORT fixup_signature = fixup_array[0];

    // FixupCount, imza dahil toplam USHORT sayısıdır.
    // Her 512 byte'lık sektör sonundaki USHORT değeri kontrol edilir.
    for (USHORT i = 1; i < header->FixupCount; ++i) {
        // Kontrol edilecek pozisyon (sektör sonu - 2 byte)
        DWORD position = (i * 512) - sizeof(USHORT);
        if (position >= MFT_RECORD_SIZE) {
             std::wcerr << L"Uyarı: Fixup pozisyonu MFT kayıt boyutunu aşıyor (" << position << L")." << std::endl;
            continue; // Bu pozisyonu atla
        }

        USHORT* value_at_position = reinterpret_cast<USHORT*>(buffer + position);

        // Değerin fixup imzasıyla eşleşip eşleşmediğini kontrol et
        if (*value_at_position != fixup_signature) {
            std::wcerr << L"Hata: Fixup imzası eşleşmedi! Kayıt bozuk olabilir. Pozisyon: " << position
                       << L", Beklenen: " << fixup_signature << L", Bulunan: " << *value_at_position << std::endl;
            return false; // Eşleşmiyorsa, kayıt muhtemelen bozuktur
        }

        // Eşleşiyorsa, fixup array'deki orijinal değerle değiştir
        *value_at_position = fixup_array[i];
    }
    return true;
}

// Data Run'ları ayrıştırarak cluster listesini ve toplam boyutu döndürür
// Bu fonksiyon basit bir implementasyondur ve negatif ofsetleri tam işlemez.
// Tam implementasyon daha karmaşıktır.
std::vector<std::pair<uint64_t, uint64_t>> parse_data_runs(const BYTE* run_list_start, ULONG run_list_length, uint64_t& total_clusters) {
    std::vector<std::pair<uint64_t, uint64_t>> cluster_runs; // {start_cluster, num_clusters}
    const BYTE* p = run_list_start;
    const BYTE* end = run_list_start + run_list_length;
    uint64_t current_lcn = 0; // Mevcut Logical Cluster Number

    while (p < end && *p != 0x00) { // 0x00 run listesinin sonudur
        BYTE header = *p++;
        if (p > end) break; // Tampon sonunu kontrol et

        int length_bytes = header & 0x0F;
        int offset_bytes = (header >> 4) & 0x0F;

        if (p + length_bytes + offset_bytes > end) {
            std::wcerr << L"Hata: Data Run ayrıştırılırken tampon sonuna ulaşıldı." << std::endl;
            break; // Tampon sonu aşıldı
        }

        // Run Uzunluğunu (cluster sayısı) oku
        uint64_t run_length = 0;
        memcpy(&run_length, p, min(length_bytes, sizeof(run_length)));
        p += length_bytes;

        // Run Ofsetini (öncekine göre LCN farkı) oku
        int64_t run_offset = 0;
        if (offset_bytes > 0) {
            // İşaretli ofset okuma (son byte'ın en yüksek biti işareti belirler)
             BYTE sign_byte = *(p + offset_bytes - 1);
             bool is_negative = (sign_byte & 0x80) != 0;

             memcpy(&run_offset, p, min(offset_bytes, sizeof(run_offset)));

             p += offset_bytes;
        }

        // Yeni LCN'i hesapla
        current_lcn = static_cast<uint64_t>(static_cast<int64_t>(current_lcn) + run_offset);

        if (run_length > 0) {
            cluster_runs.push_back({current_lcn, run_length});
            total_clusters += run_length;
        }
    }
    return cluster_runs;
}