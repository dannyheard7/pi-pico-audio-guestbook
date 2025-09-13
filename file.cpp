#include <stdio.h>
#include <cstring>
#include <string>
#include <memory>
#include "file.h"

using std::string;
using std::unique_ptr;
using std::make_unique;

string getFileName(string path)
{
    size_t lastSeperatorIndex = path.find_last_of(DIRECTORY_SEPARATOR);
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

File::File(string path)
{
    this->path = path;
    this->name = getFileName(path);
    this->extension = getFileExtension(path);
}

WavFile::WavFile(string path, WAV_HEADER header) : File(path) {
    this->header = header;
}


unique_ptr<File> File::Create(string path)
{
    string extension = getFileExtension(path);

    if (extension == "wav")
    {
        FIL file;
        FRESULT res = f_open(&file, path.c_str(), FA_READ);
        if (res != FR_OK)
        {
            printf("Failed to open file: %s\n", path.c_str());
            return make_unique<File>(path);
        }

        WAV_HEADER header;
        UINT bytesRead;
        res = f_read(&file, &header, sizeof(WAV_HEADER), &bytesRead);
        if (res != FR_OK || bytesRead != sizeof(WAV_HEADER))
        {
            printf("Failed to read WAV header: %s\n", path.c_str());
            f_close(&file);
            return make_unique<File>(path);
        }

        if (f_close(&file) != FR_OK)
        {
            printf("Failed to close WAV file: %s\n", path.c_str());
        }

        if (strncmp(header.RIFF, "RIFF", 4) != 0 || strncmp(header.WAVE, "WAVE", 4) != 0 ||
            strncmp(header.fmt, "fmt ", 4) != 0 || strncmp(header.Data, "data", 4) != 0)
        {
            printf("Invalid WAV file format: %s\n", path.c_str());
            return make_unique<File>(path);
        }

        return make_unique<WavFile>(path, header);
    }

    return make_unique<File>(path);
}

