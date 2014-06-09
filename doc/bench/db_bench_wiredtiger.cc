// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#include <sys/types.h>
#include <stdio.h>
#include <stdlib.h>
#include <sstream>
#include <unistd.h> 
#include <time.h>
#include <errno.h> /* For ETIMEDOUT */
#include "util/crc32c.h"
#include "util/histogram.h"
#include "util/mutexlock.h"
#include "util/random.h"
#include "util/testutil.h"
#include "port/port.h"
#include "wiredtiger.h"

// Comma-separated list of operations to run in the specified order
//   Actual benchmarks:
//      fillseq       -- write N values in sequential key order in async mode
//      fillrandom    -- write N values in random key order in async mode
//      overwrite     -- overwrite N values in random key order in async mode
//      fillsync      -- write N/100 values in random key order in sync mode
//      fill100K      -- write N/1000 100K values in random order in async mode
//      deleteseq     -- delete N keys in sequential order
//      deleterandom  -- delete N keys in random order
//      readseq       -- read N times sequentially
//      readreverse   -- read N times in reverse order
//      readrandom    -- read N times in random order
//      readmissing   -- read N missing keys in random order
//      readhot       -- read N times in random order from 1% section of DB
//      seekrandom    -- N random seeks
//      crc32c        -- repeated crc32c of 4K of data
//      acquireload   -- load N*1000 times
//   Meta operations:
//      compact     -- Compact the entire DB
//      stats       -- Print DB stats
//      sstables    -- Print sstable info
//      heapprofile -- Dump a heap profile (if supported by this port)
static const char* FLAGS_benchmarks =
    "fillrandsync,"
    "fillrandom,"
    "fillrandbatch,"
    "fillseqsync,"
    "fillseq,"
    "fillseqbatch,"
    "overwrite,"
    "readrandom,"
#if 0
    "readrandom,"  // Extra run to allow previous compactions to quiesce
#endif
    "readseq,"
    "readreverse,"
#if 0
    "compact,"
    "readrandom,"
    "readseq,"
    "readreverse,"
    "fill100K,"
    "crc32c,"
    "snappycomp,"
    "snappyuncomp,"
    "acquireload,"
#endif
    ;

// Number of key/values to place in database
static int FLAGS_num = 1000000;

// Number of read operations to do.  If negative, do FLAGS_num reads.
static int FLAGS_reads = -1;

// Number of concurrent threads to run.
static int FLAGS_threads = 1;

// Time in seconds for the random-ops tests to run.
static int FLAGS_duration = 0;

// Per-thread rate limit on writes per second.
// Only for the readwhilewriting test.
static int FLAGS_writes_per_second;

// Stats are reported every N operations when this is
// greater than zero. When 0 the interval grows over time.
static int FLAGS_stats_interval = 0;

// Size of each value
static int FLAGS_value_size = 100;

// Arrange to generate values that shrink to this fraction of
// their original size after compression
static double FLAGS_compression_ratio = 0.5;

// Print histogram of operation timings
static bool FLAGS_histogram = false;

// Number of bytes to buffer in memtable before compacting
// (initialized to default value by "main")
static int FLAGS_write_buffer_size = 0;

// Number of bytes to use as a cache of uncompressed data.
// Negative means use default settings.
static int FLAGS_cache_size = -1;

// Maximum number of files to keep open at the same time (use default if == 0)
static int FLAGS_open_files = 0;

// Bloom filter bits per key.
// Negative means use default settings.
static int FLAGS_bloom_bits = -1;

// Use LSM tree.  Changed by --use_lsm=0
static bool FLAGS_use_lsm = true;

// If true, do not destroy the existing database.  If you set this
// flag and also specify a benchmark that wants a fresh database, that
// benchmark will fail.
static bool FLAGS_use_existing_db = false;

// Stagger starting point of reads for sequential (or reverse).
static bool FLAGS_stagger = false;

// Stagger starting point of reads for sequential (or reverse).
static int FLAGS_max_compact_wait = 1200;

// If true, use shuffle instead of original random algorithm.
// Guarantees full set of unique data, but uses memory for the
// shuffle array, not feasible for larger tests.
static bool FLAGS_shuffle = false;

// Use the db with the following name.
static const char* FLAGS_db = NULL;

static int *shuff = NULL;

namespace leveldb {

namespace {

// Helper for quickly generating random data.
class RandomGenerator {
 private:
  std::string data_;
  int pos_;

 public:
  RandomGenerator() {
    // We use a limited amount of data over and over again and ensure
    // that it is larger than the compression window (32KB), and also
    // large enough to serve all typical value sizes we want to write.
    Random rnd(301);
    std::string piece;
    while (data_.size() < 1048576) {
      // Add a short fragment that is as compressible as specified
      // by FLAGS_compression_ratio.
      test::CompressibleString(&rnd, FLAGS_compression_ratio, 100, &piece);
      data_.append(piece);
    }
    pos_ = 0;
  }

  Slice Generate(size_t len) {
    if (pos_ + len > data_.size()) {
      pos_ = 0;
      assert(len < data_.size());
    }
    pos_ += len;
    return Slice(data_.data() + pos_ - len, len);
  }
};

static Slice TrimSpace(Slice s) {
  size_t start = 0;
  while (start < s.size() && isspace(s[start])) {
    start++;
  }
  size_t limit = s.size();
  while (limit > start && isspace(s[limit-1])) {
    limit--;
  }
  return Slice(s.data() + start, limit - start);
}

static void AppendWithSpace(std::string* str, Slice msg) {
  if (msg.empty()) return;
  if (!str->empty()) {
    str->push_back(' ');
  }
  str->append(msg.data(), msg.size());
}

class Stats {
 private:
  int id_;
  double start_;
  double finish_;
  double seconds_;
  size_t done_;
  size_t last_report_done_;
  int64_t next_report_;
  int64_t bytes_;
  double last_op_finish_;
  double last_report_finish_;
  Histogram hist_;
  std::string message_;
  bool exclude_from_merge_;

 public:
  Stats() { Start(-1); }

  void Start(int id) {
    id_ = id;
    next_report_ = FLAGS_stats_interval ? FLAGS_stats_interval : 100;
    last_op_finish_ = start_;
    hist_.Clear();
    done_ = 0;
	last_report_done_ = 0;
    bytes_ = 0;
    seconds_ = 0;
    start_ = Env::Default()->NowMicros();
    finish_ = start_;
	last_report_finish_ = start_;
    message_.clear();
	// When set, stats from this thread won't be merged with others.
	exclude_from_merge_ = false;
  }

