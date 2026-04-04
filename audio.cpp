#include "hardware/pwm.h"
#include "hardware/clocks.h"
#include "hardware/timer.h"
#include "hardware/dma.h"
#include "hardware/irq.h"
#include "hardware/adc.h"
#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "file.h"
#include <cstring> // for memmove / memcpy

// Double buffer system for smooth audio playback
static uint16_t audio_buffer_a[4096];
static uint16_t audio_buffer_b[4096];
static volatile bool buffer_a_ready = false;
static volatile bool buffer_b_ready = false;
static volatile bool playback_active = false;

void setup_pwm(const uint gpio, const uint bitsPerSample, uint32_t sampleRate)
{
    gpio_set_function(gpio, GPIO_FUNC_PWM);
    uint slice_num = pwm_gpio_to_slice_num(gpio);
    
    // Use 12-bit resolution for better audio quality
    // 125MHz / 4096 = ~30.5kHz PWM frequency (above audible range)
    pwm_set_wrap(slice_num, 4095); // 12-bit resolution (0-4095)
    pwm_set_clkdiv(slice_num, 1.0f); // Full speed
    
    // Start at center for 12-bit
    pwm_set_gpio_level(gpio, 2048);
    pwm_set_enabled(slice_num, true);
}

void play_wav_pwm(const WavFile &wav, const uint pwm_gpio)
{
    setup_pwm(pwm_gpio, wav.header.bitsPerSample, wav.header.SamplesPerSec);

    // Debug: print WAV file info
    printf("Playing WAV: %d Hz, %d-bit, %d channels\n", 
           wav.header.SamplesPerSec, wav.header.bitsPerSample, wav.header.NumOfChannels);
    printf("Data size: %d bytes\n", wav.dataSize);

    FIL fil;
    FRESULT fr = f_open(&fil, wav.path.c_str(), FA_READ);
    if (fr != FR_OK)
    {
        printf("ERROR: Could not open file\n");
        return;
    }

    // Seek to audio data start
    f_lseek(&fil, wav.dataStartPosition);

    UINT br;
    uint32_t bytes_per_frame = (wav.header.bitsPerSample / 8) * wav.header.NumOfChannels;
    uint32_t total_frames = wav.dataSize / bytes_per_frame;
    
    // Calculate sample timing - use hardware timer for precise playback
    uint32_t sample_period_us = 1000000 / wav.header.SamplesPerSec;
    
    // Use much larger buffers - 8KB each for double buffering
    const uint32_t BUFFER_SIZE = 8192;
    uint8_t* read_buffer = (uint8_t*)malloc(BUFFER_SIZE);
    if (!read_buffer) {
        printf("ERROR: Could not allocate buffer\n");
        f_close(&fil);
        return;
    }
    
    uint32_t buffer_index = BUFFER_SIZE; // Start empty
    uint32_t bytes_in_buffer = 0;
    
    // Use absolute timing to prevent drift
    uint64_t start_time = time_us_64();
    uint64_t next_sample_time = start_time;
    
    // Pre-fill buffer before starting
    fr = f_read(&fil, read_buffer, BUFFER_SIZE, &br);
    bytes_in_buffer = br;
    buffer_index = 0;
    
    printf("Starting playback...\n");
    
    // Stream audio frames with improved buffering
    for (uint32_t frame = 0; frame < total_frames && fr == FR_OK; frame++)
    {
        // Refill buffer when we're running low (not empty)
        // This gives us time to read without disrupting playback
        if (buffer_index + bytes_per_frame * 512 > bytes_in_buffer && bytes_in_buffer == BUFFER_SIZE) 
        {
            // Copy remaining data to start of buffer
            uint32_t remaining = bytes_in_buffer - buffer_index;
            if (remaining > 0) {
                memmove(read_buffer, read_buffer + buffer_index, remaining);
            }
            
            // Read more data
            fr = f_read(&fil, read_buffer + remaining, BUFFER_SIZE - remaining, &br);
            bytes_in_buffer = remaining + br;
            buffer_index = 0;
        }
        
        if (wav.header.bitsPerSample == 16)
        {
            if (buffer_index + bytes_per_frame <= bytes_in_buffer)
            {
                // Read 16-bit signed sample (little-endian)
                int16_t sample = (int16_t)(read_buffer[buffer_index] | (read_buffer[buffer_index + 1] << 8));
                buffer_index += 2;
                
                // If stereo, average both channels for better quality
                if (wav.header.NumOfChannels == 2) {
                    int16_t sample_r = (int16_t)(read_buffer[buffer_index] | (read_buffer[buffer_index + 1] << 8));
                    buffer_index += 2;
                    sample = (sample + sample_r) / 2; // Average the channels
                }
                
                // Convert 16-bit signed (-32768 to 32767) to 12-bit PWM (0 to 4095)
                uint16_t pwm_value = ((sample >> 4) + 2048) & 0xFFF;
                pwm_set_gpio_level(pwm_gpio, pwm_value);
            }
            else
            {
                // Silence if we run out of data
                pwm_set_gpio_level(pwm_gpio, 2048);
            }
        }
        else if (wav.header.bitsPerSample == 8)
        {
            if (buffer_index < bytes_in_buffer)
            {
                // 8-bit unsigned PCM (0-255)
                uint8_t sample = read_buffer[buffer_index];
                buffer_index++;
                
                // If stereo, average both channels
                if (wav.header.NumOfChannels == 2 && buffer_index < bytes_in_buffer) {
                    uint8_t sample_r = read_buffer[buffer_index];
                    buffer_index++;
                    sample = (sample + sample_r) / 2;
                }
                
                // Convert 8-bit (0-255) to 12-bit (0-4095)
                uint16_t pwm_value = (sample << 4) | (sample >> 4); // Expand to 12-bit
                pwm_set_gpio_level(pwm_gpio, pwm_value);
            }
            else
            {
                // Silence
                pwm_set_gpio_level(pwm_gpio, 2048);
            }
        }
        
        // Calculate next sample time
        next_sample_time = start_time + ((frame + 1) * 1000000ULL / wav.header.SamplesPerSec);
        
        // Wait for the right time - but don't wait if we're already late
        uint64_t current_time = time_us_64();
        if (current_time < next_sample_time) {
            // Only sleep if we have more than 10us to wait
            int64_t wait_time = next_sample_time - current_time;
            if (wait_time > 10) {
                sleep_us(wait_time - 5); // Sleep most of it, then busy-wait
            }
            // Busy-wait for the last few microseconds for precision
            while (time_us_64() < next_sample_time) {
                tight_loop_contents();
            }
        } else if (current_time > next_sample_time + 1000) {
            // We're falling behind significantly - report it
            if (frame % 1000 == 0) {
                printf("Warning: audio timing drift at frame %lu\n", frame);
            }
        }
    }

    // Output center (silence) when done
    pwm_set_gpio_level(pwm_gpio, 2048);
    
    free(read_buffer);
    f_close(&fil);
    
    printf("Playback complete\n");
}

