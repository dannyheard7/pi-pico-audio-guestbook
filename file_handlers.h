#pragma once

#include "sd_card.h"
#include "file.h"
#include <string>
#include <vector>
#include <memory>

using std::string;
using std::vector;
using std::unique_ptr;

vector<unique_ptr<File>> list_files_in_folder(sd_card_t *pSD, string folder, unique_ptr<FileType> typeFilter = NULL);