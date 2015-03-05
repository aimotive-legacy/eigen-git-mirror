// This file is part of Eigen, a lightweight C++ template library
// for linear algebra.
//
// Copyright (C) 2015 Benoit Jacob <benoitjacob@google.com>
//
// This Source Code Form is subject to the terms of the Mozilla
// Public License v. 2.0. If a copy of the MPL was not distributed
// with this file, You can obtain one at http://mozilla.org/MPL/2.0/.

#include <iostream>
#include <cstdint>
#include <cstdlib>
#include <vector>
#include <fstream>
#include <memory>

bool eigen_use_specific_block_size;
int eigen_block_size_k, eigen_block_size_m, eigen_block_size_n;
#define EIGEN_TEST_SPECIFIC_BLOCKING_SIZES eigen_use_specific_block_size
#define EIGEN_TEST_SPECIFIC_BLOCKING_SIZE_K eigen_block_size_k
#define EIGEN_TEST_SPECIFIC_BLOCKING_SIZE_M eigen_block_size_m
#define EIGEN_TEST_SPECIFIC_BLOCKING_SIZE_N eigen_block_size_n
#include <Eigen/Core>

#include <bench/BenchTimer.h>

using namespace Eigen;
using namespace std;

static BenchTimer timer;

// how many times we repeat each measurement.
// measurements are randomly shuffled - we're not doing
// all N identical measurements in a row.
const int measurement_repetitions = 3;

// Timings below this value are too short to be accurate,
// we'll repeat measurements with more iterations until
// we get a timing above that threshold.
const float g_min_accurate_time = 1e-2f;

// See --min-working-set-size command line parameter.
size_t g_min_working_set_size = 0;

// range of sizes that we will benchmark (in all 3 K,M,N dimensions)
const size_t maxsize = 2048;
const size_t minsize = 16;

typedef MatrixXf MatrixType;
typedef MatrixType::Scalar Scalar;
typedef internal::packet_traits<Scalar>::type Packet;

static_assert((maxsize & (maxsize - 1)) == 0, "maxsize must be a power of two");
static_assert((minsize & (minsize - 1)) == 0, "minsize must be a power of two");
static_assert(maxsize > minsize, "maxsize must be larger than minsize");
static_assert(maxsize < (minsize << 16), "maxsize must be less than (minsize<<16)");

// just a helper to store a triple of K,M,N sizes for matrix product
struct size_triple_t
{
  size_t k, m, n;
  size_triple_t() : k(0), m(0), n(0) {}
  size_triple_t(size_t _k, size_t _m, size_t _n) : k(_k), m(_m), n(_n) {}
  size_triple_t(const size_triple_t& o) : k(o.k), m(o.m), n(o.n) {}
  size_triple_t(uint16_t compact)
  {
    k = 1 << ((compact & 0xf00) >> 8);
    m = 1 << ((compact & 0x0f0) >> 4);
    n = 1 << ((compact & 0x00f) >> 0);
  }
};

uint8_t log2_pot(size_t x) {
  size_t l = 0;
  while (x >>= 1) l++;
  return l;
}

// Convert between size tripes and a compact form fitting in 12 bits
// where each size, which must be a POT, is encoded as its log2, on 4 bits
// so the largest representable size is 2^15 == 32k  ... big enough.
uint16_t compact_size_triple(size_t k, size_t m, size_t n)
{
  return (log2_pot(k) << 8) | (log2_pot(m) << 4) | log2_pot(n);
}

uint16_t compact_size_triple(const size_triple_t& t)
{
  return compact_size_triple(t.k, t.m, t.n);
}

// A single benchmark. Initially only contains benchmark params.
// Then call run(), which stores the result in the gflops field.
struct benchmark_t
{
  uint16_t compact_product_size;
  uint16_t compact_block_size;
  bool use_default_block_size;
  float gflops;
  size_t min_working_set_size;
  float min_accurate_time;
  benchmark_t(size_t pk, size_t pm, size_t pn,
              size_t bk, size_t bm, size_t bn)
    : compact_product_size(compact_size_triple(pk, pm, pn))
    , compact_block_size(compact_size_triple(bk, bm, bn))
    , use_default_block_size(false)
    , gflops(0)
    , min_working_set_size(g_min_working_set_size)
    , min_accurate_time(g_min_accurate_time)
  {}
  benchmark_t(size_t pk, size_t pm, size_t pn)
    : compact_product_size(compact_size_triple(pk, pm, pn))
    , compact_block_size(0)
    , use_default_block_size(true)
    , gflops(0)
    , min_working_set_size(g_min_working_set_size)
    , min_accurate_time(g_min_accurate_time)
  {}

