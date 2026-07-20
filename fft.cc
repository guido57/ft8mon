#include "fft.h"
#include <mutex>
#include <unistd.h>
#include <assert.h>
#include <sys/file.h>
#include <sys/types.h>
#include <sys/stat.h>
#include "util.h"

// MEASURE=0, ESTIMATE=64, PATIENT=32
int fftw_type = FFTW_ESTIMATE;

#include "kiss_fft.h"
#include "kiss_fftr.h"
#include <map>
#include <set> 

// --- ISOLATED KISS FFT CACHE (With pre-allocated ESP32-friendly buffers) ---
#include "kiss_fft.h"
#include "kiss_fftr.h"
#include <map>
#include <mutex>

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

static std::mutex kiss_mu;
static std::map<int, KissPlan*> kiss_fwd_cache;

static kiss_fftr_cfg get_kiss_fwd(int n) {
  kiss_mu.lock();
  if(kiss_fwd_cache.count(n) == 0) {
    kiss_fwd_cache[n] = new KissPlan(n);
  }
  KissPlan *kp = kiss_fwd_cache[n];
  kiss_mu.unlock();
  return kp->cfg;
}
// ------------------------------

#define TIMING 0

// a cached fftw plan, for both of:
// fftw_plan_dft_r2c_1d(n, m_in, m_out, FFTW_ESTIMATE);
// fftw_plan_dft_c2r_1d(n, m_in, m_out, FFTW_ESTIMATE);
class Plan {
public:
  int n_;
  int type_;

  //
  // real -> complex
  //
  fftw_complex *c_; // (n_ / 2) + 1 of these
  double *r_; // n_ of these
  fftw_plan fwd_; // forward plan
  fftw_plan rev_; // reverse plan

  //
  // complex -> complex
  //
  fftw_complex *cc1_; // n
  fftw_complex *cc2_; // n
  fftw_plan cfwd_; // forward plan
  fftw_plan crev_; // reverse plan

  // how much CPU time spent in FFTs that use this plan.
#if TIMING
  double time_;
#endif
  const char *why_;
  int uses_;
};

static std::mutex plansmu;
static Plan *plans[1000];
static int nplans;
static int plan_master_pid = 0;

Plan *
get_plan(int n, const char *why)
{
  // cache fftw plans in the parent process,
  // so they will already be there for fork()ed children.

  plansmu.lock();
  
  if(plan_master_pid == 0){
    plan_master_pid = getpid();
  }
    
  for(int i = 0; i < nplans; i++){
    if(plans[i]->n_ == n
       && plans[i]->type_ == fftw_type
#if TIMING
       && strcmp(plans[i]->why_, why) == 0
#endif
       ){
      Plan *p = plans[i];
      p->uses_ += 1;
      plansmu.unlock();
      return p;
    }
  }

  double t0 = now();

  // fftw_make_planner_thread_safe();

  // the fftw planner is not thread-safe.
  // can't rely on plansmu because both ft8.so
  // and snd.so may be using separate copies of fft.cc.
  // the lock file really should be per process.
  int lockfd = creat("/tmp/fft-plan-lock", 0666);
  assert(lockfd >= 0);
  fchmod(lockfd, 0666);
  int lockret = flock(lockfd, LOCK_EX);
  assert(lockret == 0);

  fftw_set_timelimit(5);

  //
  // real -> complex
  //

  Plan *p = new Plan;
  
  p->n_ = n;
#if TIMING
  p->time_ = 0;
#endif
  p->uses_ = 1;
  p->why_ = why;
  p->r_ = (double*) fftw_malloc(n * sizeof(double));
  assert(p->r_);
  p->c_ = (fftw_complex*) fftw_malloc(((n/2)+1) * sizeof(fftw_complex));
  assert(p->c_);
  
  // FFTW_ESTIMATE
  // FFTW_MEASURE
  // FFTW_PATIENT
  // FFTW_EXHAUSTIVE
  int type = fftw_type;
  if(getpid() != plan_master_pid){
    type = FFTW_ESTIMATE;
  }
  p->type_ = type;
  p->fwd_ = fftw_plan_dft_r2c_1d(n, p->r_, p->c_, type);
  assert(p->fwd_);
  p->rev_ = fftw_plan_dft_c2r_1d(n, p->c_, p->r_, type);
  assert(p->rev_);
  
  //
  // complex -> complex
  //
  p->cc1_ = (fftw_complex*) fftw_malloc(n * sizeof(fftw_complex));
  assert(p->cc1_);
  p->cc2_ = (fftw_complex*) fftw_malloc(n * sizeof(fftw_complex));
  assert(p->cc2_);
  p->cfwd_ = fftw_plan_dft_1d(n, p->cc1_, p->cc2_, FFTW_FORWARD, type);
  assert(p->cfwd_);
  p->crev_ = fftw_plan_dft_1d(n, p->cc2_, p->cc1_, FFTW_BACKWARD, type);
  assert(p->crev_);

  flock(lockfd, LOCK_UN);
  close(lockfd);

  assert(nplans+1 < 1000);
  
  plans[nplans] = p;
  __sync_synchronize();
  nplans += 1;

  if(0 && getpid() == plan_master_pid){
    double t1 = now();
    fprintf(stderr, "miss pid=%d master=%d n=%d t=%.3f total=%d type=%d, %s\n",
            getpid(), plan_master_pid, n, t1 - t0, nplans, type, why);
  }

  plansmu.unlock();

  return p;
}

