#include <sqlite3.h>
#include <time.h>
#include <iostream>
#include <functional>
#include <chrono>
#include <random>
#include "primitive.h"

namespace elf {

using namespace std;

typedef int SqlCB(void*,int,char**,char**);

class SharedRWBuffer {
public:
    struct Record {
        uint64_t timestamp = 0;
        uint64_t game_id = 0;
        int seq = 0;
        float pri = 0.0;
        float reward = 0.0;
        string machine;
        string content;
    };

    class Sampler {
    public:
        explicit Sampler(SharedRWBuffer *data) : data_(data), rng_(time(NULL)) {
            data_->rw_lock_.read_shared_lock();
        }
        Sampler(const Sampler &) = delete;
        Sampler(Sampler &&sampler) : data_(sampler.data_), rng_(move(sampler.rng_)) { }

        const Record &sample() {
            const auto *loaded = &data_->get_recent_load();
            if (loaded->empty()) {
                data_->table_read_recent(1000);
                loaded = &data_->get_recent_load();
            }
            int idx = rng_() % loaded->size();
            return loaded->at(idx);
        }

        ~Sampler() {
            data_->rw_lock_.read_shared_unlock();
        }
    private:
        SharedRWBuffer *data_;
        mt19937 rng_;
    };

    SharedRWBuffer(const string &filename, const string& table_name, bool verbose = false)
      : table_name_(table_name), recent_loaded_(2), verbose_(verbose) {
        int rc = sqlite3_open(filename.c_str(), &db_);
        if (rc) {
            cerr << "Can't open database. Filename: " << filename << ", ErrMsg: " << sqlite3_errmsg(db_) << endl;
            throw std::range_error("Cannot open database");
        }
        // Check whether table exists.
        if (! table_exists()) {
            table_create();
        }
    }

    Sampler GetSampler() {
        return Sampler(this);
    }

    bool Insert(const Record &r) {
        return table_insert(r);
    }

    const string &LastError() const { return last_err_; }

    ~SharedRWBuffer() {
        sqlite3_close(db_);
    }

    // Save and load
private:
   sqlite3 *db_ = nullptr;
   const string table_name_;
   string last_err_;

   int curr_idx_ = 0;
   vector<vector<Record>> recent_loaded_;

   mutable RWLock rw_lock_;
   mutable mutex alt_mutex_;

   bool verbose_ = false;

   friend int read_callback(void *, int, char **, char **);

   int exec(const string &sql, SqlCB callback = nullptr) {
       char *zErrMsg;
       if (verbose_) {
           cout << "SQL: " << sql << endl;
       }
       int rc = sqlite3_exec(db_, sql.c_str(), callback, this, &zErrMsg);
       if (rc != 0) {
           last_err_ = zErrMsg;
       } else {
           last_err_ = "";
        }
       sqlite3_free(zErrMsg);
       return rc;
   }

   bool table_exists() {
       const string sql = "SELECT 1 FROM " + table_name_ + " LIMIT 1;";
       return exec(sql) == 0;
   }

   bool table_create() {
       const string sql =
         "CREATE TABLE " + table_name_ + " ("  \
             "TIME           CHAR(20) PRIMARY KEY NOT NULL," \
             "GAME_ID        INT     NOT NULL," \
             "MACHINE        CHAR(80) NOT NULL," \
             "SEQ            INT     NOT NULL," \
             "PRI            REAL    NOT NULL," \
             "REWARD         REAL    NOT NULL," \
             "CONTENT        TEXT);";

       if (exec(sql) != 0) return false;

       const string sql_idx = "CREATE INDEX idx_pri ON " + table_name_ + "(PRI);";
       if (exec(sql_idx) != 0) return false;

       const string sql_reward = "CREATE INDEX idx_reward ON " + table_name_ + "(REWARD);";
       if (exec(sql_reward) != 0) return false;

       return true;
   }

   bool table_insert(const Record &r) {
       auto now = chrono::system_clock::now();
       uint64_t timestamp = chrono::duration_cast<chrono::milliseconds>(now.time_since_epoch()).count();

       const string sql = "INSERT INTO " + table_name_ + " VALUES (" \
          + "\"" + to_string(r.timestamp == 0 ? timestamp : r.timestamp) + "\", " \
          + to_string(r.game_id) + ", " \
          + "\"" + r.machine + "\", " \
          + to_string(r.seq) + ", " \
          + to_string(r.pri) + ", " \
          + to_string(r.reward) + ", " \
          + "\"" + r.content + "\");";
       return exec(sql) == 0;
   }

   bool table_read_recent(int max_num_records) {
       auto read_callback = [](void *handle, int num_columns, char **column_texts, char **column_names) {
           (void)num_columns;
           (void)column_names;

           SharedRWBuffer *h = reinterpret_cast<SharedRWBuffer *>(handle);
           SharedRWBuffer::Record r;

           r.timestamp = stoi(column_texts[0]);
           r.game_id = stoi(column_texts[1]);
           r.machine = column_texts[2];
           r.seq = stoi(column_texts[3]);
           r.pri = stof(column_texts[4]);
           r.reward = stof(column_texts[5]);
           r.content = column_texts[6];

           h->cb_save(r);
           return 0;
       };

       // Read things into a buffer.
       const string sql = "SELECT * FROM " + table_name_ + " ORDER BY TIME DESC LIMIT " + to_string(max_num_records) + ";";
       cb_save_start();
       int ret = exec(sql, read_callback);
       cb_save_end();
       return ret == 0;
   }

   const vector<Record> &get_recent_load() const { return recent_loaded_[curr_idx_]; }

   void cb_save_start() {
       alt_mutex_.lock();
       int alt_idx = (curr_idx_ + 1) % recent_loaded_.size();
       recent_loaded_[alt_idx].clear();
   }

   bool cb_save(const Record &r) {
       int alt_idx = (curr_idx_ + 1) % recent_loaded_.size();
       // Assuming the locker is ready.
       recent_loaded_[alt_idx].push_back(r);
       return true;
   }

   void cb_save_end() {
       int alt_idx = (curr_idx_ + 1) % recent_loaded_.size();

       rw_lock_.write_lock();
       curr_idx_ = alt_idx;
       rw_lock_.write_unlock();

       alt_mutex_.unlock();
   }
};

} // namespace elf