// ---------------------------------------------------------------------------
// Analogue microphone recording via ADC + ping-pong DMA
// ---------------------------------------------------------------------------

// RP2040 ADC runs at 48 MHz; each conversion takes 96 ADC clock cycles.
static const uint32_t ADC_CLOCK_HZ              = 48000000;
static const uint32_t ADC_CYCLES_PER_CONVERSION = 96;

// Two equally-sized capture buffers that DMA alternates between.
// At 16 kHz mono the Pico fills one buffer every ~128 ms, giving the CPU
// ample time to write the other buffer to the SD card.
static const uint32_t RECORD_BUFFER_SAMPLES = 2048;
static uint16_t rec_buffer_a[RECORD_BUFFER_SAMPLES];
static uint16_t rec_buffer_b[RECORD_BUFFER_SAMPLES];

// Scratch buffer used to convert 12-bit unsigned ADC values to 16-bit signed
// PCM before writing to the file (avoids a large stack allocation).
static int16_t rec_convert_buf[RECORD_BUFFER_SAMPLES];

// DMA channel handles (-1 = not claimed).
static int rec_dma_chan_a = -1;
static int rec_dma_chan_b = -1;

// Flags set by the DMA IRQ when each buffer has been filled.
static volatile bool rec_buf_a_ready = false;
static volatile bool rec_buf_b_ready = false;

// Set to false to ask record_wav_adc() to stop.
static volatile bool recording_active = false;

// DMA IRQ handler – fires when either channel completes a buffer.
// Re-arms the completed channel so it is ready for its next turn in the
// ping-pong chain.
static void __isr adc_dma_irq_handler()
{
    if (rec_dma_chan_a >= 0 && (dma_hw->ints0 & (1u << rec_dma_chan_a)))
    {
        dma_hw->ints0 = (1u << rec_dma_chan_a); // clear interrupt
        rec_buf_a_ready = true;
        // Reset write address and transfer count so channel A is ready for
        // its next activation by channel B's chain_to.
        dma_channel_set_write_addr(rec_dma_chan_a, rec_buffer_a, false);
        dma_channel_set_trans_count(rec_dma_chan_a, RECORD_BUFFER_SAMPLES, false);
    }
    if (rec_dma_chan_b >= 0 && (dma_hw->ints0 & (1u << rec_dma_chan_b)))
    {
        dma_hw->ints0 = (1u << rec_dma_chan_b);
        rec_buf_b_ready = true;
        dma_channel_set_write_addr(rec_dma_chan_b, rec_buffer_b, false);
        dma_channel_set_trans_count(rec_dma_chan_b, RECORD_BUFFER_SAMPLES, false);
    }
}

