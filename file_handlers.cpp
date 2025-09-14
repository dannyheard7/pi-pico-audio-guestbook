#include <stdio.h>
#include <cstring>
#include <string>
#include <vector>
#include <memory>
#include "sd_card.h"
#include "file.h"
#include "file_handlers.h"

using std::string;
using std::unique_ptr;
using std::vector;

bool string_starts_with(const string str, const string prefix)
{
    if (str.size() == 0 || prefix.size() == 0)
    {
        return false;
    }

    return str.compare(0, prefix.size(), prefix) == 0;
}

bool string_ends_with(const string str, const string suffix)
{
    if (str.size() == 0 || suffix.size() == 0)
    {
        return false;
    }

    return str.substr(str.size() - suffix.size()) == suffix;
}

vector<unique_ptr<File>> list_files_in_folder(
    const sd_card_t *pSD,
    const string folder,
    const unique_ptr<FileType> typeFilter)
{
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

    DIR dir;
    FRESULT res = f_opendir(&dir, fullPath.c_str());

    if (res == FR_NO_PATH)
    {
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

        if (typeFilter == NULL || file.get()->getType() == *typeFilter || (*typeFilter == FileType::Generic))
        {
            files.push_back(std::move(file));
        }
    }

    if (f_closedir(&dir) != FR_OK)
    {
        printf("Failed to close directory: %s\n", fullPath);
    }

    return files;
}

int write_file(const sd_card_t *pSD, const string path, const string content)
{
    FRESULT fr;
    FIL fil;

    // Open file for writing ()
    fr = f_open(&fil, path.c_str(), FA_WRITE | FA_CREATE_ALWAYS);
    if (fr != FR_OK)
    {
        printf("ERROR: Could not open file (%d)\r\n", fr);
        return -1;
    }

    // Write something to file
    int ret = f_printf(&fil, content.c_str());
    if (ret < 0)
    {
        printf("ERROR: Could not write to file (%d)\r\n", ret);
        f_close(&fil);
        return -1;
    }

    fr = f_close(&fil);
    if (fr != FR_OK)
    {
        printf("ERROR: Could not close file (%d)\r\n", fr);
        return -1;
    }

    return 0;
}