//
// do just one FFT on samples[i0..i0+block]
// real inputs, complex outputs.
// output has (block / 2) + 1) points.
//
std::vector<std::complex<double>>
one_fft(const std::vector<double> &samples, int i0, int block,
        const char *why, Plan *p)
{
  assert(i0 >= 0);
  assert(block > 1);
  
  int nsamples = samples.size();
  int nbins = (block / 2) + 1;

  // ==========================================
  // KISS FFT (TRULY allocate once and reuse!)
  // ==========================================
  kiss_mu.lock();
  if(kiss_fwd_cache.count(block) == 0) {
    kiss_fwd_cache[block] = new KissPlan(block);
  }
  KissPlan *kp = kiss_fwd_cache[block];
  kiss_mu.unlock();

  // NO MORE MALLOC! Just use the buffers attached to the plan.
  float *m_in = kp->m_in;
  char *raw_out = kp->raw_out;

  for(int i = 0; i < block; i++){
    m_in[i] = (i0 + i < nsamples) ? (float)samples[i0 + i] : 0.0f;
  }

  kiss_fftr(kp->cfg, m_in, (kiss_fft_cpx*)raw_out);

  std::vector<std::complex<double>> out(nbins);
  for(int bi = 0; bi < nbins; bi++){
    float *f_ptr = (float*)(raw_out + bi * 8);
    out[bi] = std::complex<double>((double)f_ptr[0], (double)f_ptr[1]);
  }

  return out;
  // ==========================================
}

//
// do a full set of FFTs, one per symbol-time.
// bins[time][frequency]
//
ffts_t
ffts(const std::vector<double> &samples, int i0, int block, const char *why)
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
  // KISS FFT (Optimized for ESP32: allocate once, reuse!)
  // ==========================================
  kiss_fftr_cfg kiss_cfg = get_kiss_fwd(block);
  assert(kiss_cfg);

  // Allocate buffers ONCE and reuse them for the whole loop
  float *m_in = (float *) malloc(sizeof(float) * block);
  int out_byte_size = nbins * 8;
  char *raw_out = (char *) malloc(out_byte_size);
  assert(m_in && raw_out);

  for(int si = 0; si < nblocks; si++){
    int off = i0 + si * block;
    for(int i = 0; i < block; i++){
      m_in[i] = (off + i < nsamples) ? (float)samples[off + i] : 0.0f;
    }

    kiss_fftr(kiss_cfg, m_in, (kiss_fft_cpx*)raw_out);

    for(int bi = 0; bi < nbins; bi++){
      float *f_ptr = (float*)(raw_out + bi * 8);
      bins[si][bi] = std::complex<double>((double)f_ptr[0], (double)f_ptr[1]);
    }
  }

  // Free once at the very end
  free(raw_out);
  free(m_in);

  return bins;
  // ==========================================
}

//
// do just one FFT on samples[i0..i0+block]
// real inputs, complex outputs.
// output has block points.
//
std::vector<std::complex<double>>
one_fft_c(const std::vector<double> &samples, int i0, int block, const char *why)
{
  assert(i0 >= 0);
  assert(block > 1);
  
  int nsamples = samples.size();

  Plan *p = get_plan(block, why);
  fftw_plan m_plan = p->cfwd_;

#if TIMING
  double t0 = now();
#endif

  fftw_complex *m_in  = (fftw_complex*) fftw_malloc(block * sizeof(fftw_complex));
  fftw_complex *m_out = (fftw_complex*) fftw_malloc(block * sizeof(fftw_complex));
  assert(m_in && m_out);

  for(int i = 0; i < block; i++){
    if(i0 + i < nsamples){
      m_in[i][0] = samples[i0 + i]; // real
    } else {
      m_in[i][0] = 0;
    }
    m_in[i][1] = 0; // imaginary
  }

  fftw_execute_dft(m_plan, m_in, m_out);

  std::vector<std::complex<double>> out(block);

  double norm = 1.0 / sqrt(block);
  for(int bi = 0; bi < block; bi++){
    double re = m_out[bi][0];
    double im = m_out[bi][1];
    std::complex<double> c(re, im);
    c *= norm;
    out[bi] = c;
  }
    
  fftw_free(m_in);
  fftw_free(m_out);

#if TIMING
  p->time_ += now() - t0;
#endif

  return out;
}