  void Merge(const Stats& other) {
	if (other.exclude_from_merge_)
		return;

    hist_.Merge(other.hist_);
    done_ += other.done_;
    bytes_ += other.bytes_;
    seconds_ += other.seconds_;
    if (other.start_ < start_) start_ = other.start_;
    if (other.finish_ > finish_) finish_ = other.finish_;

    // Just keep the messages from one thread
    if (message_.empty()) message_ = other.message_;
  }

  void Stop() {
    finish_ = Env::Default()->NowMicros();
    seconds_ = (finish_ - start_) * 1e-6;
  }

  void AddMessage(Slice msg) {
    AppendWithSpace(&message_, msg);
  }

  void SetExcludeFromMerge() { exclude_from_merge_ = true; }

  void TimeStr(time_t seconds, char *buf) {
	struct tm tm;
	localtime_r(&seconds, &tm);
	sprintf(buf, "%04d/%02d/%02d-%02d:%02d:%02d",
		tm.tm_year+1900, tm.tm_mon+1, tm.tm_mday,
		tm.tm_hour, tm.tm_min, tm.tm_sec);
  }

  void FinishedSingleOp() {
    if (FLAGS_histogram) {
      double now = Env::Default()->NowMicros();
      double micros = now - last_op_finish_;
      hist_.Add(micros);
      if (micros > 20000) {
        fprintf(stderr, "long op: %.1f micros%30s\r", micros, "");
        fflush(stderr);
      }
      last_op_finish_ = now;
    }

    done_++;
    if (done_ >= next_report_) {
	  if (!FLAGS_stats_interval) {
      if      (next_report_ < 1000)   next_report_ += 100;
      else if (next_report_ < 5000)   next_report_ += 500;
      else if (next_report_ < 10000)  next_report_ += 1000;
      else if (next_report_ < 50000)  next_report_ += 5000;
      else if (next_report_ < 100000) next_report_ += 10000;
      else if (next_report_ < 500000) next_report_ += 50000;
      else                            next_report_ += 100000;
      fprintf(stderr, "... finished %zd ops%30s\r", done_, "");
      fflush(stderr);
	  } else {
        double now = Env::Default()->NowMicros();
		char buf[20];
		TimeStr((int) (now/1000000), buf);
        fprintf(stderr,
                "%s ... thread %d: (%zd,%zd) ops and "
                "(%.1f,%.1f) ops/second in (%.6f,%.6f) seconds\n",
                buf,
                id_,
                done_ - last_report_done_, done_,
                (done_ - last_report_done_) /
                ((now - last_report_finish_) / 1000000.0),
                done_ / ((now - start_) / 1000000.0),
                (now - last_report_finish_) / 1000000.0,
                (now - start_) / 1000000.0);
        fflush(stderr);
        next_report_ += FLAGS_stats_interval;
        last_report_finish_ = now;
        last_report_done_ = done_;
	  }
    }
  }

  void AddBytes(int64_t n) {
    bytes_ += n;
  }

  void Report(const Slice& name) {
    // Pretend at least one op was done in case we are running a benchmark
    // that does not call FinishedSingleOp().
    if (done_ < 1) done_ = 1;

    std::string extra;
    double elapsed = (finish_ - start_) * 1e-6;
    if (bytes_ > 0) {
      // Rate is computed on actual elapsed time, not the sum of per-thread
      // elapsed times.
      char rate[100];
      snprintf(rate, sizeof(rate), "%6.1f MB/s",
               (bytes_ / 1048576.0) / elapsed);
      extra = rate;
    }
    AppendWithSpace(&extra, message_);
    double throughput = (double)done_/elapsed;

    fprintf(stdout, "%-12s : %11.3f micros/op %ld ops/sec;%s%s\n",
            name.ToString().c_str(),
            elapsed * 1e6 / done_,
            (long)throughput,
            (extra.empty() ? "" : " "),
            extra.c_str());
    if (FLAGS_histogram) {
      fprintf(stdout, "Microseconds per op:\n%s\n", hist_.ToString().c_str());
    }
    fflush(stdout);
  }
};

// State shared by all concurrent executions of the same benchmark.
struct SharedState {
  port::Mutex mu;
  port::CondVar cv;
  int total;

  // Each thread goes through the following states:
  //    (1) initializing
  //    (2) waiting for others to be initialized
  //    (3) running
  //    (4) done

  int num_initialized;
  int num_done;
  bool start;

  SharedState() : cv(&mu) { }
};

// Per-thread state for concurrent executions of the same benchmark.
struct ThreadState {
  int tid;             // 0..n-1 when running in n threads
  Random rand;         // Has different seeds for different threads
  Stats stats;
  SharedState* shared;
  WT_SESSION *session;

  ThreadState(int index, WT_CONNECTION *conn)
      : tid(index),
        rand(1000 + index) {
    if (index == 0 && shuff)
		rand.Shuffle(shuff, FLAGS_num);
    conn->open_session(conn, NULL, NULL, &session);
    assert(session != NULL);
  }
  ~ThreadState() {
    session->close(session, NULL);
  }
};

class Duration {
 public:
  Duration(int max_seconds, int64_t max_ops) {
    max_seconds_ = max_seconds;
    max_ops_= max_ops;
    ops_ = 0;
    start_at_ = Env::Default()->NowMicros();
  }

  bool Done(int64_t increment) {
    if (increment <= 0) increment = 1;    // avoid Done(0) and infinite loops
    ops_ += increment;

    if (max_seconds_) {
      // Recheck every appx 1000 ops (exact iff increment is factor of 1000)
      if ((ops_/1000) != ((ops_-increment)/1000)) {
        double now = Env::Default()->NowMicros();
        return ((now - start_at_) / 1000000.0) >= max_seconds_;
      } else {
        return false;
      }
    } else {
      return ops_ > max_ops_;
    }
  }

 private:
  int max_seconds_;
  int64_t max_ops_;
  int64_t ops_;
  double start_at_;
};
}  // namespace

class Benchmark {
 private:
  WT_CONNECTION *conn_;
  std::string uri_;
  int db_num_;
  int num_;
  int value_size_;
  int entries_per_batch_;
  int sync_;
  int reads_;
  int heap_counter_;

