// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

// Copyright (c) 2012 Howard Chu @ Symas Corp. Terms as above.

#include <stdio.h>
#include <stdlib.h>
#include <lmdb.h>
#include "port/port.h"
#include "util/histogram.h"
#include "util/mutexlock.h"
#include "util/random.h"
#include "util/testutil.h"

// Comma-separated list of operations to run in the specified order
//   Actual benchmarks:
//
//   fillseq       -- write N values in sequential key order in async mode
//   fillrandom    -- write N values in random key order in async mode
//   fillrandint   -- write N values in random binary key order in async mode
//   overwrite     -- overwrite N values in random key order in async mode
//   fillseqsync   -- write N/100 values in sequential key order in sync mode
//   fillseqbatch  -- batch write N values in sequential key order in async mode
//   fillrandsync  -- write N/100 values in random key order in sync mode
//   fillrandbatch  -- batch write N values in random key order in async mode
//   fillrandibatch  -- batch write N values in random binary key order in async mode
//   fillrand100K  -- write N/1000 100K values in random order in async mode
//   fillseq100K   -- write N/1000 100K values in seq order in async mode
//   readseq       -- read N times sequentially
//   readreverse   -- read N times in reverse order
//   readseq100K   -- read N/1000 100K values in sequential order in async mode
//   readrand100K  -- read N/1000 100K values in sequential order in async mode
//   readrandom    -- read N times in random order
static const char* FLAGS_benchmarks =
    "fillrandsync,"
    "fillrandom,"
    "fillrandbatch,"
    "fillseqsync,"
    "fillseq,"
    "fillseqbatch,"
    "overwrite,"
#if 0
    "overwritebatch,"
#endif
    "readrandom,"
    "readseq,"
    "readreverse,"
#if 0
    "fillrand100K,"
    "fillseq100K,"
    "readseq100K,"
    "readrand100K,"
#endif
    ;

// Number of key/values to place in database
static int FLAGS_num = 1000000;

// Number of read operations to do.  If negative, do FLAGS_num reads.
static int FLAGS_reads = -1;

static int FLAGS_cleanmem = 0;

// Number of concurrent threads to run.
static int FLAGS_threads = 1;

// Size of each value
static int FLAGS_value_size = 100;

// Number of key/values to place in database
static int FLAGS_batch = 1000;

// Arrange to generate values that shrink to this fraction of
// their original size after compression
static double FLAGS_compression_ratio = 0.5;

// Print histogram of operation timings
static bool FLAGS_histogram = false;

// Page size. Default 1 KB
static int FLAGS_page_size = 1024;

// If true, do not destroy the existing database.  If you set this
// flag and also specify a benchmark that wants a fresh database, that
// benchmark will fail.
static bool FLAGS_use_existing_db = false;

// If true, we allow batch writes to occur
static bool FLAGS_transaction = true;

// If false, skip sync of meta pages on synchronous writes
static bool FLAGS_metasync = true;

// If true, use writable mmap
static bool FLAGS_writemap = true;

// The Linux kernel does readahead by default
static bool FLAGS_readahead = true;

// Use the db with the following name.
static const char* FLAGS_db = NULL;

// If true, use shuffle instead of original random algorithm.
// Guarantees full set of unique data, but uses memory for the
// shuffle array, not feasible for larger tests.
static bool FLAGS_shuffle = false;

static int *shuff = NULL;

namespace leveldb {

// Helper for quickly generating random data.
namespace {
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

