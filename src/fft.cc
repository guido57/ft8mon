#include "fft.h"
#include <mutex>
#include <unistd.h>
#include <assert.h>
#include <sys/file.h>
#include <sys/types.h>
#include <sys/stat.h>
#include "util.h"

#include <set> 

// --- ISOLATED KISS FFT CACHE (With pre-allocated ESP32-friendly buffers) ---
#include "kiss_fft.h"
#include "kiss_fftr.h"
#include <map>
#include <mutex>
#if defined(ESP32) 
#include <esp_heap_caps.h>
#else
#define heap_caps_get_free_size(x) (0)
#endif

// A struct to hold the KISS config and its dedicated memory buffers
// so we never call malloc() during the decode cycle.
struct KissPlan {
  kiss_fftr_cfg cfg;
  float *m_in;
  char *raw_out;
  
  KissPlan(int n) {
    int nbins = (n / 2) + 1;
    cfg = kiss_fftr_alloc(n, 0, NULL, NULL);
    m_in = (float *) malloc(sizeof(float) * n);
    raw_out = (char *) malloc(nbins * 8); // 8 bytes per complex bin
    assert(cfg && m_in && raw_out);
  }
  
  ~KissPlan() {
    if(cfg) kiss_fftr_free(cfg);
    if(m_in) free(m_in);
    if(raw_out) free(raw_out);
  }
};
// ----------------------------------------------
// --- THREAD-FREE, SELF-CLEANING KISS FFT CACHE ---
static std::map<int, KissPlan*>& get_kiss_cache() {
  static std::map<int, KissPlan*> cache;
  return cache;
}

// Cache for Complex-to-Complex (C2C) KISS FFTs
struct KissC2CPlan {
  kiss_fft_cfg cfg;
  kiss_fft_cpx *buf_in;
  kiss_fft_cpx *buf_out;
  
  KissC2CPlan(int n) {
    cfg = kiss_fft_alloc(n, 0, NULL, NULL); // 0 = forward FFT
    buf_in = (kiss_fft_cpx*)malloc(sizeof(kiss_fft_cpx) * n);
    buf_out = (kiss_fft_cpx*)malloc(sizeof(kiss_fft_cpx) * n);
    assert(cfg && buf_in && buf_out);
  }
  
  ~KissC2CPlan() {
    if(cfg) kiss_fft_free(cfg);
    if(buf_in) free(buf_in);
    if(buf_out) free(buf_out);
  }
};

static std::map<int, KissC2CPlan*>& get_kiss_c2c_cache() {
  static std::map<int, KissC2CPlan*> cache;
  return cache;
}

// Cache for Inverse Complex-to-Complex KISS FFTs
struct KissC2CIPlan {
  kiss_fft_cfg cfg;
  kiss_fft_cpx *buf_in;
  kiss_fft_cpx *buf_out;
  
  KissC2CIPlan(int n) {
    cfg = kiss_fft_alloc(n, 1, NULL, NULL); // 1 = INVERSE FFT
    buf_in = (kiss_fft_cpx*)malloc(sizeof(kiss_fft_cpx) * n);
    buf_out = (kiss_fft_cpx*)malloc(sizeof(kiss_fft_cpx) * n);
    assert(cfg && buf_in && buf_out);
  }
  
  ~KissC2CIPlan() {
    if(cfg) kiss_fft_free(cfg);
    if(buf_in) free(buf_in);
    if(buf_out) free(buf_out);
  }
};

static std::map<int, KissC2CIPlan*>& get_kiss_c2ci_cache() {
  static std::map<int, KissC2CIPlan*> cache;
  return cache;
}

// Cache for Inverse Complex-to-Real KISS FFTs (kiss_fftri)
struct KissIFFTRPlan {
  kiss_fftr_cfg cfg;
  kiss_fft_cpx *buf_in;
  float *buf_out;
  
  KissIFFTRPlan(int n) {
    cfg = kiss_fftr_alloc(n, 1, NULL, NULL); // 1 = INVERSE
    buf_in = (kiss_fft_cpx*)malloc(sizeof(kiss_fft_cpx) * ((n/2) + 1));
    buf_out = (float*)malloc(sizeof(float) * n);
    assert(cfg && buf_in && buf_out);
  }
  