  void PrintHeader() {
    const int kKeySize = 16;
    PrintEnvironment();
    fprintf(stdout, "Keys:       %d bytes each\n", kKeySize);
    fprintf(stdout, "Values:     %d bytes each (%d bytes after compression)\n",
            FLAGS_value_size,
            static_cast<int>(FLAGS_value_size * FLAGS_compression_ratio + 0.5));
    fprintf(stdout, "Entries:    %d\n", num_);
    fprintf(stdout, "RawSize:    %.1f MB (estimated)\n",
            ((static_cast<int64_t>(kKeySize + FLAGS_value_size) * num_)
             / 1048576.0));
    fprintf(stdout, "FileSize:   %.1f MB (estimated)\n",
            (((kKeySize + FLAGS_value_size * FLAGS_compression_ratio) * num_)
             / 1048576.0));
    PrintWarnings();
    fprintf(stdout, "------------------------------------------------\n");
  }

  void PrintWarnings() {
#if defined(__GNUC__) && !defined(__OPTIMIZE__)
    fprintf(stdout,
            "WARNING: Optimization is disabled: benchmarks unnecessarily slow\n"
            );
#endif
#ifndef NDEBUG
    fprintf(stdout,
            "WARNING: Assertions are enabled; benchmarks unnecessarily slow\n");
#endif

    // See if snappy is working by attempting to compress a compressible string
    const char text[] = "yyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyy";
    std::string compressed;
    if (!port::Snappy_Compress(text, sizeof(text), &compressed)) {
      fprintf(stdout, "WARNING: Snappy compression is not enabled\n");
    } else if (compressed.size() >= sizeof(text)) {
      fprintf(stdout, "WARNING: Snappy compression is not effective\n");
    }
  }

  void PrintEnvironment() {
    int wtmaj, wtmin, wtpatch;
    const char *wtver = wiredtiger_version(&wtmaj, &wtmin, &wtpatch);
    fprintf(stdout, "WiredTiger:    version %s, lib ver %d, lib rev %d patch %d\n",
      wtver, wtmaj, wtmin, wtpatch);
    fprintf(stderr, "WiredTiger:    version %s, lib ver %d, lib rev %d patch %d\n",
      wtver, wtmaj, wtmin, wtpatch);

#if defined(__linux)
    time_t now = time(NULL);
    fprintf(stderr, "Date:       %s", ctime(&now));  // ctime() adds newline

    FILE* cpuinfo = fopen("/proc/cpuinfo", "r");
    if (cpuinfo != NULL) {
      char line[1000];
      int num_cpus = 0;
      std::string cpu_type;
      std::string cache_size;
      while (fgets(line, sizeof(line), cpuinfo) != NULL) {
        const char* sep = strchr(line, ':');
        if (sep == NULL) {
          continue;
        }
        Slice key = TrimSpace(Slice(line, sep - 1 - line));
        Slice val = TrimSpace(Slice(sep + 1));
        if (key == "model name") {
          ++num_cpus;
          cpu_type = val.ToString();
        } else if (key == "cache size") {
          cache_size = val.ToString();
        }
      }
      fclose(cpuinfo);
      fprintf(stderr, "CPU:        %d * %s\n", num_cpus, cpu_type.c_str());
      fprintf(stderr, "CPUCache:   %s\n", cache_size.c_str());
    }
#endif
  }

 public:
  Benchmark()
  : num_(FLAGS_num),
    value_size_(FLAGS_value_size),
    db_num_(0),
    entries_per_batch_(1),
    reads_(FLAGS_reads < 0 ? FLAGS_num : FLAGS_reads),
    heap_counter_(0) {
    std::vector<std::string> files;
    Env::Default()->GetChildren(FLAGS_db, &files);
    for (int i = 0; i < files.size(); i++) {
      if (Slice(files[i]).starts_with("heap-")) {
        Env::Default()->DeleteFile(std::string(FLAGS_db) + "/" + files[i]);
      }
    }
    if (!FLAGS_use_existing_db) {
      for (int i = 0; i < files.size(); i++) {
        std::string file_name(FLAGS_db);
        file_name += "/";
        file_name += files[i];
        Env::Default()->DeleteFile(file_name.c_str());
      }
    }
  }

  ~Benchmark() {
  }

