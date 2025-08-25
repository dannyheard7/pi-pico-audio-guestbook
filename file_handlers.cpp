#include <stdio.h>
#include "sd_card.h"
#include "error_handling.h"
#include "file_handlers.h"
#include <cstring>
#include <string>
#include <vector>

using std::string;
using std::vector;

const string SEPARATOR = "/";

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

string getFileName(string path)
{
    size_t lastSeperatorIndex = path.find_last_of(SEPARATOR);
    if (lastSeperatorIndex != string::npos)
    {
        return path.substr(lastSeperatorIndex + 1);
    }

    return path;
}

string getFileExtension(string path)
{
    size_t dotIndex = path.find_last_of(".");

    if (dotIndex != string::npos)
    {
        return path.substr(dotIndex + 1);
    }

    return "";
}

vector<File> list_files_in_folder(sd_card_t *pSD, string folder)
{
    DIR dir;

    string fullPath;

    if (!string_starts_with(folder, pSD->pcName))
    {
        if (!string_starts_with(folder, SEPARATOR))
        {
            fullPath = pSD->pcName + SEPARATOR + folder;
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
        flashErrorLED();
        return {};
    }

    vector<File> files;
    FILINFO finfo;
    while (f_readdir(&dir, &finfo) == FR_OK && finfo.fname[0] != 0)
    {
        printf("Found file: %s\n", finfo.fname);
    }

    if (f_closedir(&dir) != FR_OK)
    {
        printf("Failed to close directory: %s\n", fullPath);
        flashErrorLED();
        return files;
    }

    return files;
}

File::File(string path)
{
    this->path = path;
    this->name = getFileName(path);
    this->extension = getFileExtension(path);
}