  Slice Generate(int len) {
    if (pos_ + len > data_.size()) {
      pos_ = 0;
      assert(len < data_.size());
    }
    pos_ += len;
    return Slice(data_.data() + pos_ - len, len);
  }
};

static Slice TrimSpace(Slice s) {
  int start = 0;
  while (start < s.size() && isspace(s[start])) {
    start++;
  }
  int limit = s.size();
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
  double start_;
  double finish_;
  double seconds_;
  int done_;
  int next_report_;
  int64_t bytes_;
  double last_op_finish_;
  Histogram hist_;
  std::string message_;

 public:
  Stats() { Start(); }

  void Start() {
    next_report_ = 100;
    last_op_finish_ = start_;
    hist_.Clear();
    done_ = 0;
    bytes_ = 0;
    seconds_ = 0;
    start_ = Env::Default()->NowMicros();
    finish_ = start_;
    message_.clear();
  }

  void Merge(const Stats& other) {
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
      if      (next_report_ < 1000)   next_report_ += 100;
      else if (next_report_ < 5000)   next_report_ += 500;
      else if (next_report_ < 10000)  next_report_ += 1000;
      else if (next_report_ < 50000)  next_report_ += 5000;
      else if (next_report_ < 100000) next_report_ += 10000;
      else if (next_report_ < 500000) next_report_ += 50000;
      else                            next_report_ += 100000;
      fprintf(stderr, "... finished %d ops%30s\r", done_, "");
      fflush(stderr);
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
    if (bytes_ > 0) {
      // Rate is computed on actual elapsed time, not the sum of per-thread
      // elapsed times.
      double elapsed = (finish_ - start_) * 1e-6;
      char rate[100];
      snprintf(rate, sizeof(rate), "%6.1f MB/s",
               (bytes_ / 1048576.0) / elapsed);
      extra = rate;
    }
    AppendWithSpace(&extra, message_);
    double elapsed = (finish_ - start_) * 1e-6;
    double throughput = (double)done_/elapsed;

    fprintf(stdout, "%-12s : %11.3f micros/op %ld ops/sec;%s%s\n",
            name.ToString().c_str(),
            seconds_ * 1e6 / done_,
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

  ThreadState(int index)
      : tid(index),
        rand(1000 + index) {
  }
};
}  // namespace

class Benchmark {
 public:
  enum Order {
    SEQUENTIAL,
    RANDOM
  };
  enum DBFlags {
    NONE = 0,
	SYNC,
	INT
  };

 private:
  MDB_env *db_;
  MDB_dbi dbi_;
  int db_num_;
  int num_;
  int value_size_;
  int entries_per_batch_;
  int reads_;
  DBFlags dbflags_;
  Order write_order_;

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
  }

  void PrintEnvironment() {
    fprintf(stderr, "MDB:    version %s\n", mdb_version(NULL, NULL, NULL));

#if defined(__linux)
    time_t now = time(NULL);
    fprintf(stderr, "Date:           %s", ctime(&now));  // ctime() adds newline

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
      fprintf(stderr, "CPU:            %d * %s\n", num_cpus, cpu_type.c_str());
      fprintf(stderr, "CPUCache:       %s\n", cache_size.c_str());
    }
#endif
  }

 public:
  Benchmark()
  : db_(NULL),
    db_num_(0),
    num_(FLAGS_num),
	value_size_(FLAGS_value_size),
	entries_per_batch_(1),
    reads_(FLAGS_reads < 0 ? FLAGS_num : FLAGS_reads) {
    std::vector<std::string> files;
    std::string test_dir;
    Env::Default()->GetTestDirectory(&test_dir);
    Env::Default()->GetChildren(test_dir.c_str(), &files);
    if (!FLAGS_use_existing_db) {
      for (int i = 0; i < files.size(); i++) {
        if (Slice(files[i]).starts_with("dbbench_mdb")) {
          std::string file_name(test_dir);
          file_name += "/";
          file_name += files[i];
          Env::Default()->DeleteFile(file_name.c_str());
        }
      }
    }
  }

  ~Benchmark() {
  	mdb_env_close(db_);
  }