  void Run() {
    PrintHeader();
    Open();

    const char* benchmarks = FLAGS_benchmarks;
    while (benchmarks != NULL) {
      const char* sep = strchr(benchmarks, ',');
      Slice name;
      if (sep == NULL) {
        name = benchmarks;
        benchmarks = NULL;
      } else {
        name = Slice(benchmarks, sep - benchmarks);
        benchmarks = sep + 1;
      }

      // Reset parameters that may be overriddden below
      num_ = FLAGS_num;
      reads_ = (FLAGS_reads < 0 ? FLAGS_num : FLAGS_reads);
      value_size_ = FLAGS_value_size;
      entries_per_batch_ = 1;
      sync_ = false;

      void (Benchmark::*method)(ThreadState*) = NULL;
      bool fresh_db = false;
      int num_threads = FLAGS_threads;

      if (name == Slice("fillseq")) {
        fresh_db = true;
        method = &Benchmark::WriteSeq;
      } else if (name == Slice("fillseqbatch")) {
        fresh_db = true;
        entries_per_batch_ = 1000;
        method = &Benchmark::WriteSeq;
      } else if (name == Slice("fillrandbatch")) {
        fresh_db = true;
        entries_per_batch_ = 1000;
        method = &Benchmark::WriteRandom;
      } else if (name == Slice("fillrandom")) {
        fresh_db = true;
        method = &Benchmark::WriteRandom;
      } else if (name == Slice("overwrite")) {
        fresh_db = false;
        method = &Benchmark::WriteRandom;
      } else if (name == Slice("fillseqsync")) {
        fresh_db = true;
#if 1
        num_ /= 1000;
		if (num_<10) num_=10;
#endif
        sync_ = true;
        method = &Benchmark::WriteSeq;
      } else if (name == Slice("fillrandsync")) {
        fresh_db = true;
#if 1
        num_ /= 1000;
		if (num_<10) num_=10;
#endif
        sync_ = true;
        method = &Benchmark::WriteRandom;
      } else if (name == Slice("fill100K")) {
        fresh_db = true;
        num_ /= 1000;
        value_size_ = 100 * 1000;
        method = &Benchmark::WriteRandom;
      } else if (name == Slice("readseq")) {
        method = &Benchmark::ReadSequential;
      } else if (name == Slice("readreverse")) {
        method = &Benchmark::ReadReverse;
      } else if (name == Slice("readrandom")) {
        method = &Benchmark::ReadRandom;
      } else if (name == Slice("readmissing")) {
        method = &Benchmark::ReadMissing;
      } else if (name == Slice("seekrandom")) {
        method = &Benchmark::SeekRandom;
      } else if (name == Slice("readhot")) {
        method = &Benchmark::ReadHot;
      } else if (name == Slice("readrandomsmall")) {
        reads_ /= 1000;
        method = &Benchmark::ReadRandom;
      } else if (name == Slice("deleteseq")) {
        method = &Benchmark::DeleteSeq;
      } else if (name == Slice("deleterandom")) {
        method = &Benchmark::DeleteRandom;
      } else if (name == Slice("readwhilewriting")) {
        num_threads++;  // Add extra thread for writing
        method = &Benchmark::ReadWhileWriting;
      } else if (name == Slice("compact")) {
        method = &Benchmark::Compact;
      } else if (name == Slice("crc32c")) {
        method = &Benchmark::Crc32c;
      } else if (name == Slice("acquireload")) {
        method = &Benchmark::AcquireLoad;
      } else if (name == Slice("snappycomp")) {
        method = &Benchmark::SnappyCompress;
      } else if (name == Slice("snappyuncomp")) {
        method = &Benchmark::SnappyUncompress;
      } else if (name == Slice("heapprofile")) {
        HeapProfile();
      } else if (name == Slice("stats")) {
        PrintStats("leveldb.stats");
      } else if (name == Slice("sstables")) {
        PrintStats("leveldb.sstables");
      } else {
        if (name != Slice()) {  // No error message for empty name
          fprintf(stderr, "unknown benchmark '%s'\n", name.ToString().c_str());
        }
      }

      if (fresh_db) {
        if (FLAGS_use_existing_db) {
          fprintf(stdout, "%-12s : skipped (--use_existing_db is true)\n",
                  name.ToString().c_str());
          method = NULL;
        } else {
          if (conn_ != NULL) {
            conn_->close(conn_, NULL);
            conn_ = NULL;
          }
          std::vector<std::string> files;
          Env::Default()->GetChildren(FLAGS_db, &files);
          if (!FLAGS_use_existing_db) {
            for (int i = 0; i < files.size(); i++) {
              std::string file_name(FLAGS_db);
              file_name += "/";
              file_name += files[i];
              Env::Default()->DeleteFile(file_name.c_str());
            }
          }
          Open();
        }
      } else if (FLAGS_use_existing_db) {
          /*
     * We get here if we don't want to use a fresh db and
     * don't want to skip this benchmark.  We just want to
     * resue the DB that already exists for the current benchmark.
     */
          if (conn_ != NULL) {
            conn_->close(conn_, NULL);
            conn_ = NULL;
          }
          Open();
    fprintf(stderr, "Reusing existing DB %s\n",uri_.c_str());
      }

      if (method != NULL) {
        RunBenchmark(num_threads, name, method);
  if (method == &Benchmark::WriteSeq ||
      method == &Benchmark::WriteRandom) {
      char cmd[200];
      std::string test_dir;
      Env::Default()->GetTestDirectory(&test_dir);
      sprintf(cmd, "du %s", test_dir.c_str());
      if (system(cmd)) exit(1);
  }
      }
    }
    if (conn_ != NULL) {
      conn_->close(conn_, NULL);
      conn_ = NULL;
    }
  }

 private:
  struct ThreadArg {
    Benchmark* bm;
    SharedState* shared;
    ThreadState* thread;
    void (Benchmark::*method)(ThreadState*);
  };

  static void ThreadBody(void* v) {
    ThreadArg* arg = reinterpret_cast<ThreadArg*>(v);
    SharedState* shared = arg->shared;
    ThreadState* thread = arg->thread;
    {
      MutexLock l(&shared->mu);
      shared->num_initialized++;
      if (shared->num_initialized >= shared->total) {
        shared->cv.SignalAll();
      }
      while (!shared->start) {
        shared->cv.Wait();
      }
    }

    thread->stats.Start(thread->tid);
    (arg->bm->*(arg->method))(thread);
    thread->stats.Stop();

    {
      MutexLock l(&shared->mu);
      shared->num_done++;
      if (shared->num_done >= shared->total) {
        shared->cv.SignalAll();
      }
    }
  }

  void RunBenchmark(int n, Slice name,
                    void (Benchmark::*method)(ThreadState*)) {
    SharedState shared;
    shared.total = n;
    shared.num_initialized = 0;
    shared.num_done = 0;
    shared.start = false;

    ThreadArg* arg = new ThreadArg[n];
    for (int i = 0; i < n; i++) {
      arg[i].bm = this;
      arg[i].method = method;
      arg[i].shared = &shared;
      arg[i].thread = new ThreadState(i, conn_);
      arg[i].thread->shared = &shared;
      Env::Default()->StartThread(ThreadBody, &arg[i]);
    }

    shared.mu.Lock();
    while (shared.num_initialized < n) {
      shared.cv.Wait();
    }

    shared.start = true;
    shared.cv.SignalAll();
    while (shared.num_done < n) {
      shared.cv.Wait();
    }
    shared.mu.Unlock();

    for (int i = 1; i < n; i++) {
      arg[0].thread->stats.Merge(arg[i].thread->stats);
    }
    arg[0].thread->stats.Report(name);

    for (int i = 0; i < n; i++) {
      delete arg[i].thread;
    }
    delete[] arg;
  }

  void Crc32c(ThreadState* thread) {
    // Checksum about 500MB of data total
    const int size = 4096;
    const char* label = "(4K per op)";
    std::string data(size, 'x');
    int64_t bytes = 0;
    uint32_t crc = 0;
    while (bytes < 500 * 1048576) {
      crc = crc32c::Value(data.data(), size);
      thread->stats.FinishedSingleOp();
      bytes += size;
    }
    // Print so result is not dead
    fprintf(stderr, "... crc=0x%x\r", static_cast<unsigned int>(crc));

    thread->stats.AddBytes(bytes);
    thread->stats.AddMessage(label);
  }

  void AcquireLoad(ThreadState* thread) {
    int dummy;
    port::AtomicPointer ap(&dummy);
    int count = 0;
    void *ptr = NULL;
    thread->stats.AddMessage("(each op is 1000 loads)");
    while (count < 100000) {
      for (int i = 0; i < 1000; i++) {
        ptr = ap.Acquire_Load();
      }
      count++;
      thread->stats.FinishedSingleOp();
    }
    if (ptr == NULL) exit(1); // Disable unused variable warning.
  }

