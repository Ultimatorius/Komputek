#pragma once

#include "database_manager.h"
#include <string>

void save_index(DatabaseManager& db_manager, const std::string& backup_file_path = DEFAULT_BACKUP_FILENAME);
void load_index(DatabaseManager& db_manager, const std::string& backup_file_path = DEFAULT_BACKUP_FILENAME);