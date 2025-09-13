#include <stdio.h>
#include <cstring>
#include <string>
#include <vector>
#include <memory>
#include "sd_card.h"
#include "file.h"
#include "file_handlers.h"


using std::string;
using std::vector;
using std::unique_ptr;

bool string_starts_with(string str, string prefix)
{
    if (str.size() == 0 || prefix.size() == 0)
    {
        return false;
    }

    return str.compare(0, prefix.size(), prefix) == 0;
}

bool string_ends_with(string str, string suffix)
{
    if (str.size() == 0 || suffix.size() == 0)
    {
        return false;
    }

    return str.substr(str.size() - suffix.size()) == suffix;
}


vector<unique_ptr<File>> list_files_in_folder(
    sd_card_t *pSD,
    string folder,
    unique_ptr<FileType> typeFilter)
{
    DIR dir;

    string fullPath;

    if (!string_starts_with(folder, pSD->pcName))
    {
        if (!string_starts_with(folder, DIRECTORY_SEPARATOR))
        {
            fullPath = pSD->pcName + DIRECTORY_SEPARATOR + folder;
        }
        else
        {
            fullPath = pSD->pcName + folder;
        }
    }
    else
    {
        fullPath = folder;
    }

    FRESULT res = f_opendir(&dir, fullPath.c_str());

    if (res == FR_NO_PATH)
    {
        // Create
        printf("No Directory: %s\n", fullPath);
        return {};
    }
    else if (res != FR_OK)
    {
        printf("Failed to open directory: %s\n", fullPath);
        return {};
    }

    vector<unique_ptr<File>> files = {};
    FILINFO finfo;
    while (f_readdir(&dir, &finfo) == FR_OK && finfo.fname[0] != 0)
    {
        string fullFilePath = fullPath + DIRECTORY_SEPARATOR + finfo.fname;
        unique_ptr<File> file = File::Create(fullFilePath);

        if(typeFilter == NULL || file.get()->getType() == *typeFilter || (*typeFilter == FileType::Generic)) 
        {
            files.push_back(std::move(file));
        }
    }

    if (f_closedir(&dir) != FR_OK)
    {
        printf("Failed to close directory: %s\n", fullPath);
        return files;
    }

    return files;
}