  void Run() {
    PrintHeader();

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

	  num_ = FLAGS_num;
	  reads_ = (FLAGS_reads < 0 ? FLAGS_num : FLAGS_reads);
	  value_size_ = FLAGS_value_size;
	  entries_per_batch_ = 1;

	  void (Benchmark::*method)(ThreadState*) = NULL;
	  bool fresh_db = false;
	  int num_threads = FLAGS_threads;

	  dbflags_ = NONE;
      if (name == Slice("fillseq")) {
		fresh_db = true;
		write_order_ = SEQUENTIAL;
		method = &Benchmark::Write;
      } else if (name == Slice("fillseqbatch")) {
		fresh_db = true;
		write_order_ = SEQUENTIAL;
		entries_per_batch_ = FLAGS_batch;
		method = &Benchmark::Write;
      } else if (name == Slice("fillrandom")) {
		fresh_db = true;
		write_order_ = RANDOM;
		method = &Benchmark::Write;
      } else if (name == Slice("fillrandbatch")) {
		fresh_db = true;
		write_order_ = RANDOM;
		entries_per_batch_ = FLAGS_batch;
		method = &Benchmark::Write;
      } else if (name == Slice("fillrandint")) {
		fresh_db = true;
		write_order_ = RANDOM;
		dbflags_ = INT;
		method = &Benchmark::Write;
      } else if (name == Slice("fillrandibatch")) {
		fresh_db = true;
		write_order_ = RANDOM;
		dbflags_ = INT;
		entries_per_batch_ = FLAGS_batch;
		method = &Benchmark::Write;
      } else if (name == Slice("overwrite")) {
		write_order_ = RANDOM;
		method = &Benchmark::Write;
      } else if (name == Slice("overwritebatch")) {
		write_order_ = RANDOM;
		entries_per_batch_ = FLAGS_batch;
		method = &Benchmark::Write;
      } else if (name == Slice("fillrandsync")) {
		fresh_db = true;
#if 1
		num_ /= 1000;
		if (num_<10) num_=10;
#endif
		write_order_ = RANDOM;
        dbflags_ = SYNC;
		method = &Benchmark::Write;
      } else if (name == Slice("fillseqsync")) {
		fresh_db = true;
#if 1
		num_ /= 1000;
		if (num_<10) num_=10;
#endif
		write_order_ = SEQUENTIAL;
        dbflags_ = SYNC;
		method = &Benchmark::Write;
      } else if (name == Slice("fillrand100K")) {
		fresh_db = true;
		write_order_ = RANDOM;
		num_ /= 1000;
		value_size_ = 100 * 1000;
		method = &Benchmark::Write;
      } else if (name == Slice("fillseq100K")) {
		fresh_db = true;
		write_order_ = SEQUENTIAL;
		num_ /= 1000;
		value_size_ = 100 * 1000;
		method = &Benchmark::Write;
      } else if (name == Slice("readseq")) {
        method = &Benchmark::ReadSequential;
      } else if (name == Slice("readreverse")) {
        method = &Benchmark::ReadReverse;
      } else if (name == Slice("readrandom")) {
        method = &Benchmark::ReadRandom;
      } else if (name == Slice("readrand100K")) {
        reads_ /= 1000;
        method = &Benchmark::ReadRandom;
      } else if (name == Slice("readseq100K")) {
        reads_ /= 1000;
        method = &Benchmark::ReadSequential;
      } else if (name == Slice("readwhilewriting")) {
	    num_threads++;
        method = &Benchmark::ReadWhileWriting;
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
      }
	  if (db_) {
		  char cmd[200];
		  sprintf(cmd, "rm -rf %s*", FLAGS_db);
		  mdb_env_close(db_);
		  system(cmd);
		  db_ = NULL;
	  }
    }
	if (!db_)
		Open(dbflags_);

	if (method != NULL) {
		RunBenchmark(num_threads, name, method);
		if (method == &Benchmark::Write) {
		  char cmd[200];
		  std::string test_dir;
		  Env::Default()->GetTestDirectory(&test_dir);
		  sprintf(cmd, "du %s", test_dir.c_str());
		  system(cmd);
		}
    }
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

    thread->stats.Start();
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
      arg[i].thread = new ThreadState(i);
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

    void Open(DBFlags flags) {
    assert(db_ == NULL);
	int rc;
	MDB_txn *txn;

    char file_name[100], cmd[200];
    db_num_++;
    std::string test_dir;
    Env::Default()->GetTestDirectory(&test_dir);
    snprintf(file_name, sizeof(file_name),
             "%s/dbbench_mdb-%d",
             test_dir.c_str(),
             db_num_);

	sprintf(cmd, "mkdir -p %s", file_name);
	system(cmd);

	int env_opt = 0;
	size_t msize;
	if (flags != SYNC)
		env_opt = MDB_NOSYNC;
	else if (!FLAGS_metasync)
		env_opt = MDB_NOMETASYNC;

	if (FLAGS_writemap)
		env_opt |= MDB_WRITEMAP;
#ifdef MDB_CLEANMEM
	if (FLAGS_cleanmem)
		env_opt |= MDB_CLEANMEM;
#endif
	if (!FLAGS_readahead)
		env_opt |= MDB_NORDAHEAD;

    // Create tuning options and open the database
	rc = mdb_env_create(&db_);
	msize = FLAGS_num*32L*FLAGS_value_size/10;
	rc = mdb_env_set_mapsize(db_, msize);
	rc = mdb_env_open(db_, file_name, env_opt, 0664);
	if (rc) {
      fprintf(stderr, "open error: %s\n", mdb_strerror(rc));
    }
	rc = mdb_txn_begin(db_, NULL, 0, &txn);
	rc = mdb_open(txn, NULL, flags == INT ? MDB_INTEGERKEY:0, &dbi_);
	rc = mdb_txn_commit(txn);
  }

