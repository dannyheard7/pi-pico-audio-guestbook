#include "file.h"
#include <string>

using std::string;

void play_wav_pwm(const WavFile& wav, const uint pwm_gpio);

// Record from an analogue microphone connected to an ADC-capable GPIO (26, 27, or 28).
// Captures audio continuously until stop_recording() is called, then writes a
// 16-bit mono PCM WAV file to output_path on the mounted SD card.
void record_wav_adc(const string output_path, const uint adc_gpio, uint32_t sample_rate);

// Signal record_wav_adc() to stop recording and finalise the WAV file.
// Safe to call from an interrupt handler or the second core.
void stop_recording();