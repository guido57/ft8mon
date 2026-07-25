//#include <sndfile.h>
#include <sys/time.h>
#include <fstream>
#include <cstring>
#include <assert.h>
#include <math.h>
#include <string.h>
#include <complex>
#include "util.h"

double
now()
{
  struct timeval tv;
  gettimeofday(&tv, 0);
  return tv.tv_sec + tv.tv_usec / 1000000.0;
}


// Helper function to read a 4-byte little-endian integer from a file
static uint32_t read_u32(std::ifstream &f) {
    uint8_t b[4];
    f.read(reinterpret_cast<char*>(b), 4);
    return (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
}

// Helper function to read a 2-byte little-endian integer
static uint16_t read_u16(std::ifstream &f) {
    uint8_t b[2];
    f.read(reinterpret_cast<char*>(b), 2);
    return (uint16_t)b[0] | ((uint16_t)b[1] << 8);
}

#include <fstream>
#include <cstring>

std::vector<int16_t>
readwav(const char *filename, int &rate_out)
{
    std::ifstream file(filename, std::ios::in | std::ios::binary);
    if(!file.is_open()){
        fprintf(stderr, "ERROR: cannot open wav file %s\n", filename);
        exit(1);
    }

    // 1. Read and verify the RIFF header
    char riff_id[5] = {0};
    file.read(riff_id, 4);
    if(std::memcmp(riff_id, "RIFF", 4) != 0) {
        fprintf(stderr, "ERROR: %s is not a valid WAV file (missing RIFF)\n", filename);
        exit(1);
    }
    
    read_u32(file); // Skip file size
    
    char wave_id[5] = {0};
    file.read(wave_id, 4);
    if(std::memcmp(wave_id, "WAVE", 4) != 0) {
        fprintf(stderr, "ERROR: %s is not a valid WAV file (missing WAVE)\n", filename);
        exit(1);
    }

    // 2. Search for the "fmt " and "data" chunks
    bool found_fmt = false;
    bool found_data = false;
    uint16_t audio_format = 0;
    uint16_t num_channels = 0;
    uint32_t sample_rate = 0;
    uint16_t bits_per_sample = 0;
    uint32_t data_size = 0;

    while(!found_data && !file.eof()) {
        char chunk_id[5] = {0};
        file.read(chunk_id, 4);
        uint32_t chunk_size = read_u32(file);

        if(std::memcmp(chunk_id, "fmt ", 4) == 0) {
            found_fmt = true;
            audio_format = read_u16(file);
            num_channels = read_u16(file);
            sample_rate = read_u32(file);
            read_u32(file); // byte rate
            read_u16(file); // block align
            bits_per_sample = read_u16(file);
            
            // Skip any extra format bytes
            if(chunk_size > 16) {
                file.seekg(chunk_size - 16, std::ios::cur);
            }
        } 
        else if(std::memcmp(chunk_id, "data", 4) == 0) {
            found_data = true;
            data_size = chunk_size;
        } 
        else {
            // Skip unknown chunks (like LIST, bext, etc.)
            file.seekg(chunk_size, std::ios::cur);
        }
    }

    if(!found_fmt || !found_data) {
        fprintf(stderr, "ERROR: %s is missing fmt or data chunks\n", filename);
        exit(1);
    }

    // 3. Prepare to read audio data
    rate_out = sample_rate;
    uint32_t total_samples = data_size / (num_channels * (bits_per_sample / 8));
    
    std::vector<int16_t> out;
    out.resize(total_samples);

    // 4. Read the audio data
    if(audio_format == 1 && bits_per_sample == 16) {
        // 16-bit PCM (The native format of ESP32 I2S microphones)
        file.read(reinterpret_cast<char*>(out.data()), data_size);
        
        if(!file) {
             fprintf(stderr, "ERROR: Failed to read full data chunk!\n");
             exit(1);
        }
        
        // If stereo, extract just the left channel
        if(num_channels > 1) {
            std::vector<int16_t> stereo_data = out;
            out.clear();
            for(uint32_t i = 0; i < total_samples; i++) {
                out.push_back(stereo_data[i * num_channels]);
            }
        }
    } 
    else if(audio_format == 3 && bits_per_sample == 32) {
        // If you ever get 32-bit float WAVs, convert to 16-bit int
        std::vector<float> buf(total_samples * num_channels);
        file.read(reinterpret_cast<char*>(buf.data()), data_size);
        for(uint32_t i = 0; i < total_samples; i++) {
            float val = buf[i * num_channels] * 32767.0f;
            if(val > 32767.0f) val = 32767.0f;
            if(val < -32768.0f) val = -32768.0f;
            out[i] = (int16_t)val;
        }
    }
    else {
        fprintf(stderr, "ERROR: Unsupported WAV format. Please use 16-bit PCM or 32-bit Float.\n");
        exit(1);
    }

    file.close();
    return out;
}

// void
// writewav(const std::vector<double> &samples, const char *filename, int rate)
// {
//   double mx = 0;
//   for(ulong i = 0; i < samples.size(); i++){
//     mx = std::max(mx, std::abs(samples[i]));
//   }
//   std::vector<double> v(samples.size());
//   for(ulong i = 0; i < samples.size(); i++){
//     v[i] = (samples[i] / mx) * 0.95;
//   }

//   SF_INFO sf;
//   sf.channels = 1;
//   sf.samplerate = rate;
//   sf.format = SF_FORMAT_WAV | SF_FORMAT_PCM_16;
//   SNDFILE *f = sf_open(filename, SFM_WRITE, &sf);
//   assert(f);
//   sf_write_double(f, v.data(), v.size());
//   sf_write_sync(f);
//   sf_close(f);
// }

// std::vector<double>
// readwav(const char *filename, int &rate_out)
// {
//   SF_INFO info;
//   memset(&info, 0, sizeof(info));
//   SNDFILE *sf = sf_open(filename, SFM_READ, &info);
//   if(sf == 0){
//     fprintf(stderr, "cannot open %s\n", filename);
//     exit(1); // XXX
//   }
//   rate_out = info.samplerate;

//   std::vector<double> out;

//   while(1){
//     double buf[512];
//     int n = sf_read_double(sf, buf, 512);
//     if(n <= 0)
//       break;
//     for(int i = 0; i < n; i++){
//       out.push_back(buf[i]);
//     }
//   }

//   sf_close(sf);

//   return out;
// }

void
writetxt(std::vector<double> v, const char *filename)
{
  FILE *fp = fopen(filename, "w");
  if(fp == 0){
    fprintf(stderr, "could not write %s\n", filename);
    exit(1);
  }
  for(ulong i = 0; i < v.size(); i++){
    fprintf(fp, "%f\n", v[i]);
  }
  fclose(fp);
}

//
// Goertzel Algorithm for a Non-integer Frequency Index, Rick Lyons
// https://www.dsprelated.com/showarticle/495.php
//
std::complex<double>
goertzel(std::vector<double> v, int rate, int i0, int n, double hz)
{
  //double radians_per_sample = (hz * 2 * M_PI) / rate;
  //double k = radians_per_sample * n;
  double bin_hz = rate / (double) n;
  double k = hz / bin_hz;

  double alpha = 2 * M_PI * k / n;
  double beta = 2 * M_PI * k * (n - 1.0) / n;

  double two_cos_alpha = 2 * cos(alpha);
  double a = cos(beta);
  double b = -sin(beta);
  double c = sin(alpha) * sin(beta) - cos(alpha)*cos(beta);
  double d = sin(2 * M_PI * k);

  double w1 = 0;
  double w2 = 0;

  for(int i = 0; i < n; i++){
    double w0 = v[i0+i] + two_cos_alpha * w1 - w2;
    w2 = w1;
    w1 = w0;
  }

  double re = w1*a + w2*c;
  double im = w1*b + w2*d;

  return std::complex<double>(re, im);
}

double
vmax(const std::vector<double> &v)
{
  double mx = 0;
  int got = 0;
  for(int i = 0; i < (int) v.size(); i++){
    if(got == 0 || v[i] > mx){
      got = 1;
      mx = v[i];
    }
  }
  return mx;
}

std::vector<double>
vreal(const std::vector<std::complex<double>> &a)
{
  std::vector<double> b(a.size());
  for(int i = 0; i < (int) a.size(); i++){
    b[i] = a[i].real();
  }
  return b;
}

std::vector<double>
vimag(const std::vector<std::complex<double>> &a)
{
  std::vector<double> b(a.size());
  for(int i = 0; i < (int) a.size(); i++){
    b[i] = a[i].imag();
  }
  return b;
}

// generate 8-FSK, at 25 hz, bin size 6.25 hz,
// 200 samples/second, 32 samples/symbol.
// used as reference to detect pairs of symbols.
// superseded by gfsk().
std::vector<std::complex<double>>
fsk_c(const std::vector<int> &syms)
{
  int n = syms.size();
  std::vector<std::complex<double>> v(n*32);
  double theta = 0;
  for(int si = 0; si < n; si++){
    double hz = 25 + syms[si] * 6.25;
    for(int i = 0; i < 32; i++){
      // v[si*32+i] = std::complex(cos(theta), sin(theta));
      v[si*32+i] = {cos(theta), sin(theta)};
      theta += 2 * M_PI / (200 / hz);
    }
  }
  return v;
}

// copied from wsjt-x ft2/gfsk_pulse.f90.
// b is 1.0 for FT4; 2.0 for FT8. 
double
gfsk_point(double b, double t)
{
  double c = M_PI * sqrt(2.0 / log(2.0));
  double x = 0.5 * (erf(c * b * (t + 0.5)) - erf(c * b * (t - 0.5)));
  return x;
}

// the smoothing window for gfsk.
// run the window over impulses of symbol frequencies,
// each impulse at the center of its symbol time.
// three symbols wide.
// most of the pulse is in the center symbol.
// b is 1.0 for FT4; 2.0 for FT8. 
std::vector<double>
gfsk_window(int samples_per_symbol, double b)
{
  std::vector<double> v(3 * samples_per_symbol);
  double sum = 0;
  for(int i = 0; i < (int) v.size(); i++){
    double x = i / (double)samples_per_symbol;
    x -= 1.5;
    double y = gfsk_point(b, x);
    v[i] = y;
    sum += y;
  }
  
  for(int i = 0; i < (int) v.size(); i++){
    v[i] /= sum;
  }

  return v;
}

// gaussian-smoothed fsk.
// the gaussian smooths the instantaneous frequencies,
// so that the transitions between symbols don't
// cause clicks.
// gwin is gfsk_window(32, 2.0)
std::vector<std::complex<double>>
gfsk_c(const std::vector<int> &symbols,
     double hz0, double hz1,
     double spacing, int rate, int symsamples,
     double phase0,
     const std::vector<double> &gwin)
{
  assert((gwin.size() % 2) == 0);
  
  // compute frequency for each symbol.
  // generate a spike in the middle of each symbol time;
  // the gaussian filter will turn it into a waveform.
  std::vector<double> hzv(symsamples * (symbols.size() + 2), 0.0);
  for(int bi = 0; bi < (int) symbols.size(); bi++){
    double base_hz = hz0 + (hz1 - hz0) * (bi / (double) symbols.size());
    double fr = base_hz + (symbols[bi] * spacing);
    int mid = symsamples*(bi+1) + symsamples/2;
    // the window has even size, so split the impulse over
    // the two middle samples to be symmetric.
    hzv[mid] = fr * symsamples / 2.0;
    hzv[mid-1] = fr * symsamples / 2.0;
  }

  // repeat first and last symbols
  for(int i = 0; i < symsamples; i++){
    hzv[i] = hzv[i+symsamples];
    hzv[symsamples*(symbols.size()+1) + i] = hzv[symsamples*symbols.size() + i];
  }
  
  // run the per-sample frequency vector through
  // the gaussian filter.
  int half = gwin.size() / 2;
  std::vector<double> o(hzv.size());
  for(int i = 0; i < (int) o.size(); i++){
    double sum = 0;
    for(int j = 0; j < (int) gwin.size(); j++){
      int k = i - half + j;
      if(k >= 0 && k < (int) hzv.size()){
        sum += hzv[k] * gwin[j];
      }
    }
    o[i] = sum;
  }

  // drop repeated first and last symbols
  std::vector<double> oo(symsamples * symbols.size());
  for(int i = 0; i < (int) oo.size(); i++){
    oo[i] = o[i + symsamples];
  }

  // now oo[i] contains the frequency for the i'th sample.

  std::vector<std::complex<double>> v(symsamples * symbols.size());
  double theta = phase0;
  for(int i = 0; i < (int) v.size(); i++){
    // v[i] = std::complex(cos(theta), sin(theta));
    v[i] = {cos(theta), sin(theta)};
    double hz = oo[i];
    theta += 2 * M_PI / (rate / hz);
  }

  return v;
}

// gaussian-smoothed fsk.
// the gaussian smooths the instantaneous frequencies,
// so that the transitions between symbols don't
// cause clicks.
// gwin is gfsk_window(32, 2.0)
std::vector<double>
gfsk_r(const std::vector<int> &symbols,
       double hz0, double hz1,
       double spacing, int rate, int symsamples,
       double phase0,
       const std::vector<double> &gwin)
{
  assert((gwin.size() % 2) == 0);
  
  // compute frequency for each symbol.
  // generate a spike in the middle of each symbol time;
  // the gaussian filter will turn it into a waveform.
  std::vector<double> hzv(symsamples * (symbols.size() + 2), 0.0);
  for(int bi = 0; bi < (int) symbols.size(); bi++){
    double base_hz = hz0 + (hz1 - hz0) * (bi / (double) symbols.size());
    double fr = base_hz + (symbols[bi] * spacing);
    int mid = symsamples*(bi+1) + symsamples/2;
    // the window has even size, so split the impulse over
    // the two middle samples to be symmetric.
    hzv[mid] = fr * symsamples / 2.0;
    hzv[mid-1] = fr * symsamples / 2.0;
  }

  // repeat first and last symbols
  for(int i = 0; i < symsamples; i++){
    hzv[i] = hzv[i+symsamples];
    hzv[symsamples*(symbols.size()+1) + i] = hzv[symsamples*symbols.size() + i];
  }
  
  // run the per-sample frequency vector through
  // the gaussian filter.
  int half = gwin.size() / 2;
  std::vector<double> o(hzv.size());
  for(int i = 0; i < (int) o.size(); i++){
    double sum = 0;
    for(int j = 0; j < (int) gwin.size(); j++){
      int k = i - half + j;
      if(k >= 0 && k < (int) hzv.size()){
        sum += hzv[k] * gwin[j];
      }
    }
    o[i] = sum;
  }

  // drop repeated first and last symbols
  std::vector<double> oo(symsamples * symbols.size());
  for(int i = 0; i < (int) oo.size(); i++){
    oo[i] = o[i + symsamples];
  }

  // now oo[i] contains the frequency for the i'th sample.

  std::vector<double> v(symsamples * symbols.size());
  double theta = phase0;
  for(int i = 0; i < (int) v.size(); i++){
    v[i] = cos(theta);
    double hz = oo[i];
    theta += 2 * M_PI / (rate / hz);
  }

  return v;
}