  void run();
};

ostream& operator<<(ostream& s, const benchmark_t& b)
{
  s << hex << b.compact_product_size << dec;
  if (b.use_default_block_size) {
    size_triple_t t(b.compact_product_size);
    Index k = t.k, m = t.m, n = t.n;
    internal::computeProductBlockingSizes<Scalar, Scalar>(k, m, n);
    s << " default(" << k << ", " << m << ", " << n << ")";
  } else {
    s << " " << hex << b.compact_block_size << dec;
  }
  s << " " << b.gflops;
  return s;
}

// We sort first by increasing benchmark parameters,
// then by decreasing performance.
bool operator<(const benchmark_t& b1, const benchmark_t& b2)
{ 
  return b1.compact_product_size < b2.compact_product_size ||
           (b1.compact_product_size == b2.compact_product_size && (
             (b1.compact_block_size < b2.compact_block_size || (
               b1.compact_block_size == b2.compact_block_size &&
                 b1.gflops > b2.gflops))));
}

void benchmark_t::run()
{
  size_triple_t productsizes(compact_product_size);

  if (use_default_block_size) {
    eigen_use_specific_block_size = false;
  } else {
    // feed eigen with our custom blocking params
    eigen_use_specific_block_size = true;
    size_triple_t blocksizes(compact_block_size);
    eigen_block_size_k = blocksizes.k;
    eigen_block_size_m = blocksizes.m;
    eigen_block_size_n = blocksizes.n;
  }

  // set up the matrix pool

  const size_t combined_three_matrices_sizes =
    sizeof(Scalar) *
      (productsizes.k * productsizes.m +
       productsizes.k * productsizes.n +
       productsizes.m * productsizes.n);

  // 64 M is large enough that nobody has a cache bigger than that,
  // while still being small enough that everybody has this much RAM,
  // so conveniently we don't need to special-case platforms here.
  const size_t unlikely_large_cache_size = 64 << 20;

  const size_t working_set_size =
    min_working_set_size ? min_working_set_size : unlikely_large_cache_size;

  const size_t matrix_pool_size =
    1 + working_set_size / combined_three_matrices_sizes;

  MatrixType *lhs = new MatrixType[matrix_pool_size];
  MatrixType *rhs = new MatrixType[matrix_pool_size];
  MatrixType *dst = new MatrixType[matrix_pool_size];
  
  for (size_t i = 0; i < matrix_pool_size; i++) {
    lhs[i] = MatrixType::Zero(productsizes.m, productsizes.k);
    rhs[i] = MatrixType::Zero(productsizes.k, productsizes.n);
    dst[i] = MatrixType::Zero(productsizes.m, productsizes.n);
  }

  // main benchmark loop

  int iters_at_a_time = 1;
  float time_per_iter = 0.0f;
  size_t matrix_index = 0;
  while (true) {

    double starttime = timer.getCpuTime();
    for (int i = 0; i < iters_at_a_time; i++) {
      dst[matrix_index] = lhs[matrix_index] * rhs[matrix_index];
      matrix_index++;
      if (matrix_index == matrix_pool_size) {
        matrix_index = 0;
      }
    }
    double endtime = timer.getCpuTime();

    const float timing = float(endtime - starttime);

    if (timing >= min_accurate_time) {
      time_per_iter = timing / iters_at_a_time;
      break;
    }

    iters_at_a_time *= 2;
  }

  delete[] lhs;
  delete[] rhs;
  delete[] dst;

  gflops = 2e-9 * productsizes.k * productsizes.m * productsizes.n / time_per_iter;
}

void print_cpuinfo()
{
#ifdef __linux__
  cout << "contents of /proc/cpuinfo:" << endl;
  string line;
  ifstream cpuinfo("/proc/cpuinfo");
  if (cpuinfo.is_open()) {
    while (getline(cpuinfo, line)) {
      cout << line << endl;
    }
    cpuinfo.close();
  }
  cout << endl;
#elif defined __APPLE__
  cout << "output of sysctl hw:" << endl;
  system("sysctl hw");
  cout << endl;
#endif
}

template <typename T>
string type_name()
{
  return "unknown";
}

template<>
string type_name<float>()
{
  return "float";
}

template<>
string type_name<double>()
{
  return "double";
}