std::vector<std::complex<double>>
one_fft_cc(const std::vector<std::complex<double>> &samples, int i0, int block, const char *why)
{
  assert(i0 >= 0);
  assert(block > 1);
  
  int nsamples = samples.size();

  Plan *p = get_plan(block, why);
  fftw_plan m_plan = p->cfwd_;

#if TIMING
  double t0 = now();
#endif

  fftw_complex *m_in  = (fftw_complex*) fftw_malloc(block * sizeof(fftw_complex));
  fftw_complex *m_out = (fftw_complex*) fftw_malloc(block * sizeof(fftw_complex));
  assert(m_in && m_out);

  for(int i = 0; i < block; i++){
    if(i0 + i < nsamples){
      m_in[i][0] = samples[i0 + i].real();
      m_in[i][1] = samples[i0 + i].imag();
    } else {
      m_in[i][0] = 0;
      m_in[i][1] = 0;
    }
  }

  fftw_execute_dft(m_plan, m_in, m_out);

  std::vector<std::complex<double>> out(block);

  //double norm = 1.0 / sqrt(block);
  for(int bi = 0; bi < block; bi++){
    double re = m_out[bi][0];
    double im = m_out[bi][1];
    std::complex<double> c(re, im);
    //c *= norm;
    out[bi] = c;
  }
    
  fftw_free(m_in);
  fftw_free(m_out);

#if TIMING
  p->time_ += now() - t0;
#endif

  return out;
}

std::vector<std::complex<double>>
one_ifft_cc(const std::vector<std::complex<double>> &bins, const char *why)
{
  int block = bins.size();

  Plan *p = get_plan(block, why);
  fftw_plan m_plan = p->crev_;

#if TIMING
  double t0 = now();
#endif

  fftw_complex *m_in = (fftw_complex*) fftw_malloc(block * sizeof(fftw_complex));
  fftw_complex *m_out = (fftw_complex *) fftw_malloc(block * sizeof(fftw_complex));
  assert(m_in && m_out);

  for(int bi = 0; bi < block; bi++){
    double re = bins[bi].real();
    double im = bins[bi].imag();
    m_in[bi][0] = re;
    m_in[bi][1] = im;
  }

  fftw_execute_dft(m_plan, m_in, m_out);

  std::vector<std::complex<double>> out(block);
  double norm = 1.0 / sqrt(block);
  for(int i = 0; i < block; i++){
    double re = m_out[i][0];
    double im = m_out[i][1];
    std::complex<double> c(re, im);
    c *= norm;
    out[i] = c;
  }

  fftw_free(m_in);
  fftw_free(m_out);

#if TIMING
  p->time_ += now() - t0;
#endif

  return out;
}

std::vector<double>
one_ifft(const std::vector<std::complex<double>> &bins, const char *why)
{
  int nbins = bins.size();
  int block = (nbins - 1) * 2;

  Plan *p = get_plan(block, why);
  fftw_plan m_plan = p->rev_;

#if TIMING
  double t0 = now();
#endif

  fftw_complex *m_in = (fftw_complex *) fftw_malloc(sizeof(fftw_complex) *
                                                    ((p->n_ / 2) + 1));
  double *m_out = (double *) fftw_malloc(sizeof(double) * p->n_);

  for(int bi = 0; bi < nbins; bi++){
    double re = bins[bi].real();
    double im = bins[bi].imag();
    m_in[bi][0] = re;
    m_in[bi][1] = im;
  }

  fftw_execute_dft_c2r(m_plan, m_in, m_out);

  std::vector<double> out(block);
  for(int i = 0; i < block; i++){
    out[i] = m_out[i];
  }

  fftw_free(m_in);
  fftw_free(m_out);

#if TIMING
  p->time_ += now() - t0;
#endif

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

void
fft_stats()
{
  for(int i = 0; i < nplans; i++){
    Plan *p = plans[i];
    printf("%-13s %6d %9d %6.3f\n",
           p->why_,
           p->n_,
           p->uses_,
#if TIMING
           p->time_
#else
           0.0
#endif
           );
  }
}