  void SnappyCompress(ThreadState* thread) {
    RandomGenerator gen;
    Slice input = gen.Generate(4096);
    int64_t bytes = 0;
    int64_t produced = 0;
    bool ok = true;
    std::string compressed;
    while (ok && bytes < 1024 * 1048576) {  // Compress 1G
      ok = port::Snappy_Compress(input.data(), input.size(), &compressed);
      produced += compressed.size();
      thread->stats.FinishedSingleOp();
      bytes += input.size();
    }

    if (!ok) {
      thread->stats.AddMessage("(snappy failure)");
    } else {
      char buf[100];
      snprintf(buf, sizeof(buf), "(output: %.1f%%)",
               (produced * 100.0) / bytes);
      thread->stats.AddMessage(buf);
      thread->stats.AddBytes(bytes);
    }
  }

  void SnappyUncompress(ThreadState* thread) {
    RandomGenerator gen;
    Slice input = gen.Generate(4096);
    std::string compressed;
    bool ok = port::Snappy_Compress(input.data(), input.size(), &compressed);
    int64_t bytes = 0;
    char* uncompressed = new char[input.size()];
    while (ok && bytes < 1024 * 1048576) {  // Compress 1G
      ok =  port::Snappy_Uncompress(compressed.data(), compressed.size(),
                                    uncompressed);
      thread->stats.FinishedSingleOp();
      bytes += input.size();
    }
    delete[] uncompressed;

    if (!ok) {
      thread->stats.AddMessage("(snappy failure)");
    } else {
      thread->stats.AddBytes(bytes);
    }
  }

  /* Start Wired Tiger modified section. */
  void Open() {
#define SMALL_CACHE 10*1024*1024
    std::stringstream config;
    config.str("");
    if (!FLAGS_use_existing_db)
      config << "create";
    if (FLAGS_cache_size > 0)
      config << ",cache_size=" << FLAGS_cache_size;
    /* TODO: Translate write_buffer_size - maybe it's chunk size?
    options.write_buffer_size = FLAGS_write_buffer_size;
    */
#if 0 /* ndef SYMAS_CONFIG */
    config << ",extensions=[libwiredtiger_snappy.so]";
#endif
    //config << ",verbose=[lsm]";
    Env::Default()->CreateDir(FLAGS_db);
    wiredtiger_open(FLAGS_db, NULL, config.str().c_str(), &conn_);
    assert(conn_ != NULL);

    WT_SESSION *session;
    conn_->open_session(conn_, NULL, NULL, &session);
    assert(session != NULL);

    char uri[100];
    snprintf(uri, sizeof(uri), "%s:dbbench_wt-%d", 
        FLAGS_use_lsm ? "lsm" : "table", ++db_num_);
    uri_ = uri;

    if (!FLAGS_use_existing_db) {
      // Create tuning options and create the data file
      config.str("");
      config << "key_format=S,value_format=S";
      config << ",prefix_compression=false";
      config << ",checksum=off";
      if (FLAGS_cache_size < SMALL_CACHE && FLAGS_cache_size > 0) {
          config << ",internal_page_max=4kb";
          config << ",leaf_page_max=4kb";
        config << ",memory_page_max=" << FLAGS_cache_size;
      } else {
          config << ",internal_page_max=16kb";
          config << ",leaf_page_max=16kb";
        if (FLAGS_cache_size > 0) {
          int memmax = FLAGS_cache_size * 0.75;
          config << ",memory_page_max=" << memmax;
        }
      }

      if (FLAGS_use_lsm) {
        config << ",lsm=(";
        if (FLAGS_cache_size > SMALL_CACHE)
          config << ",chunk_size=20MB";
        if (FLAGS_bloom_bits > 0)
          config << ",bloom_bit_count=" << FLAGS_bloom_bits;
        else if (FLAGS_bloom_bits == 0)
          config << ",bloom=false";
        config << ")";
      }
#if 0 /* ndef SYMAS_CONFIG */
      config << ",block_compressor=snappy";
#endif
      fprintf(stderr, "Creating %s with config %s\n",uri_.c_str(), config.str().c_str());
      int ret = session->create(session, uri_.c_str(), config.str().c_str());
      if (ret != 0) {
        fprintf(stderr, "create error: %s\n", wiredtiger_strerror(ret));
        exit(1);
      }
      session->close(session, NULL);
    }

  }

  void WriteSeq(ThreadState* thread) {
    DoWrite(thread, true);
  }

  void WriteRandom(ThreadState* thread) {
    DoWrite(thread, false);
  }

  void DoWrite(ThreadState* thread, bool seq) {
	const int test_duration = seq ? 0 : FLAGS_duration;

	Duration duration(test_duration, num_);
    if (num_ != FLAGS_num) {
      char msg[100];
      snprintf(msg, sizeof(msg), "(%d ops)", num_);
      thread->stats.AddMessage(msg);
    }

    RandomGenerator gen;
    int64_t bytes = 0;
    int stagger = 0;
    std::stringstream txn_config;
    txn_config.str("");
    txn_config << "isolation=snapshot";
    if (sync_)
        txn_config << ",sync=full";
    else
        txn_config << ",sync=none";

    WT_CURSOR *cursor;
    std::stringstream cur_config;
    cur_config.str("");
    cur_config << "overwrite";
    if (seq && FLAGS_threads == 1)
  cur_config << ",bulk=true";
    if (FLAGS_stagger)
      stagger = (FLAGS_num / FLAGS_threads) * thread->tid;

    int ret = thread->session->open_cursor(thread->session, uri_.c_str(), NULL, cur_config.str().c_str(), &cursor);
    if (ret != 0) {
      fprintf(stderr, "open_cursor error: %s\n", wiredtiger_strerror(ret));
      exit(1);
    }
	int i = 0;
	while (!duration.Done(entries_per_batch_)) {
      for (int j = 0; j < entries_per_batch_; j++) {
        int k = seq ? (i+j+stagger) % FLAGS_num : (shuff ? shuff[(i+j+stagger)%FLAGS_num] : (thread->rand.Next() % FLAGS_num));
        if (k == 0)
          continue; /* Wired Tiger does not support 0 keys. */
        char key[100];
        snprintf(key, sizeof(key), "%016d", k);
        cursor->set_key(cursor, key);
        std::string value = gen.Generate(value_size_).ToString();
        cursor->set_value(cursor, value.c_str());
        int ret = cursor->insert(cursor);
        if (ret != 0) {
          fprintf(stderr, "set error: %s\n", wiredtiger_strerror(ret));
          exit(1);
        }
        thread->stats.FinishedSingleOp();
        bytes += value_size_ + strlen(key);
      }
	  i += entries_per_batch_;
    }
    cursor->close(cursor);
    thread->stats.AddBytes(bytes);
  }