struct action_t
{
  virtual const char* invokation_name() const { abort(); return nullptr; }
  virtual void run() const { abort(); }
  virtual ~action_t() {}
};

void show_usage_and_exit(int /*argc*/, char* argv[],
                         const vector<unique_ptr<action_t>>& available_actions)
{
  cerr << "usage: " << argv[0] << " <action> [options...]" << endl << endl;
  cerr << "available actions:" << endl << endl;
  for (auto it = available_actions.begin(); it != available_actions.end(); ++it) {
    cerr << "  " << (*it)->invokation_name() << endl;
  }
  cerr << endl;
  cerr << "options:" << endl << endl;
  cerr << "  --min-working-set-size=N:" << endl;
  cerr << "       Set the minimum working set size to N bytes." << endl;
  cerr << "       This is rounded up as needed to a multiple of matrix size." << endl;
  cerr << "       A larger working set lowers the chance of a warm cache." << endl;
  cerr << "       The default value 0 means use a large enough working" << endl;
  cerr << "       set to likely outsize caches." << endl;
  cerr << "       A value of 1 (that is, 1 byte) would mean don't do anything to" << endl;
  cerr << "       avoid warm caches." << endl;
  exit(1);
}
     
float measure_clock_speed()
{
  cerr << "Measuring clock speed...                              \r" << flush;
          
  vector<float> all_gflops;
  for (int i = 0; i < 8; i++) {
    // a good measure of clock speed is obtained by benchmarking small matrices that
    // fit in L1 cache and use warm caches (min_working_set_size = 1).
    benchmark_t b(128, 128, 128);
    b.min_working_set_size = 1;
    b.min_accurate_time = 0.1f; // long-running for better accuracy
    b.run();
    all_gflops.push_back(b.gflops);
  }

  sort(all_gflops.begin(), all_gflops.end());
  float stable_estimate = all_gflops[2] + all_gflops[3] + all_gflops[4] + all_gflops[5];

  // multiply by an arbitrary constant to discourage trying doing anything with the
  // returned values besides just comparing them with each other.
  float result = stable_estimate * 123.456f;

  return result;
}

struct human_duration_t
{
  int seconds;
  human_duration_t(int s) : seconds(s) {}
};

ostream& operator<<(ostream& s, const human_duration_t& d)
{
  int remainder = d.seconds;
  if (remainder > 3600) {
    int hours = remainder / 3600;
    s << hours << " h ";
    remainder -= hours * 3600;
  }
  if (remainder > 60) {
    int minutes = remainder / 60;
    s << minutes << " min ";
    remainder -= minutes * 60;
  }
  if (d.seconds < 600) {
    s << remainder << " s";
  }
  return s;
}

