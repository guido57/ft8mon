//
// decode FT8 from a sound card
//
// Robert Morris, AB1HL
//

// ---------------------------------------------------------
// ESP32 PSRAM GLOBAL OVERRIDE
// ---------------------------------------------------------
#ifdef ESP32
#include <esp_heap_caps.h>
#include <new>
#include <Arduino.h>

void* operator new(size_t size) {
    void* p = heap_caps_malloc(size, MALLOC_CAP_SPIRAM);

    if(p){
      printf("Allocated %d bytes in PSRAM at %p. Remaining: %d bytes\n", size, p,
            heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    }

    if (!p) {
        p = malloc(size); // Fallback to internal
    }

    if (!p) {
        Serial.printf("FATAL OOM: Failed to allocate %d bytes.\n", size);
        Serial.printf("Internal Free: %d bytes (Total: %d)\n", 
            heap_caps_get_free_size(MALLOC_CAP_INTERNAL), 
            heap_caps_get_total_size(MALLOC_CAP_INTERNAL));
        Serial.printf("PSRAM Free:    %d bytes (Total: %d)\n", 
            heap_caps_get_free_size(MALLOC_CAP_SPIRAM), 
            heap_caps_get_total_size(MALLOC_CAP_SPIRAM));
        Serial.flush();
        throw std::bad_alloc();
    }


    return p;
}

void operator delete(void* p) noexcept {
    heap_caps_free(p);
}

#else

#include <new>
#include <cstdio>
#include <cstdlib>

void* operator new(size_t size) {

    void* p = std::malloc(size);

    if (!p) {
        throw std::bad_alloc();
    }

    if(size > 1000000) {
        // printf("operator new: large allocation: %zu bytes at %p\n", size, p);
    } else {
        // printf("Linux allocation: %zu bytes at %p\n", size, p);
    }
    // printf("Linux allocation: %zu bytes at %p\n", size, p);

    return p;
}


void operator delete(void* p) noexcept {
    //printf("Linux free large allocation: %p\n", p);
    std::free(p);
}


// Needed since C++14
void* operator new[](size_t size) {

    void* p = std::malloc(size);

    if (!p) {
        throw std::bad_alloc();
    }

    printf("Linux array allocation: %zu bytes at %p\n", size, p);

    return p;
}


void operator delete[](void* p) noexcept {
    printf("Linux array free: %p\n", p);
    std::free(p);
}


#endif

#ifdef ESP32
#include <Arduino.h>
#include <FS.h>
#include <LittleFS.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <assert.h>
#include <vector>
#include <time.h>
#include <string.h>
#include <mutex>
#include <map>
#include <string>

// Conditionally include thread for Linux
#ifndef ESP32
#include <thread>
#endif

#include "util.h"
#include "unpack.h"
#include "ft8.h"
#include "fft.h"

std::mutex cycle_mu;
volatile int cycle_count = 0;
time_t saved_cycle_start;
std::map<std::string,bool> cycle_already;

double start_now;
int count = 1;

//
// a91 is 91 bits -- 77 plus the 14-bit CRC.
//
int
hcb(int *a91, double hz0, double hz1, double off,
    const char *comment, double snr, int pass,
    int correct_bits)
{
  std::string msg = unpack(a91);

  cycle_mu.lock();

  if(cycle_already.count(msg) > 0){
    cycle_already[msg] = true;
    cycle_count += 1;
    cycle_mu.unlock();
    return 1; // 1 => already seen, don't subtract.
  }

  cycle_already[msg] = true;
  cycle_count += 1;

  cycle_mu.unlock();

  struct tm result;
  gmtime_r(&saved_cycle_start, &result);

  char output_buf[256];
  snprintf(output_buf, sizeof(output_buf), "%.3f %d %02d%02d%02d %3d %3d %5.2f %6.1f %s\n",
         now() - start_now,
         count++,
         result.tm_hour,
         result.tm_min,
         result.tm_sec,
         (int)snr,
         correct_bits,
         off - 0.5f,
         hz0,
         msg.c_str());

#ifdef ESP32
  // Print to ESP32 Serial monitor
  Serial.print(output_buf);
#else
  // Print to Linux stdout
  printf("%s", output_buf);
  fflush(stdout);
#endif
  
  return 2; // 2 => new decode, do subtract.
}

//
// Shared decoding logic to avoid duplicating code between Linux and ESP32
//
void run_decoder(const std::vector<int16_t> &s16, int rate) {
  int hints[2] = { 2, 0 }; // CQ
  double budget = 5;

  extern int nthreads;
  nthreads = 1;

  cycle_mu.lock();
  cycle_count = 0;
  saved_cycle_start = time(nullptr);
  cycle_already.clear();
  cycle_mu.unlock();

  // Use float, because entry() and the FFT engine explicitly require float*
  std::vector<float> s(s16.size());
  
  // Convert int16_t directly to float
  for(size_t i = 0; i < s16.size(); i++) {
    s[i] = (float)s16[i] / 32768.0f;
  }
  
  entry(s.data(), s.size(), 0.5 * rate, rate,
        150,
        3600, 
        hints, hints, budget, budget, hcb
      //  0, (struct cdecode *) 0
      );
}
// ---------------------------------------------------------
// ESP32 SPECIFIC IMPLEMENTATION
// ---------------------------------------------------------
#ifdef ESP32

#include <Arduino.h>
#include <FS.h>
#include <LittleFS.h>

void setup() {
  Serial.begin(115200);
  delay(1000); // Wait for Serial monitor to connect
  Serial.println("Starting FT8 Decoder...");

  // 1. Mount LittleFS (Flash Memory)
  if (!LittleFS.begin()) {
    Serial.println("Failed to mount LittleFS");
    return;
  }

  // 2. Open the WAV file
  File wavFile = LittleFS.open("/test_01.wav", "r");
  if (!wavFile || wavFile.isDirectory()) {
    Serial.println("Failed to open /test_01.wav from flash");
    return;
  }

  // 3. Read the standard 44-byte WAV header
  uint8_t header[44];
  if (wavFile.read(header, 44) != 44) {
    Serial.println("File too small to be a valid WAV");
    wavFile.close();
    return;
  }

  // Extract Sample Rate (Little Endian format: bytes 24-27)
  int rate = header[24] | (header[25] << 8) | (header[26] << 16) | (header[27] << 24);

  // Extract Data Size (Little Endian format: bytes 40-43)
  uint32_t data_size = header[40] | (header[41] << 8) | (header[42] << 16) | (header[43] << 24);

  // 4. Read the rest of the file into RAM
  std::vector<int16_t> s16(data_size / 2);
  
  wavFile.read((uint8_t*)s16.data(), data_size);
  wavFile.close();

  Serial.printf("Read %d bytes of audio (%d Hz). Decoding...\n", data_size, rate);

  start_now = (double)millis() / 1000.0;
  run_decoder(s16, rate);

  Serial.println("Decoding finished.");
}

void loop() {
  // Nothing to do here, execution happens once in setup()
}

#else
// ---------------------------------------------------------
// LINUX NATIVE IMPLEMENTATION
// ---------------------------------------------------------

void
usage()
{
  fprintf(stderr, "Usage: ft8mon -card card channel\n");
  fprintf(stderr, "       ft8mon -file xxx.wav ...\n");
  exit(1);
}

int
main(int argc, char *argv[])
{
  if(argc >= 3 && strcmp(argv[1], "-file") == 0){
    for(int ii = 2; ii < argc; ii++){
      // the .wav file should start at an even 15-second boundary.
      int rate;
      std::vector<int16_t> s16 = readwav(argv[ii], rate);
      //Int16Vector s16 = readwav(argv[ii], rate); // Use PSRAM vector for large audio data

      start_now = now();
      run_decoder(s16, rate);
    }
  } else {
    usage();
  }
  return 0;
}

#endif