  void ReadSequential(ThreadState* thread) {
    const char *ckey, *cvalue;
    char key[100];
    WT_CURSOR *cursor;
    int ret = thread->session->open_cursor(thread->session, uri_.c_str(), NULL, NULL, &cursor);
    if (ret != 0) {
      fprintf(stderr, "open_cursor error: %s\n", wiredtiger_strerror(ret));
      exit(1);
    }

    int64_t bytes = 0;
    int i = 0;
    int k;
    if (FLAGS_stagger) {
      k = (FLAGS_num / FLAGS_threads) * thread->tid + 1;
      snprintf(key, sizeof(key), "%016d", k);
      cursor->set_key(cursor, key);
      if (cursor->search(cursor) != 0) {
        fprintf(stderr, "cursor search for key %d error: %s\n", k, wiredtiger_strerror(ret));
        exit(1);
      }
    }
repeat:
    while ((ret = cursor->next(cursor)) == 0 && i < reads_) {
      cursor->get_key(cursor, &ckey);
      cursor->get_value(cursor, &cvalue);
      thread->stats.FinishedSingleOp();
      bytes += strlen(ckey) + strlen(cvalue);
      ++i;
    }
    /*
     * Allow repetitive reads, simply wrapping back if the number of
     * reads exceeds the number of keys to read.
     */
    sscanf(ckey, "%d", &k);
    if (k == (FLAGS_num - 1) && i < reads_ && cursor->reset(cursor) == 0)
  goto repeat;
    if (ret != 0) {
      fprintf(stderr, "ReadSeq: cursor_next ckey %s k %d i %d error: %s\n", ckey, k, i, wiredtiger_strerror(ret));
      exit(1);
    }

    cursor->close(cursor);
    thread->stats.AddBytes(bytes);
  }

  void ReadReverse(ThreadState* thread) {
    const char *ckey, *cvalue;
    char key[100];
    WT_CURSOR *cursor;
    int ret = thread->session->open_cursor(thread->session, uri_.c_str(), NULL, NULL, &cursor);
    if (ret != 0) {
      fprintf(stderr, "open_cursor error: %s\n", wiredtiger_strerror(ret));
      exit(1);
    }

    int64_t bytes = 0;
    int i = 0;
    int k;
    if (FLAGS_stagger) {
      k = (FLAGS_num / FLAGS_threads) * thread->tid + 1;
      snprintf(key, sizeof(key), "%016d", k);
      cursor->set_key(cursor, key);
      if (cursor->search(cursor) != 0) {
        fprintf(stderr, "cursor search for key %d error: %s\n", k, wiredtiger_strerror(ret));
        exit(1);
      }
      // Make sure ckey has a valid value in it in case we are at the front
      // and the while loop below is skipped initially.
      cursor->get_key(cursor, &ckey);
    }
repeat:
    while ((ret = cursor->prev(cursor)) == 0 && i < reads_) {
      cursor->get_key(cursor, &ckey);
      cursor->get_value(cursor, &cvalue);
      thread->stats.FinishedSingleOp();
      bytes += strlen(ckey) + strlen(cvalue);
      ++i;
    }
    /*
     * Allow repetitive reads, simply wrapping back if the number of
     * reads exceeds the number of keys to read.
     */
    sscanf(ckey, "%d", &k);
    if (k == 1 && i < reads_ && cursor->reset(cursor) == 0)
  goto repeat;

    if (ret != 0) {
      fprintf(stderr, "ReadReverse: cursor_next ckey %s k %d i %d error: %s\n", ckey, k, i, wiredtiger_strerror(ret));
      exit(1);
    }
    cursor->close(cursor);
    thread->stats.AddBytes(bytes);
  }

  void ReadRandom(ThreadState* thread) {
    const char *ckey;
    WT_CURSOR *cursor;
    int ret = thread->session->open_cursor(thread->session, uri_.c_str(), NULL, NULL, &cursor);
    if (ret != 0) {
      fprintf(stderr, "open_cursor error: %s\n", wiredtiger_strerror(ret));
      exit(1);
    }
    size_t read = 0;
    size_t found = 0;
	Duration duration(FLAGS_duration, reads_);
	while (!duration.Done(1)) {
      char key[100];
      const int k = thread->rand.Next() % FLAGS_num;
      if (k == 0) {
        continue; /* Wired Tiger does not support 0 keys. */
      }
      snprintf(key, sizeof(key), "%016d", k);
      cursor->set_key(cursor, key);
	  read++;
      if (cursor->search(cursor) == 0) {
       found++;
      }
      thread->stats.FinishedSingleOp();
    }
    cursor->close(cursor);
    char msg[100];
    snprintf(msg, sizeof(msg), "(%zd of %zd found)", found, read);
    thread->stats.AddMessage(msg);
  }

  void ReadMissing(ThreadState* thread) {
    const char *ckey;
    WT_CURSOR *cursor;
    int ret = thread->session->open_cursor(thread->session, uri_.c_str(), NULL, NULL, &cursor);
    if (ret != 0) {
      fprintf(stderr, "open_cursor error: %s\n", wiredtiger_strerror(ret));
      exit(1);
    }
    for (int i = 0; i < reads_; i++) {
      char key[100];
      const int k = thread->rand.Next() % FLAGS_num;
      snprintf(key, sizeof(key), "%016d.", k);
      cursor->set_key(cursor, key);
      cursor->search(cursor);
      thread->stats.FinishedSingleOp();
    }
    cursor->close(cursor);
  }

  void ReadHot(ThreadState* thread) {
    const char *ckey;
    WT_CURSOR *cursor;
    int ret = thread->session->open_cursor(thread->session, uri_.c_str(), NULL, NULL, &cursor);
    if (ret != 0) {
      fprintf(stderr, "open_cursor error: %s\n", wiredtiger_strerror(ret));
      exit(1);
    }
    const int range = (FLAGS_num + 99) / 100;
    for (int i = 0; i < reads_; i++) {
      char key[100];
      const int k = thread->rand.Next() % range;
      snprintf(key, sizeof(key), "%016d", k);
      cursor->set_key(cursor, key);
      cursor->search(cursor);
      thread->stats.FinishedSingleOp();
    }
    cursor->close(cursor);
  }