  void Write(ThreadState *thread) {
    if (num_ != FLAGS_num) {
      char msg[100];
      snprintf(msg, sizeof(msg), "(%d ops)", num_);
      thread->stats.AddMessage(msg);
    }

	if (write_order_ == RANDOM && shuff)
	  thread->rand.Shuffle(shuff, num_);

	RandomGenerator gen;
	MDB_val mkey, mval;
	MDB_txn *txn;
	char key[100];
	int ikey, flag = 0;
	if (dbflags_ == INT) {
		mkey.mv_data = &ikey;
		mkey.mv_size = sizeof(ikey);
	} else {
		mkey.mv_data = key;
	}
	mval.mv_size = value_size_;
	if (write_order_ == SEQUENTIAL)
		flag = MDB_APPEND;
	int64_t bytes = 0;
//	flag |= MDB_RESERVE;
    // Write to database
    for (int i = 0; i < num_; i+= entries_per_batch_)
    {
	  MDB_cursor *mc;
	  mdb_txn_begin(db_, NULL, 0, &txn);
	  mdb_cursor_open(txn, dbi_, &mc);
	  
	  for (int j=0; j < entries_per_batch_; j++) {

      const int k = (write_order_ == SEQUENTIAL) ? i+j : (shuff ? shuff[i+j] : (thread->rand.Next() % FLAGS_num));
	  int rc;
	  if (dbflags_ == INT)
	  	  ikey = k;
	  else
		  mkey.mv_size = snprintf(key, sizeof(key), "%016d", k);
      bytes += value_size_ + mkey.mv_size;

	  mval.mv_data = (void *)gen.Generate(value_size_).data();
	  mval.mv_size = value_size_;
	  rc = mdb_cursor_put(mc, &mkey, &mval, flag);
      if (rc) {
        fprintf(stderr, "set error: %s\n", mdb_strerror(rc));
      }
      thread->stats.FinishedSingleOp();
	  }
	  mdb_cursor_close(mc);
	  mdb_txn_commit(txn);
    }
	thread->stats.AddBytes(bytes);
  }

  void ReadReverse(ThreadState *thread) {
    MDB_txn *txn;
	MDB_cursor *cursor;
	MDB_val key, data;
	int64_t bytes = 0;

	mdb_txn_begin(db_, NULL, MDB_RDONLY, &txn);
	mdb_cursor_open(txn, dbi_, &cursor);
    while (mdb_cursor_get(cursor, &key, &data, MDB_PREV) == 0) {
      bytes += key.mv_size + data.mv_size;
      thread->stats.FinishedSingleOp();
    }
	mdb_cursor_close(cursor);
	mdb_txn_abort(txn);
	thread->stats.AddBytes(bytes);
  }

  void ReadSequential(ThreadState *thread) {
    MDB_txn *txn;
	MDB_cursor *cursor;
	MDB_val key, data;
	int64_t bytes = 0;

	mdb_txn_begin(db_, NULL, MDB_RDONLY, &txn);
	mdb_cursor_open(txn, dbi_, &cursor);
    while (mdb_cursor_get(cursor, &key, &data, MDB_NEXT) == 0) {
      bytes += key.mv_size + data.mv_size;
      thread->stats.FinishedSingleOp();
    }
	mdb_cursor_close(cursor);
	mdb_txn_abort(txn);
	thread->stats.AddBytes(bytes);
  }