  ~KissIFFTRPlan() {
    if(cfg) kiss_fftr_free(cfg);
    if(buf_in) free(buf_in);
    if(buf_out) free(buf_out);
  }
};

static std::map<int, KissIFFTRPlan*>& get_kiss_ifft_cache() {
  static std::map<int, KissIFFTRPlan*> cache;
  return cache;
}
// ----------------------------------------------


#define TIMING 0

//
// do just one FFT on samples[i0..i0+block]
// real inputs, complex outputs.
// output has (block / 2) + 1) points.
//
std::vector<std::complex<float>>
one_fft(const std::vector<float> &samples, int i0, int block,
        const char *why, Plan *p)
{
  (void)p; // Silence unused parameter warning (leftover from FFTW)
  assert(i0 >= 0);
  assert(block > 1);
  
  int nsamples = samples.size();
  int nbins = (block / 2) + 1;

  // ==========================================
  // KISS FFT (TRULY allocate once and reuse!)
  // ==========================================
  std::map<int, KissPlan*>& cache = get_kiss_cache();
  if(cache.count(block) == 0) {
    printf("one_fft: allocating KissPlan for reason %s and block size %d. PSRAM free=%d\n", why, block,
            heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    cache[block] = new KissPlan(block);
    printf("one_fft: Allocated KissPlan for block size %d. PSRAM free=%d\n", 
            block,
            heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
  }else if( block > 10000){
    // printf("one_fft: Reusing KissPlan for reason %s and block size %d. PSRAM free=%d\n", why, block,
    //         heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
  }  
  
  KissPlan *kp = cache[block];

  float *m_in = kp->m_in;
  kiss_fft_cpx *cpx_out = (kiss_fft_cpx *)kp->raw_out;

  for(int i = 0; i < block; i++){
    m_in[i] = (i0 + i < nsamples) ? (float)samples[i0 + i] : 0.0f;
  }

  kiss_fftr(kp->cfg, m_in, cpx_out);
  // printf("one_fft: allocating an array of complex<double> of size nbins=%d for reason %s and block size %d. PSRAM free=%d\n", 
  //         nbins, why, block,
  //         heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
  std::vector<std::complex<float>> out(nbins);
  // printf("one_fft: filling the array with the results from cpx_out for reason %s and block size %d. PSRAM free=%d\n", 
          // why, block,
          // heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
  for(int bi = 0; bi < nbins; bi++){
    out[bi] = std::complex<float>((float)cpx_out[bi].r, (float)cpx_out[bi].i);
  }

  return out;
  // ==========================================
}

//
// do a full set of FFTs, one per symbol-time.
// bins[time][frequency]
//
ffts_t
ffts(const std::vector<float> &samples, int i0, int block, const char *why)
{
  assert(i0 >= 0);
  assert(block > 1 && (block % 2) == 0);
  
  int nsamples = samples.size();
  int nbins = (block / 2) + 1;
  int nblocks = (nsamples - i0) / block;
  ffts_t bins(nblocks);
  for(int si = 0; si < nblocks; si++){
    bins[si].resize(nbins);
  }

  // ==========================================
  // KISS FFT (Using pre-allocated KissPlan)
  // ==========================================
  std::map<int, KissPlan*>& cache = get_kiss_cache();
  if(cache.count(block) == 0) {
    printf("ffts: allocating KissPlan for reason %s and block size %d. PSRAM free=%d\n", why, block,
            heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    cache[block] = new KissPlan(block);
    printf("ffts: Allocated KissPlan for reason %s and block size %d. PSRAM free=%d\n", why, block,
            heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
  }
  KissPlan *kp = cache[block];

  float *m_in = kp->m_in;
  kiss_fft_cpx *cpx_out = (kiss_fft_cpx *)kp->raw_out;

  for(int si = 0; si < nblocks; si++){
    int off = i0 + si * block;
    for(int i = 0; i < block; i++){
      m_in[i] = (off + i < nsamples) ? (float)samples[off + i] : 0.0f;
    }

    kiss_fftr(kp->cfg, m_in, cpx_out);

    for(int bi = 0; bi < nbins; bi++){
      bins[si][bi] = std::complex<double>((double)cpx_out[bi].r, (double)cpx_out[bi].i);
    }
  }

  return bins;
  // ==========================================
}
//
// do just one FFT on samples[i0..i0+block]
// real inputs, complex outputs.
// output has block points.
//

//
// do just one FFT on samples[i0..i0+block]
// real inputs, complex outputs.
// output has block points.
//
std::vector<std::complex<double>>
one_fft_c(const std::vector<double> &samples, int i0, int block, const char *why)
{
  (void)why; // unused
  assert(i0 >= 0);
  assert(block > 1);
  
  int nsamples = samples.size();

  std::map<int, KissC2CPlan*>& cache = get_kiss_c2c_cache();
  if(cache.count(block) == 0) {
    printf("one_fft_c: allocating KissC2CPlan for reason %s and block size %d. PSRAM free=%d\n", why, block,
            heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    cache[block] = new KissC2CPlan(block);
    printf("one_fft_c: Allocated KissC2CPlan for reason %s and block size %d. PSRAM free=%d\n", why, block,
            heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
  }
  KissC2CPlan *kp = cache[block];
 
  for(int i = 0; i < block; i++){
    if(i0 + i < nsamples){
      kp->buf_in[i].r = (float)samples[i0 + i];
    } else {
      kp->buf_in[i].r = 0.0f;
    }
    kp->buf_in[i].i = 0.0f; // imaginary part is zero
  }

  kiss_fft(kp->cfg, kp->buf_in, kp->buf_out);

  std::vector<std::complex<double>> out(block);
  double norm = 1.0 / sqrt(block);
  for(int bi = 0; bi < block; bi++){
    out[bi] = std::complex<double>((double)kp->buf_out[bi].r, (double)kp->buf_out[bi].i) * norm;
  }

  return out;
}

std::vector<std::complex<double>>
one_fft_cc(const std::vector<std::complex<double>> &samples, int i0, int block, const char *why)
{
  (void)why; // unused
  assert(i0 >= 0);
  assert(block > 1);
  
  int nsamples = samples.size();

  std::map<int, KissC2CPlan*>& cache = get_kiss_c2c_cache();
  if(cache.count(block) == 0) {
    cache[block] = new KissC2CPlan(block);
  }
  KissC2CPlan *kp = cache[block];

  for(int i = 0; i < block; i++){
    if(i0 + i < nsamples){
      kp->buf_in[i].r = (float)samples[i0 + i].real();
      kp->buf_in[i].i = (float)samples[i0 + i].imag();
    } else {
      kp->buf_in[i].r = 0.0f;
      kp->buf_in[i].i = 0.0f;
    }
  }

  kiss_fft(kp->cfg, kp->buf_in, kp->buf_out);

  std::vector<std::complex<double>> out(block);
  // Note: KISS FFT forward does not scale, matching FFTW's fftw_execute_dft behavior
  for(int bi = 0; bi < block; bi++){
    out[bi] = std::complex<double>((double)kp->buf_out[bi].r, (double)kp->buf_out[bi].i);
  }

  return out;
}

std::vector<std::complex<double>>
one_ifft_cc(const std::vector<std::complex<double>> &bins, const char *why)
{
  
  int block = bins.size();

  std::map<int, KissC2CIPlan*>& cache = get_kiss_c2ci_cache();
  if(cache.count(block) == 0) {
    printf("one_ifft_cc: allocating KissC2CIPlan for reason %s and block size %d. PSRAM free=%d\n", why, block,
            heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    cache[block] = new KissC2CIPlan(block);
    printf("one_ifft_cc: Allocated KissC2CIPlan for reason %s and block size %d. PSRAM free=%d\n", why, block,
            heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
  }
  KissC2CIPlan *kp = cache[block];

  for(int bi = 0; bi < block; bi++){
    kp->buf_in[bi].r = (float)bins[bi].real();
    kp->buf_in[bi].i = (float)bins[bi].imag();
  }

  kiss_fft(kp->cfg, kp->buf_in, kp->buf_out);

  std::vector<std::complex<double>> out(block);
  double norm = 1.0 / sqrt(block);
  for(int i = 0; i < block; i++){
    out[i] = std::complex<double>((double)kp->buf_out[i].r, (double)kp->buf_out[i].i) * norm;
  }

  return out;
}


std::vector<float>
one_ifft(const std::vector<std::complex<float>> &bins, const char *why)
{
  (void)why; // unused
  int nbins = bins.size();
  int block = (nbins - 1) * 2;

  std::map<int, KissIFFTRPlan*>& cache = get_kiss_ifft_cache();
  if(cache.count(block) == 0) {
    printf("one_ifft: allocating KissIFFTRPlan for reason %s and block size %d. PSRAM free=%d\n", why, block,
            heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    cache[block] = new KissIFFTRPlan(block);
    printf("one_ifft: Allocated KissIFFTRPlan for reason %s and block size %d. PSRAM free=%d\n", why, block,
            heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
  }else if( block > 10000){
    // printf("one_ifft: Reusing KissIFFTRPlan for reason %s and block size %d. PSRAM free=%d\n", why, block,
    //         heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
  }
  KissIFFTRPlan *kp = cache[block];

  for(int bi = 0; bi < nbins; bi++){
    kp->buf_in[bi].r = (float)bins[bi].real();
    kp->buf_in[bi].i = (float)bins[bi].imag();
  }

  kiss_fftri(kp->cfg, kp->buf_in, kp->buf_out);

  std::vector<float> out(block);
  double norm = 1.0 / (double)block; // <-- ADD THIS: Match FFTW's automatic scaling
  for(int i = 0; i < block; i++){
    out[i] = (double)kp->buf_out[i] * norm; // <-- APPLY IT HERE
  }

  return out;
}

//
// return the analytic signal for signal x,
// just like scipy.signal.hilbert(), from which
// this code is copied.
//
// the return value is x + iy, where y is the hilbert transform of x.
//
std::vector<std::complex<double>>
analytic(const std::vector<double> &x, const char *why)
{
  ulong n = x.size();

  std::vector<std::complex<double>> y = one_fft_c(x, 0, n, why);
  assert(y.size() == n);

  // leave y[0] alone.
  // double the first (positive) half of the spectrum.
  // zero out the second (negative) half of the spectrum.
  // y[n/2] is the nyquist bucket if n is even; leave it alone.
  if((n % 2) == 0){
    for(ulong i = 1; i < n/2; i++)
      y[i] *= 2;
    for(ulong i = n/2+1; i < n; i++)
      y[i] = 0;
  } else {
    for(ulong i = 1; i < (n+1)/2; i++)
      y[i] *= 2;
    for(ulong i = (n+1)/2; i < n; i++)
      y[i] = 0;
  }
      
  std::vector<std::complex<double>> z = one_ifft_cc(y, why);

  return z;
}

//
// general-purpose shift x in frequency by hz.
// uses hilbert transform to avoid sidebands.
// but it does wrap around at 0 hz and the nyquist frequency.
//
// note analytic() does an FFT over the whole signal, which
// is expensive, and often re-used, but it turns out it
// isn't a big factor in overall run-time.
//
// like weakutil.py's freq_shift().
//
std::vector<double>
hilbert_shift(const std::vector<double> &x, double hz0, double hz1, int rate)
{
  // y = scipy.signal.hilbert(x)
  std::vector<std::complex<double>> y = analytic(x, "hilbert_shift");
  assert(y.size() == x.size());

  double dt = 1.0 / rate;
  int n = x.size();

  std::vector<double> ret(n);
  
  for(int i = 0; i < n; i++){
    // complex "local oscillator" at hz.
    double hz = hz0 + (i / (double)n) * (hz1 - hz0);
    std::complex<double> lo = std::exp(std::complex<double>(0.0, 2 * M_PI * hz * dt * i));
    ret[i] = (lo * y[i]).real();
  }

  return ret;
}