  void SeekRandom(ThreadState* thread) {
    const char *ckey;
    WT_CURSOR *cursor;
    int ret = thread->session->open_cursor(thread->session, uri_.c_str(), NULL, NULL, &cursor);
    if (ret != 0) {
      fprintf(stderr, "open_cursor error: %s\n", wiredtiger_strerror(ret));
      exit(1);
    }
    size_t read = 0;
    size_t found = 0;
	Duration duration(FLAGS_duration, reads_);
	while (!duration.Done(1)) {
      char key[100];
      const int k = thread->rand.Next() % FLAGS_num;
      snprintf(key, sizeof(key), "%016d", k);
      cursor->set_key(cursor, key);
	  read++;
      if(cursor->search(cursor) == 0) {
        found++;
      }
      thread->stats.FinishedSingleOp();
    }
    cursor->close(cursor);
    char msg[100];
    snprintf(msg, sizeof(msg), "(%zd of %zd found)", found, read);
    thread->stats.AddMessage(msg);
  }

  void DoDelete(ThreadState* thread, bool seq) {
    const char *ckey;
    int64_t bytes = 0;
    WT_CURSOR *cursor;
    int ret = thread->session->open_cursor(thread->session, uri_.c_str(), NULL, NULL, &cursor);
    if (ret != 0) {
      fprintf(stderr, "open_cursor error: %s\n", wiredtiger_strerror(ret));
    exit(1);
    }
    std::stringstream txn_config;
    txn_config.str("");
    txn_config << "isolation=snapshot";
    if (sync_)
      txn_config << ",sync=full";
    else
      txn_config << ",sync=none";
	Duration duration(seq ? 0 : FLAGS_duration, num_);
	int64_t i = 0;
    while (!duration.Done(entries_per_batch_)) {
      for (int j = 0; j < entries_per_batch_; j++) {
        int k = seq ? i+j : shuff ? shuff[i+j] : (thread->rand.Next() % FLAGS_num);
        char key[100];
        snprintf(key, sizeof(key), "%016d", k);
        if (k == 0)
          continue; /* Wired Tiger does not support 0 keys. */
  cursor->set_key(cursor, key);
  if ((ret = cursor->remove(cursor)) != 0) {
    if (FLAGS_threads == 1 || ret != WT_NOTFOUND) {
      fprintf(stderr, "del error: key %s %s\n", key, wiredtiger_strerror(ret));
      exit(1);
    }
        }
        thread->stats.FinishedSingleOp();
        bytes += strlen(key);
      }
	  i += entries_per_batch_;
    }
    cursor->close(cursor);
    thread->stats.AddBytes(bytes);
  }

  void DeleteSeq(ThreadState* thread) {
    DoDelete(thread, true);
  }

  void DeleteRandom(ThreadState* thread) {
    DoDelete(thread, false);
  }

  void ReadWhileWriting(ThreadState* thread) {
    if (thread->tid > 0) {
      ReadRandom(thread);
    } else {
      BGWriter(thread);
	}
  }

  void BGWriter(ThreadState *thread) {
    // Special thread that keeps writing until other threads are done.
    RandomGenerator gen;
	double last = Env::Default()->NowMicros();
	int writes_per_second_by_10 = 0;
	int num_writes = 0;

	// --writes_per_second rate limit is enforced per 100 milliseconds
	// intervals to avoid a burst of writes at the start of each second.
	if (FLAGS_writes_per_second > 0)
		writes_per_second_by_10 = FLAGS_writes_per_second / 10;

	// Don't merge stats from this thread with the readers.
	thread->stats.SetExcludeFromMerge();

      while (true) {
        {
          MutexLock l(&thread->shared->mu);
          if (thread->shared->num_done + 1 >= thread->shared->num_initialized) {
            // Other threads have finished
		  // Report and wipe out our own stats
		  char buf[100];
		  snprintf(buf, sizeof(buf), "(desired %d ops/sec)",
			FLAGS_writes_per_second);
		  thread->stats.Stop();
		  thread->stats.AddMessage(buf);
		  thread->stats.Report(Slice("writer"));
		  thread->stats.Start(thread->tid);
            break;
          }
        }

        const char *ckey;
        WT_CURSOR *cursor;
        std::stringstream cur_config;
        cur_config.str("");
        cur_config << "overwrite";
        int ret = thread->session->open_cursor(thread->session, uri_.c_str(), NULL, cur_config.str().c_str(), &cursor);
        if (ret != 0) {
          fprintf(stderr, "open_cursor error: %s\n", wiredtiger_strerror(ret));
        exit(1);
        }
        const int k = thread->rand.Next() % FLAGS_num;
        char key[100];
        snprintf(key, sizeof(key), "%016d", k);
        cursor->set_key(cursor, key);
        std::string value = gen.Generate(value_size_).ToString();
        cursor->set_value(cursor, value.c_str());
        ret = cursor->insert(cursor);
        if (ret != 0) {
          fprintf(stderr, "set error: %s\n", wiredtiger_strerror(ret));
          exit(1);
        }
        cursor->close(cursor);
		thread->stats.FinishedSingleOp();

	  ++num_writes;
	  if (writes_per_second_by_10 && num_writes >= writes_per_second_by_10) {
		double now = Env::Default()->NowMicros();
		double usecs_since_last = now - last;

		num_writes = 0;
		last = now;

		if (usecs_since_last < 100000.0) {
		  Env::Default()->SleepForMicroseconds(100000.0 - usecs_since_last);
		  last = Env::Default()->NowMicros();
		}
      }
    }
  }