  void ReadRandom(ThreadState *thread) {
    MDB_txn *txn;
	MDB_cursor *cursor;
	MDB_val key, data;
	int64_t bytes = 0;
	int found = 0;
    char ckey[100];
	key.mv_data = ckey;

	mdb_txn_begin(db_, NULL, MDB_RDONLY, &txn);
	mdb_cursor_open(txn, dbi_, &cursor);
	mdb_txn_reset(txn);
    for (int i = 0; i < reads_; i++) {
      const int k = thread->rand.Next() % FLAGS_num;
      key.mv_size = snprintf(ckey, sizeof(ckey), "%016d", k);
	  mdb_txn_renew(txn);
	  if (!mdb_cursor_get(cursor, &key, &data, MDB_SET))
		found++;
      thread->stats.FinishedSingleOp();
	  mdb_txn_reset(txn);
    }
	mdb_cursor_close(cursor);
	mdb_txn_abort(txn);
	char msg[100];
	snprintf(msg, sizeof(msg), "(%d of %d found)", found, num_);
    thread->stats.AddMessage(msg);
  }

  void ReadWhileWriting(ThreadState* thread) {
    if (thread->tid > 0) {
      ReadRandom(thread);
    } else {
      // Special thread that keeps writing until other threads are done.
      RandomGenerator gen;
      while (true) {
        {
          MutexLock l(&thread->shared->mu);
          if (thread->shared->num_done + 1 >= thread->shared->num_initialized) {
            // Other threads have finished
            break;
          }
        }

		MDB_val mkey, mval;
		MDB_txn *txn;
        const int k = thread->rand.Next() % FLAGS_num;
        char key[100];
		int rc;
		mkey.mv_data = key;
        mkey.mv_size = snprintf(key, sizeof(key), "%016d", k);
		mval.mv_data = (void *)gen.Generate(value_size_).data();
		mval.mv_size = value_size_;
		mdb_txn_begin(db_, NULL, 0, &txn);
        rc = mdb_put(txn, dbi_, &mkey, &mval, 0);
        if (rc) {
          fprintf(stderr, "put error: %s\n", mdb_strerror(rc));
          exit(1);
        }
		rc = mdb_txn_commit(txn);
        if (rc) {
          fprintf(stderr, "commit error: %s\n", mdb_strerror(rc));
          exit(1);
        }
      }

      // Do not count any of the preceding work/delay in stats.
      thread->stats.Start();
    }
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
    } else if (sscanf(argv[i], "--use_existing_db=%d%c", &n, &junk) == 1 &&
               (n == 0 || n == 1)) {
      FLAGS_use_existing_db = n;
    } else if (sscanf(argv[i], "--metasync=%d%c", &n, &junk) == 1 &&
               (n == 0 || n == 1)) {
      FLAGS_metasync = n;
    } else if (sscanf(argv[i], "--writemap=%d%c", &n, &junk) == 1 &&
               (n == 0 || n == 1)) {
      FLAGS_writemap = n;
    } else if (sscanf(argv[i], "--num=%d%c", &n, &junk) == 1) {
      FLAGS_num = n;
    } else if (sscanf(argv[i], "--batch=%d%c", &n, &junk) == 1) {
      FLAGS_batch = n;
    } else if (sscanf(argv[i], "--reads=%d%c", &n, &junk) == 1) {
      FLAGS_reads = n;
    } else if (sscanf(argv[i], "--threads=%d%c", &n, &junk) == 1) {
      FLAGS_threads = n;
    } else if (sscanf(argv[i], "--value_size=%d%c", &n, &junk) == 1) {
      FLAGS_value_size = n;
    } else if (sscanf(argv[i], "--cleanmem=%d%c", &n, &junk) == 1) {
      FLAGS_cleanmem = n;
    } else if (strncmp(argv[i], "--db=", 5) == 0) {
      FLAGS_db = argv[i] + 5;
    } else if (sscanf(argv[i], "--shuffle=%d%c", &n, &junk) == 1 &&
               (n == 0 || n == 1)) {
      FLAGS_shuffle = n;
    } else if (sscanf(argv[i], "--readahead=%d%c", &n, &junk) == 1 &&
               (n == 0 || n == 1)) {
      FLAGS_readahead = n;
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