// Convert a buffer of 12-bit unsigned ADC samples to 16-bit signed PCM and
// append them to the open WAV file.  Updates *data_size with bytes written.
static void write_adc_samples(FIL *file, const uint16_t *samples,
                               uint32_t count, uint32_t *data_size)
{
    for (uint32_t i = 0; i < count; i++)
    {
        // ADC result is 12-bit right-aligned (bits 11:0), value 0-4095.
        // Centre at 2048, then scale to signed 16-bit PCM:
        //   range: (0-2048)*16 = -32768  to  (4095-2048)*16 = 32752
        // Both extremes fit within int16_t (-32768 to 32767).
        rec_convert_buf[i] = (int16_t)(((int32_t)(samples[i] & 0x0FFF) - 2048) * 16);
    }
    UINT bw;
    f_write(file, rec_convert_buf, count * sizeof(int16_t), &bw);
    *data_size += bw;
}

void stop_recording()
{
    recording_active = false;
}

void record_wav_adc(const sd_card_t *pSD, const string output_path,
                    const uint adc_gpio, uint32_t sample_rate)
{
    printf("ADC recording: GPIO %u, %lu Hz -> %s\n",
           adc_gpio, (unsigned long)sample_rate, output_path.c_str());

    // ------------------------------------------------------------------
    // ADC setup
    // adc_gpio must be one of GPIO 26 (ADC0), 27 (ADC1), or 28 (ADC2).
    // ------------------------------------------------------------------
    adc_init();
    adc_gpio_init(adc_gpio);
    adc_select_input(adc_gpio - 26u);

    // Enable the FIFO and wire it to DMA.  No error bit, no 8-bit shift
    // (keep full 12-bit result right-aligned in a 16-bit word).
    adc_fifo_setup(true, true, 1, false, false);

    // Clock divider: clkdiv = (ADC_CLOCK_HZ / (sample_rate * ADC_CYCLES_PER_CONVERSION)) - 1
    float clkdiv = (float)ADC_CLOCK_HZ /
                   ((float)sample_rate * (float)ADC_CYCLES_PER_CONVERSION) - 1.0f;
    if (clkdiv < 0.0f) clkdiv = 0.0f;
    adc_set_clkdiv(clkdiv);

    // ------------------------------------------------------------------
    // Ping-pong DMA setup (channels A and B chain to each other)
    // ------------------------------------------------------------------
    rec_dma_chan_a = dma_claim_unused_channel(true);
    rec_dma_chan_b = dma_claim_unused_channel(true);

    // Channel A: fills rec_buffer_a, then triggers channel B
    dma_channel_config cfg_a = dma_channel_get_default_config(rec_dma_chan_a);
    channel_config_set_transfer_data_size(&cfg_a, DMA_SIZE_16);
    channel_config_set_read_increment(&cfg_a, false);  // ADC FIFO is a fixed address
    channel_config_set_write_increment(&cfg_a, true);
    channel_config_set_dreq(&cfg_a, DREQ_ADC);
    channel_config_set_chain_to(&cfg_a, rec_dma_chan_b);

    // Channel B: fills rec_buffer_b, then triggers channel A
    dma_channel_config cfg_b = dma_channel_get_default_config(rec_dma_chan_b);
    channel_config_set_transfer_data_size(&cfg_b, DMA_SIZE_16);
    channel_config_set_read_increment(&cfg_b, false);
    channel_config_set_write_increment(&cfg_b, true);
    channel_config_set_dreq(&cfg_b, DREQ_ADC);
    channel_config_set_chain_to(&cfg_b, rec_dma_chan_a);

    // Enable DMA IRQ0 for both channels and register the handler
    dma_channel_set_irq0_enabled(rec_dma_chan_a, true);
    dma_channel_set_irq0_enabled(rec_dma_chan_b, true);
    irq_set_exclusive_handler(DMA_IRQ_0, adc_dma_irq_handler);
    irq_set_enabled(DMA_IRQ_0, true);

    // Pre-configure both channels (do not start yet)
    dma_channel_configure(rec_dma_chan_a, &cfg_a,
                          rec_buffer_a, &adc_hw->fifo,
                          RECORD_BUFFER_SAMPLES, false);
    dma_channel_configure(rec_dma_chan_b, &cfg_b,
                          rec_buffer_b, &adc_hw->fifo,
                          RECORD_BUFFER_SAMPLES, false);

    // ------------------------------------------------------------------
    // Create output WAV file and write placeholder header
    // ------------------------------------------------------------------
    FIL file;
    FRESULT fr = f_open(&file, output_path.c_str(), FA_WRITE | FA_CREATE_ALWAYS);
    if (fr != FR_OK)
    {
        printf("ERROR: Could not create recording file: %s (%d)\n",
               output_path.c_str(), fr);
        irq_set_enabled(DMA_IRQ_0, false);
        dma_channel_set_irq0_enabled(rec_dma_chan_a, false);
        dma_channel_set_irq0_enabled(rec_dma_chan_b, false);
        dma_channel_unclaim(rec_dma_chan_a);
        dma_channel_unclaim(rec_dma_chan_b);
        rec_dma_chan_a = rec_dma_chan_b = -1;
        return;
    }

    const uint16_t NUM_CHANNELS  = 1;
    const uint16_t BITS_PER_SAMPLE = 16;

    WAV_HEADER header;
    memcpy(header.RIFF, "RIFF", 4);
    header.ChunkSize     = 0; // filled in at the end
    memcpy(header.WAVE, "WAVE", 4);
    memcpy(header.fmt,  "fmt ", 4);
    header.Subchunk1Size = 16;
    header.AudioFormat   = 1;  // PCM
    header.NumOfChannels = NUM_CHANNELS;
    header.SamplesPerSec = sample_rate;
    header.blockAlign    = NUM_CHANNELS * BITS_PER_SAMPLE / 8;
    header.bytesPerSec   = sample_rate * header.blockAlign;
    header.bitsPerSample = BITS_PER_SAMPLE;

    UINT bw;
    f_write(&file, &header, sizeof(WAV_HEADER), &bw);
    // "data" sub-chunk ID + placeholder size
    f_write(&file, "data", 4, &bw);
    uint32_t data_size = 0;
    f_write(&file, &data_size, sizeof(data_size), &bw);

    // ------------------------------------------------------------------
    // Start recording loop
    // ------------------------------------------------------------------
    rec_buf_a_ready  = false;
    rec_buf_b_ready  = false;
    recording_active = true;

    dma_channel_start(rec_dma_chan_a); // B will be chained automatically
    adc_run(true);

    printf("Recording started. Call stop_recording() to stop.\n");

    while (recording_active)
    {
        if (rec_buf_a_ready)
        {
            rec_buf_a_ready = false;
            write_adc_samples(&file, rec_buffer_a, RECORD_BUFFER_SAMPLES, &data_size);
        }
        if (rec_buf_b_ready)
        {
            rec_buf_b_ready = false;
            write_adc_samples(&file, rec_buffer_b, RECORD_BUFFER_SAMPLES, &data_size);
        }
        tight_loop_contents();
    }

    // ------------------------------------------------------------------
    // Stop ADC and DMA, release resources
    // ------------------------------------------------------------------
    adc_run(false);
    adc_fifo_drain();
    dma_channel_abort(rec_dma_chan_a);
    dma_channel_abort(rec_dma_chan_b);

    irq_set_enabled(DMA_IRQ_0, false);
    dma_channel_set_irq0_enabled(rec_dma_chan_a, false);
    dma_channel_set_irq0_enabled(rec_dma_chan_b, false);
    dma_channel_unclaim(rec_dma_chan_a);
    dma_channel_unclaim(rec_dma_chan_b);
    rec_dma_chan_a = rec_dma_chan_b = -1;

    // ------------------------------------------------------------------
    // Patch the WAV header with the final sizes and close the file
    // ------------------------------------------------------------------
    // ChunkSize = 4 ("WAVE") + 8 (fmt chunk header) + 16 (fmt data)
    //           + 8 ("data" chunk header) + data_size
    //           = 36 + data_size
    header.ChunkSize = 36 + data_size;
    f_lseek(&file, 0);
    f_write(&file, &header, sizeof(WAV_HEADER), &bw);

    // Patch the data sub-chunk size field (4 bytes after the "data" tag)
    f_lseek(&file, sizeof(WAV_HEADER) + 4);
    f_write(&file, &data_size, sizeof(data_size), &bw);

    f_close(&file);

    printf("Recording saved: %lu bytes of audio data\n", (unsigned long)data_size);
}