#pragma once

#include "sd_card.h"
#include <string>
#include <memory>

using std::string;
using std::unique_ptr;

const string DIRECTORY_SEPARATOR = "/";

enum class FileType { Generic, Wav };

class File
{
    public:
        virtual FileType getType() const { return FileType::Generic; }
        File(const string path);  
        string extension;
        string path;
        string name;
        static unique_ptr<File> Create(const string path);
};

struct  WAV_HEADER
{
    /* RIFF Chunk Descriptor */
    char         RIFF[4];        // RIFF Header Magic header
    uint32_t        ChunkSize;      // RIFF Chunk Size
    char         WAVE[4];        // WAVE Header
    /* "fmt" sub-chunk */
    char         fmt[4];         // FMT header
    uint32_t        Subchunk1Size;  // Size of the fmt chunk
    uint16_t        AudioFormat;    // Audio format 1=PCM,6=mulaw,7=alaw,     257=IBM Mu-Law, 258=IBM A-Law, 259=ADPCM
    uint16_t        NumOfChannels;      // Number of channels 1=Mono 2=Sterio
    uint32_t        SamplesPerSec;  // Sampling Frequency in Hz
    uint32_t        bytesPerSec;    // bytes per second
    uint16_t        blockAlign;     // 2=16-bit mono, 4=16-bit stereo
    uint16_t        bitsPerSample;  // Number of bits per sample
};

class WavFile : public File {
    public:
        WavFile(const string path, const WAV_HEADER header, const uint32_t dataStartPosition, const uint32_t dataSize);
        WAV_HEADER header;
        uint32_t dataStartPosition;
        uint32_t dataSize;
        virtual FileType getType() const override { return FileType::Wav; }
};
