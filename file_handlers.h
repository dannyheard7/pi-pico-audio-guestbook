#pragma once

#include <string>
#include <vector>
#include <memory>
#include "sd_card.h"
#include "file.h"

using std::string;
using std::vector;
using std::unique_ptr;

vector<unique_ptr<File>> list_files_in_folder(const sd_card_t *pSD, const string folder, const unique_ptr<FileType> typeFilter = NULL);

int write_file(const sd_card_t *pSD, const string path, const string content);