void try_run_some_benchmarks(
  vector<benchmark_t>& benchmarks,
  double time_start,
  size_t& first_benchmark_to_run,
  float& max_clock_speed)
{
  if (first_benchmark_to_run == benchmarks.size()) {
    return;
  }

  double time_last_progress_update = 0;
  double time_last_clock_speed_measurement = 0;
  double time_now = 0;

  size_t benchmark_index = first_benchmark_to_run;

  while (true) {
    float ratio_done = float(benchmark_index) / benchmarks.size();
    time_now = timer.getRealTime();

    // We check clock speed every minute and at the end.
    if (benchmark_index == benchmarks.size() ||
        time_now > time_last_clock_speed_measurement + 60.0f)
    {
      time_last_clock_speed_measurement = time_now;

      // Ensure that clock speed is as expected
      float current_clock_speed = measure_clock_speed();

      // we only allow 1% higher clock speeds, because we want to know the
      // clock speed with good accuracy, and this should only cause restarts
      // at the beginning of the benchmarks run.
      const float tolerance_higher_clock_speed = 1.01f;

      if (current_clock_speed > tolerance_higher_clock_speed * max_clock_speed) {
        // Clock speed is now higher than we previously measured.
        // Either our initial measurement was inaccurate, which won't happen
        // too many times as we are keeping the best clock speed value and
        // and allowing some tolerance; or something really weird happened,
        // which invalidates all benchmark results collected so far.
        // Either way, we better restart all over again now.
        if (benchmark_index) {
          cerr << "Restarting at " << 100.0f * ratio_done
               << " % because clock speed increased.          " << endl;
        }
        max_clock_speed = current_clock_speed;
        first_benchmark_to_run = 0;
        return;
      }

      // we are a bit more tolerant to lower clock speeds because we don't want
      // to cause sleeps and reruns all the time.
      const float tolerance_lower_clock_speed = 0.98f;
      bool rerun_last_tests = false;

      if (current_clock_speed < tolerance_lower_clock_speed * max_clock_speed) {
        cerr << "Measurements completed so far: "
             << 100.0f * ratio_done
             << " %                             " << endl;
        cerr << "Clock speed seems to be only "
             << current_clock_speed/max_clock_speed
             << " times what it used to be." << endl;

        unsigned int seconds_to_sleep_if_lower_clock_speed = 1;

        while (current_clock_speed < tolerance_lower_clock_speed * max_clock_speed) {
          if (seconds_to_sleep_if_lower_clock_speed > 300) {
            cerr << "Sleeping longer probably won't make a difference. Giving up." << endl;
            cerr << "Things to try:" << endl;
            cerr << " 1. Check if the device is in some energy-saving state." << endl;
            cerr << "    On Android, it may help to enable 'Stay Awake' in the dev settings." << endl;
            cerr << " 2. Check if the device is overheating." << endl;
            cerr << "    On some devices, system temperature is reported in" << endl;
            cerr << "    /sys/class/thermal/thermal_zone*/temp" << endl;
            cerr << " 3. Some system daemon might be playing with clock speeds." << endl;
            cerr << "    In particular, on Qualcomm devices, disable mpdecision " << endl;
            cerr << "    by renaming /system/bin/mpdecision and rebooting." << endl;
            cerr << " 4. CPU frequency scaling might conceivably be the problem." << endl;
            cerr << "    In particular, Intel Turbo Boost. Try disabling that." << endl;
            exit(1);
          }
          rerun_last_tests = true;
          cerr << "Sleeping "
               << seconds_to_sleep_if_lower_clock_speed
               << " s..." << endl;
          sleep(seconds_to_sleep_if_lower_clock_speed);
          current_clock_speed = measure_clock_speed();
          seconds_to_sleep_if_lower_clock_speed *= 2;
        }
      }

      if (rerun_last_tests) {
        cerr << "Redoing the last "
             << 100.0f * float(benchmark_index - first_benchmark_to_run) / benchmarks.size()
             << " % because clock speed had been low.   " << endl;
        return;
      }

      // nothing wrong with the clock speed so far, so there won't be a need to rerun
      // benchmarks run so far in case we later encounter a lower clock speed.
      first_benchmark_to_run = benchmark_index;
    }

    if (benchmark_index == benchmarks.size()) {
      // We're done!
      first_benchmark_to_run = benchmarks.size();
      // Erase progress info
      cerr << "                                                            " << endl;
      return;
    }

    // Display progress info on stderr
    if (time_now > time_last_progress_update + 1.0f) {
      time_last_progress_update = time_now;
      cerr << "Measurements... " << 100.0f * ratio_done
           << " %, ETA "
           << human_duration_t(float(time_now - time_start) * (1.0f - ratio_done) / ratio_done)
           << "                          \r" << flush;
    }

    // This is where we actually run a benchmark!
    benchmarks[benchmark_index].run();
    benchmark_index++;
  }
}

void run_benchmarks(vector<benchmark_t>& benchmarks)
{
  // Randomly shuffling benchmarks allows us to get accurate enough progress info,
  // as now the cheap/expensive benchmarks are randomly mixed so they average out.
  // It also means that if data is corrupted for some time span, the odds are that
  // not all repetitions of a given benchmark will be corrupted.
  random_shuffle(benchmarks.begin(), benchmarks.end());

  float max_clock_speed = 0.0f;
  for (int i = 0; i < 4; i++) {
    max_clock_speed = max(max_clock_speed, measure_clock_speed());
  }
  
  double time_start = timer.getRealTime();
  size_t first_benchmark_to_run = 0;
  while (first_benchmark_to_run < benchmarks.size()) {
    try_run_some_benchmarks(benchmarks,
                            time_start,
                            first_benchmark_to_run,
                            max_clock_speed);
  }

  // Sort timings by increasing benchmark parameters, and decreasing gflops.
  // The latter is very important. It means that we can ignore all but the first
  // benchmark with given parameters.
  sort(benchmarks.begin(), benchmarks.end());

  // Collect best (i.e. now first) results for each parameter values.
  vector<benchmark_t> best_benchmarks;
  for (auto it = benchmarks.begin(); it != benchmarks.end(); ++it) {
    if (best_benchmarks.empty() ||
        best_benchmarks.back().compact_product_size != it->compact_product_size ||
        best_benchmarks.back().compact_block_size != it->compact_block_size)
    {
      best_benchmarks.push_back(*it);
    }
  }

  // keep and return only the best benchmarks
  benchmarks = best_benchmarks;
}

