What is this project?

A project that creates a disk catalog by reading MFT directly from all disks simultaneously and writing to the database, has an improved search function and detects duplicate files.

Key features:

- Ultra fast search on all disks at the same time
- Creating a catalog of all disks at ultra fast speed.
- Ultra fast detection of duplicate files by comparing file names and file sizes.
- The project works as Multi thread.

--------------------------------------------------------------------------------------------------------------------

* AI COMMENT ABOUT THE PROJECT *

Hello! I have reviewed the current version of the project. It is a very comprehensive and technical study that indexes the file system by reading the NTFS Master File Table (MFT) and provides searching using the SQLite database.

General Evaluation:

The project uses an advanced technique such as reading the MFT directly for file indexing. This approach can be much faster than traditional file system navigation, especially during the initial scan. Using SQLite and FTS5 (Full-Text Search) is an efficient method for storage and searching operations. The code generally includes modern C++ features (e.g. SafeHandle for RAII, use of standard libraries), error handling and performance optimizations (parallel disk scanning, SQLite PRAGMA settings, prepared statements and transaction usage).

Modules and Functionality:

main.cpp: The main entry point of the program. Manages the user menu, calls functions from other modules (scanning, searching, etc.). Contains locale and console code page settings for Turkish character support. Indicates the need for administrator rights.
file_scanner.cpp / .h: The heart of the project. Contains the logic for reading and parsing MFT.

Finds suitable NTFS drives and runs the scan_mft_for_drive function in parallel for each one.

Directly accesses the disk volume using Windows API (requires admin rights), reads MFT data, parses MFT records (FILE or BAAD signed).
Extracts information such as file name, size, timestamps, MFT references by processing important attributes such as $STANDARD_INFORMATION and $FILE_NAME.
Applies apply_fixup to fix end-of-sector corruptions in MFT records.
Generates full file paths using parent MFT references with the reconstruct_paths function.
Determines the location of the file or MFT on disk by parsing Data Runs in the $DATA attribute.
database_manager.cpp / .h: Manages SQLite database operations.
init_db: Establishes database connection, creates files main table and fts_files virtual FTS5 table for searching. Makes PRAGMA settings for performance (WAL mode, cache size etc.) and defines triggers to keep FTS table up to date. Also creates necessary indexes.

write_to_db: Writes parsed file information to database in bulk (vector<FileData>). Uses transaction and prepared statement for performance. Updates current data using ON CONFLICT DO UPDATE for records with same MFT reference. Uses mutex for thread safety during parallel scan.

close_db: Closes database connection and performs WAL checkpoint.
utils.cpp / .h: Contains auxiliary functions.
Converts FILETIME format to readable date/time string (filetime_to_wstring).
Formats file size as KB/MB/GB (format_filesize).
Extracts MFT reference number from full reference (get_mft_ref_number_only).
Parse MFT Data Runs (parse_data_runs).
MFT fixup application logic (apply_fixup).
Improvements and Enhancements:

Completeness Check:
Missing Modules/Functions: The main.cpp file contains the header files tree_view.h, duplicate_finder.h, and index_manager.h, and calls functions such as display_tree_view(), find_duplicates(), save_index(), load_index(), list_files_sorted(). However, these header files and the related .cpp files (and therefore the implementations of these functions) are not among the files provided to me. This is a significant deficiency for the project to compile and run properly in its current state.
Missing Code: The implementation of the search_files function in the search_engine.cpp file seems to be incomplete. The FTS query has started to be created, but the part about running the query and processing the results is missing.

Code Quality and Design:
Modularity: The project does a good job of separating the functionality into logical modules (scan, database, search, helper functions).
Global Variables: Global variables such as g_db, g_db_mutex, g_total_files_found, g_total_dirs_found are used. In larger projects or teamwork, managing these variables in a class (e.g. a DatabaseManager class or an AppContext class) may be a better design. However, for the current scale of the project, it is acceptable.
Error Management: Many error conditions are checked and logged in the code using std::wcerr (e.g. database errors, file read errors, MFT parsing warnings). This is positive in terms of the robustness of the code.

