#pragma once
#include <sqlite3.h>
#include <vector>
#include <string>
#include <mutex>
#include <cstdint>
#include <map>

// Veritabanı bağlantısı (global - basitlik için, daha iyi tasarımlarda sınıf üyesi olabilir)
extern sqlite3* g_db;
// Veritabanı işlemleri için mutex (paralel tarama durumunda)
extern std::mutex g_db_mutex;

// Veritabanını başlatır ve tabloları oluşturur
bool init_db();

// Veritabanı bağlantısını kapatır
void close_db();

// Toplu halde FileData nesnelerini veritabanına yazar
// Hata durumunda rollback yapar.
// NOT: Bu fonksiyon FTS tetikleyicilerine güvenir.
bool write_to_db(const std::vector<FileData>& file_entries);