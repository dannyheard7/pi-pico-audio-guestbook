#pragma once

#include "sd_card.h"
#include <string>
#include <vector>

using std::string;
using std::vector;

class File
{
    public:
        string extension;
        string path;
        string name;
        File(string path);
};

vector<File> list_files_in_folder(sd_card_t *pSD, string folder);