  void Compact(ThreadState* thread) {
    /*
     * TODO: It probably makes sense to run multiple compacts in parallel
     * if there are multiple LSM trees. The code to do that involves
     * opening multiple session handles (and possibly multiple threads), so
     * don't do that for now.
     */
    WT_CURSOR *md_cursor;
    int exact, ret;
    const char *key;
    char buf[100]; // For the timeout.

    if (!FLAGS_use_lsm)
      return ;

    if ((ret = thread->session->open_cursor(
      thread->session, "metadata:", NULL, NULL, &md_cursor)) != 0) {
      fprintf(stderr, "open_cursor error: %s\n", wiredtiger_strerror(ret));
      exit(1);
    }
    md_cursor->set_key(md_cursor, "lsm:");
    for (ret = md_cursor->search_near(md_cursor, &exact);
      ret == 0; ret = md_cursor->next(md_cursor)) {
      /* If the initial search result is behind, move our cursor forwards */
      if (exact == -1) {
        ret = md_cursor->next(md_cursor);
        exact = 1;
      }
      ret = md_cursor->get_key(md_cursor, &key);
      if (ret != 0 || strncmp(key, "lsm:", 4) != 0)
        break;

      // TODO: track how long we've been waiting if there are multiple
      // LSM trees to compact.
      snprintf(buf, 100, "timeout=%d", FLAGS_max_compact_wait);
      // Run a compact on this LSM.
      if ((ret = thread->session->compact(thread->session, key, buf)) != 0 &&
        ret != ETIMEDOUT) {
        fprintf(stderr, "compact error: %s\n", wiredtiger_strerror(ret));
        exit(1);
      }
    }
    md_cursor->close(md_cursor);
  }

  void PrintStats(const char* key) {
    /* TODO: Implement two different cases based on input string. */
    const char *ckey, *cvalue;
    WT_CURSOR *cursor;
    std::stringstream suri;
    suri.str("");
    suri << "statistics:" << uri_;
    WT_SESSION *session;
    conn_->open_session(conn_, NULL, NULL, &session);
    int ret = session->open_cursor(session, suri.str().c_str(), NULL, NULL, &cursor);
    if (ret != 0) {
      fprintf(stderr, "open_cursor error: %s\n", wiredtiger_strerror(ret));
      exit(1);
    }

    const char *desc, *pvalue;
    uint64_t value;
    while(cursor->next(cursor) == 0 &&
      cursor->get_value(cursor, &desc, &pvalue, &value) == 0)
      printf("\t%s=%s\n", desc, pvalue);
    session->close(session, NULL);
  }

  /* Only used by HeapProfile.
  static void WriteToFile(void* arg, const char* buf, int n) {
    reinterpret_cast<WritableFile*>(arg)->Append(Slice(buf, n));
  }
  */

  void HeapProfile() {
    fprintf(stderr, "heap profiling not supported\n");
    return;
    /*
    char fname[100];
    snprintf(fname, sizeof(fname), "%s/heap-%04d", FLAGS_db, ++heap_counter_);
    WritableFile* file;
    Status s = Env::Default()->NewWritableFile(fname, &file);
    if (!s.ok()) {
      fprintf(stderr, "%s\n", s.ToString().c_str());
      return;
    }
    bool ok = port::GetHeapProfile(WriteToFile, file);
    delete file;
    if (!ok) {
      fprintf(stderr, "heap profiling not supported\n");
      Env::Default()->DeleteFile(fname);
    }
    */
  }
};

}  // namespace leveldb

int main(int argc, char** argv) {
  std::string default_db_path;

  for (int i = 1; i < argc; i++) {
    double d;
    int n;
    char junk;
    if (leveldb::Slice(argv[i]).starts_with("--benchmarks=")) {
      FLAGS_benchmarks = argv[i] + strlen("--benchmarks=");
    } else if (sscanf(argv[i], "--compression_ratio=%lf%c", &d, &junk) == 1) {
      FLAGS_compression_ratio = d;
    } else if (sscanf(argv[i], "--histogram=%d%c", &n, &junk) == 1 &&
               (n == 0 || n == 1)) {
      FLAGS_histogram = n;
    } else if (sscanf(argv[i], "--use_lsm=%d%c", &n, &junk) == 1 &&
               (n == 0 || n == 1)) {
      FLAGS_use_lsm = n;
    } else if (sscanf(argv[i], "--use_existing_db=%d%c", &n, &junk) == 1 &&
               (n == 0 || n == 1)) {
      FLAGS_use_existing_db = n;
    } else if (sscanf(argv[i], "--max_compact_wait=%d%c", &n, &junk) == 1) {
      FLAGS_max_compact_wait = n;
    } else if (sscanf(argv[i], "--num=%d%c", &n, &junk) == 1) {
      FLAGS_num = n;
    } else if (sscanf(argv[i], "--reads=%d%c", &n, &junk) == 1) {
      FLAGS_reads = n;
    } else if (sscanf(argv[i], "--stagger=%d%c", &n, &junk) == 1 &&
               (n == 0 || n == 1)) {
      FLAGS_stagger = n;
    } else if (sscanf(argv[i], "--threads=%d%c", &n, &junk) == 1) {
      FLAGS_threads = n;
    } else if (sscanf(argv[i], "--duration=%d%c", &n, &junk) == 1) {
      FLAGS_duration = n;
    } else if (sscanf(argv[i], "--stats_interval=%d%c", &n, &junk) == 1) {
      FLAGS_stats_interval = n;
    } else if (sscanf(argv[i], "--writes_per_second=%d%c", &n, &junk) == 1) {
      FLAGS_writes_per_second = n;
    } else if (sscanf(argv[i], "--value_size=%d%c", &n, &junk) == 1) {
      FLAGS_value_size = n;
    } else if (sscanf(argv[i], "--write_buffer_size=%d%c", &n, &junk) == 1) {
      FLAGS_write_buffer_size = n;
    } else if (sscanf(argv[i], "--cache_size=%d%c", &n, &junk) == 1) {
      FLAGS_cache_size = n;
    } else if (sscanf(argv[i], "--bloom_bits=%d%c", &n, &junk) == 1) {
      FLAGS_bloom_bits = n;
    } else if (sscanf(argv[i], "--open_files=%d%c", &n, &junk) == 1) {
      FLAGS_open_files = n;
    } else if (strncmp(argv[i], "--db=", 5) == 0) {
      FLAGS_db = argv[i] + 5;
    } else if (sscanf(argv[i], "--shuffle=%d%c", &n, &junk) == 1 &&
               (n == 0 || n == 1)) {
      FLAGS_shuffle = n;
    } else {
      fprintf(stderr, "Invalid flag '%s'\n", argv[i]);
      exit(1);
    }
  }

  // Choose a location for the test database if none given with --db=<path>
  if (FLAGS_db == NULL) {
      leveldb::Env::Default()->GetTestDirectory(&default_db_path);
      default_db_path += "/dbbench";
      FLAGS_db = default_db_path.c_str();
  }

  if (FLAGS_shuffle) {
  shuff = (int *)malloc(FLAGS_num * sizeof(int));
  for (int i=0; i<FLAGS_num; i++)
      shuff[i] = i;
  }
  leveldb::Benchmark benchmark;
  benchmark.Run();
  return 0;
}
