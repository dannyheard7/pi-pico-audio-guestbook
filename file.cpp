#include <stdio.h>
#include <cstring>
#include <string>
#include <memory>
#include "file.h"

using std::make_unique;
using std::string;
using std::unique_ptr;

string getFileName(const string path)
{
    size_t lastSeperatorIndex = path.find_last_of(DIRECTORY_SEPARATOR);
    if (lastSeperatorIndex != string::npos)
    {
        return path.substr(lastSeperatorIndex + 1);
    }

    return path;
}

string getFileExtension(const string path)
{
    size_t dotIndex = path.find_last_of(".");

    if (dotIndex != string::npos)
    {
        return path.substr(dotIndex + 1);
    }

    return "";
}

File::File(const string path)
{
    this->path = path;
    this->name = getFileName(path);
    this->extension = getFileExtension(path);
}

WavFile::WavFile(const string path, const WAV_HEADER header, const uint32_t dataStartPosition, const uint32_t dataSize) : File(path)
{
    this->header = header;
    this->dataStartPosition = dataStartPosition;
    this->dataSize = dataSize;
}


unique_ptr<File> File::Create(const string path)
{
    string extension = getFileExtension(path);

    if (extension == "wav")
    {
        FIL file;
        if (f_open(&file, path.c_str(), FA_READ) != FR_OK)
        {
            printf("Failed to open file: %s\n", path.c_str());
            return make_unique<File>(path);
        }

        WAV_HEADER header;
        UINT bytesRead;
        if (f_read(&file, &header, sizeof(WAV_HEADER), &bytesRead) != FR_OK || bytesRead != sizeof(WAV_HEADER))
        {
            printf("Failed to read WAV header: %s\n", path.c_str());
            f_close(&file);
            return make_unique<File>(path);
        }

        char chunkId[4];
        uint32_t chunkSize;
        FSIZE_t currentPosition = bytesRead;
        while (true)
        {
            // clear previous values
            memset(chunkId, 0, sizeof(chunkId));
            chunkSize = 0;

            // Read chunk ID
            if (f_read(&file, chunkId, 4, &bytesRead) != FR_OK || bytesRead != 4)
                break;

            if (f_read(&file, &chunkSize, 4, &bytesRead) != FR_OK || bytesRead != 4)
                break;

            if (strncmp(chunkId, "data", 4) == 0)
            {
                currentPosition += 8;
                // Found the data chunk!
                break;
            }
            else
            {
                // Skip this chunk
                currentPosition += 8 + chunkSize;
                if (currentPosition > f_size(&file))
                    break;
                f_lseek(&file, currentPosition);
            }
        }

        if (f_close(&file) != FR_OK)
        {
            printf("Failed to close WAV file: %s\n", path.c_str());
        }

        if (strncmp(chunkId, "data", 4) != 0 || chunkSize == 0)
        {
            printf("Invalid WAV file format: %s\n", path.c_str());
            return make_unique<File>(path);
        }



        return make_unique<WavFile>(path, header, currentPosition, chunkSize);
    }

    return make_unique<File>(path);
}