MFT Complexity: Parsing the MFT is a fairly complex process. The code successfully handles basic attributes, but it is noted that more complex cases such as $ATTRIBUTE_LIST (where a file's information is spread across multiple MFT records) are not handled. This may result in missing information in some files, but it is a reasonable simplification to keep the scope of the project manageable.

Platform Dependency: The code is heavily dependent on Windows APIs (e.g. CreateFileW, DeviceIoControl, FILETIME, SYSTEMTIME) and NTFS structures. This is unavoidable due to the nature of the project.

Conclusion:

The project has built a technically impressive foundation with the goal of providing fast file indexing and searching using the MFT reading technique. It includes features such as parallel scanning, SQLite FTS5 usage, and detailed MFT parsing. However, the project is not fully functional in its current state due to the unfinished code in search_engine.cpp and other modules (tree view, duplicate finder, index manager) that are called in main.cpp but do not have their files/implementations provided. These missing parts need to be completed.

Thanks for leaving the checks to me. I hope this detailed review is useful!

----------------------------------------------------------------------------------------------------

* Proje Hakkında AI Yorumu *

Merhaba! Projenin güncel halini inceledim. NTFS Master File Table (MFT) okuyarak dosya sistemini indeksleyen ve SQLite veritabanı kullanarak arama yapmayı sağlayan oldukça kapsamlı ve teknik bir çalışma olmuş.

Genel Değerlendirme:

Proje, dosya indeksleme için MFT'yi doğrudan okuma gibi ileri seviye bir teknik kullanıyor. Bu yaklaşım, özellikle ilk tarama sırasında geleneksel dosya sistemi gezintisine göre çok daha hızlı olabilir. SQLite ve FTS5 (Full-Text Search) kullanımı, depolama ve arama işlemleri için verimli bir yöntem. Kod genel olarak modern C++ özelliklerini (örneğin, RAII için SafeHandle, standart kütüphane kullanımı), hata yönetimini ve performans optimizasyonlarını (paralel disk tarama, SQLite PRAGMA ayarları, prepared statements ve transaction kullanımı) içeriyor.

Modüller ve İşlevsellik:

main.cpp: Programın ana giriş noktası. Kullanıcı menüsünü yönetir, diğer modüllerden fonksiyonları çağırır (tarama, arama vb.). Türkçe karakter desteği için locale ve konsol kod sayfası ayarlarını içerir. Yönetici hakları gereksinimini belirtir.
file_scanner.cpp / .h: Projenin kalbi. MFT okuma ve ayrıştırma mantığını içerir.
Uygun NTFS sürücülerini bulur ve her biri için paralel olarak scan_mft_for_drive fonksiyonunu çalıştırır.
Windows API kullanarak disk volume'üne doğrudan erişir (yönetici hakkı gerektirir), MFT verisini okur, MFT kayıtlarını (FILE veya BAAD imzalı) ayrıştırır.
$STANDARD_INFORMATION ve $FILE_NAME gibi önemli öznitelikleri işleyerek dosya adı, boyutu, zaman damgaları, MFT referansları gibi bilgileri çıkarır.
MFT kayıtlarındaki sektör sonu bozulmalarını düzeltmek için apply_fixup uygular.
Ebeveyn MFT referanslarını kullanarak tam dosya yollarını reconstruct_paths fonksiyonu ile oluşturur.
$DATA özniteliğindeki Data Run'ları ayrıştırarak dosyanın veya MFT'nin disk üzerindeki yerini belirler.
database_manager.cpp / .h: SQLite veritabanı işlemlerini yönetir.
init_db: Veritabanı bağlantısını kurar, files ana tablosunu ve arama için fts_files sanal FTS5 tablosunu oluşturur. Performans için PRAGMA ayarları (WAL modu, cache boyutu vb.) yapar ve FTS tablosunu güncel tutmak için trigger'lar tanımlar. Ayrıca gerekli indeksleri oluşturur.
write_to_db: Ayrıştırılan dosya bilgilerini toplu olarak (vector<FileData>) veritabanına yazar. Performans için transaction ve prepared statement kullanır. Aynı MFT referansına sahip kayıtlar için ON CONFLICT DO UPDATE kullanarak mevcut veriyi günceller. Paralel tarama sırasında thread güvenliği için mutex kullanır.
close_db: Veritabanı bağlantısını kapatır ve WAL checkpoint yapar.
utils.cpp / .h: Yardımcı fonksiyonları içerir.
FILETIME formatını okunabilir tarih/saat string'ine çevirme (filetime_to_wstring).
Dosya boyutunu KB/MB/GB olarak formatlama (format_filesize).
MFT referans numarasını tam referanstan ayıklama (get_mft_ref_number_only).
MFT Data Run'larını ayrıştırma (parse_data_runs).
MFT fixup uygulama mantığı (apply_fixup).
Geliştirme ve İyileştirmeler:

Tamlık Kontrolü:

Eksik Modüller/Fonksiyonlar: main.cpp dosyası tree_view.h, duplicate_finder.h ve index_manager.h başlık dosyalarını içeriyor ve display_tree_view(), find_duplicates(), save_index(), load_index(), list_files_sorted() gibi fonksiyonları çağırıyor. Ancak bu başlık dosyaları ve ilgili .cpp dosyaları (ve dolayısıyla bu fonksiyonların implementasyonları) bana sunulan dosyalar arasında bulunmuyor. Bu, projenin şu anki haliyle tam olarak derlenip çalışması için önemli bir eksikliktir.
Eksik Kod: search_engine.cpp dosyasındaki search_files fonksiyonunun implementasyonu yarım kalmış görünüyor. FTS sorgusu oluşturulmaya başlanmış ancak sorgunun çalıştırılması ve sonuçların işlenmesi kısmı eksik.

Kod Kalitesi ve Tasarım:

Modülerlik: Proje, işlevselliği mantıksal modüllere (tarama, veritabanı, arama, yardımcı fonksiyonlar) ayırma konusunda iyi bir iş çıkarmış.
Global Değişkenler: g_db, g_db_mutex, g_total_files_found, g_total_dirs_found gibi global değişkenler kullanılmış. Daha büyük projelerde veya ekip çalışmalarında bu değişkenleri bir sınıf içinde yönetmek (örneğin bir DatabaseManager sınıfı veya AppContext sınıfı) daha iyi bir tasarım olabilir. Ancak projenin mevcut ölçeği için kabul edilebilir.
Hata Yönetimi: Kod içerisinde std::wcerr kullanılarak birçok hata durumu kontrol ediliyor ve loglanıyor (örneğin, veritabanı hataları, dosya okuma hataları, MFT ayrıştırma uyarıları). Bu, kodun sağlamlığı açısından olumlu.
MFT Karmaşıklığı: MFT ayrıştırmak oldukça karmaşık bir işlemdir. Kod, temel öznitelikleri başarılı bir şekilde işliyor ancak $ATTRIBUTE_LIST gibi daha karmaşık durumların (bir dosyanın bilgilerinin birden fazla MFT kaydına yayılması) işlenmediği belirtilmiş. Bu, bazı dosyalarda eksik bilgiye yol açabilir ancak projenin kapsamını yönetilebilir tutmak için makul bir basitleştirme olabilir.
Platform Bağımlılığı: Kod, Windows API'lerine (örn. CreateFileW, DeviceIoControl, FILETIME, SYSTEMTIME) ve NTFS yapılarına yoğun bir şekilde bağımlıdır. Bu, projenin doğası gereği kaçınılmazdır.

Sonuç:

Proje, MFT okuma tekniğini kullanarak hızlı dosya indeksleme ve arama sağlama hedefiyle teknik olarak etkileyici bir temel oluşturmuş. Paralel tarama, SQLite FTS5 kullanımı ve detaylı MFT ayrıştırma gibi özellikler içeriyor. Ancak, search_engine.cpp'deki yarım kalmış kod ve main.cpp'de çağrılan ancak dosyaları/implementasyonları sağlanmayan diğer modüller (tree view, duplicate finder, index manager) nedeniyle proje şu anki haliyle tam olarak işlevsel değil. Bu eksik kısımların tamamlanması gerekiyor.

Kontrolleri bana bıraktığın için teşekkürler. Umarım bu detaylı inceleme faydalı olur!