struct measure_all_pot_sizes_action_t : action_t
{
  virtual const char* invokation_name() const { return "all-pot-sizes"; }
  virtual void run() const
  {
    vector<benchmark_t> benchmarks;
    for (int repetition = 0; repetition < measurement_repetitions; repetition++) {
      for (size_t ksize = minsize; ksize <= maxsize; ksize *= 2) {
        for (size_t msize = minsize; msize <= maxsize; msize *= 2) {
          for (size_t nsize = minsize; nsize <= maxsize; nsize *= 2) {
            for (size_t kblock = minsize; kblock <= ksize; kblock *= 2) {
              for (size_t mblock = minsize; mblock <= msize; mblock *= 2) {
                for (size_t nblock = minsize; nblock <= nsize; nblock *= 2) {
                  benchmarks.emplace_back(ksize, msize, nsize, kblock, mblock, nblock);
                }
              }
            }
          }
        }
      }
    }

    run_benchmarks(benchmarks);

    cout << "BEGIN MEASUREMENTS ALL POT SIZES" << endl;
    for (auto it = benchmarks.begin(); it != benchmarks.end(); ++it) {
      cout << *it << endl;
    }
  }
};

struct measure_default_sizes_action_t : action_t
{
  virtual const char* invokation_name() const { return "default-sizes"; }
  virtual void run() const
  {
    vector<benchmark_t> benchmarks;
    for (int repetition = 0; repetition < measurement_repetitions; repetition++) {
      for (size_t ksize = minsize; ksize <= maxsize; ksize *= 2) {
        for (size_t msize = minsize; msize <= maxsize; msize *= 2) {
          for (size_t nsize = minsize; nsize <= maxsize; nsize *= 2) {
            benchmarks.emplace_back(ksize, msize, nsize);
          }
        }
      }
    }

    run_benchmarks(benchmarks);

    cout << "BEGIN MEASUREMENTS DEFAULT SIZES" << endl;
    for (auto it = benchmarks.begin(); it != benchmarks.end(); ++it) {
      cout << *it << endl;
    }
  }
};

int main(int argc, char* argv[])
{
  double time_start = timer.getRealTime();
  cout.precision(4);
  cerr.precision(4);

  vector<unique_ptr<action_t>> available_actions;
  available_actions.emplace_back(new measure_all_pot_sizes_action_t);
  available_actions.emplace_back(new measure_default_sizes_action_t);

  auto action = available_actions.end();

  if (argc <= 1) {
    show_usage_and_exit(argc, argv, available_actions);
  }
  for (auto it = available_actions.begin(); it != available_actions.end(); ++it) {
    if (!strcmp(argv[1], (*it)->invokation_name())) {
      action = it;
      break;
    }
  }

  if (action == available_actions.end()) {
    show_usage_and_exit(argc, argv, available_actions);
  }

  for (int i = 2; i < argc; i++) {
    if (argv[i] == strstr(argv[i], "--min-working-set-size=")) {
      const char* equals_sign = strchr(argv[i], '=');
      g_min_working_set_size = strtoul(equals_sign+1, nullptr, 10);
    } else {
      cerr << "unrecognized option: " << argv[i] << endl << endl;
      show_usage_and_exit(argc, argv, available_actions);
    }
  }

  print_cpuinfo();

  cout << "benchmark parameters:" << endl;
  cout << "pointer size: " << 8*sizeof(void*) << " bits" << endl;
  cout << "scalar type: " << type_name<Scalar>() << endl;
  cout << "packet size: " << internal::packet_traits<MatrixType::Scalar>::size << endl;
  cout << "minsize = " << minsize << endl;
  cout << "maxsize = " << maxsize << endl;
  cout << "measurement_repetitions = " << measurement_repetitions << endl;
  cout << "g_min_accurate_time = " << g_min_accurate_time << endl;
  cout << "g_min_working_set_size = " << g_min_working_set_size;
  if (g_min_working_set_size == 0) {
    cout << " (try to outsize caches)";
  }
  cout << endl << endl;

  (*action)->run();

  double time_end = timer.getRealTime();
  cerr << "Finished in " << human_duration_t(time_end - time_start) << endl;
}
