#ifndef ACCUMULATOR_H
#define ACCUMULATOR_H

#include "util/common.h"
#include "util/rpc.h"

#include "worker/worker.pb.h"
#include <algorithm>

namespace upc {

static int StringSharding(const string& k, int shards) { return StringPiece(k).hash() % shards; }

template <class K>
static int ModSharding(const K& k, int shards) { return k % shards; }

template <class V>
struct Accumulator {
  static V min(const V& a, const V& b) { return std::min(a, b); }
  static V max(const V& a, const V& b) { return std::max(a, b); }
  static V sum(const V& a, const V& b) { return a + b; }
};

template <class T>
struct Marshal {
  static string to_string(const T& t) {
    return string(reinterpret_cast<const char*>(&t), sizeof(t));
  }
  static T from_string(const StringPiece& s) {
    T t = *reinterpret_cast<const T*>(s.data);
    return t;
  }
};

template <>
struct Marshal<string> {
  static string to_string(const string& t) { return t; }
  static string from_string(const StringPiece& s) { return s.AsString(); }
};

struct TableInfo {
public:
  // The thread with ownership over this data.
  int owner_thread;

  // The table to which this partition belongs.
  int table_id;

  // We use void* to pass around the various accumulation and sharding functions; they
  // are cast to the appropriate type at the time of use.
  void *af;
  void *sf;

  // Used for partitioned tables
  RPCHelper *rpc;
  int num_threads;
};


class Table;

struct TableIterator {
  virtual string key_str() = 0;
  virtual string value_str() = 0;
  virtual bool done() = 0;
  virtual void Next() = 0;

  virtual Table *owner() = 0;
};

template <class K, class V>
struct TypedTableIterator : public TableIterator {
  virtual const K& key() = 0;
  virtual const V& value() = 0;
};

class Table {
public:
  Table(TableInfo tinfo) : info_(tinfo) {}
  virtual ~Table() {}

  virtual string get_str(const StringPiece &k) = 0;
  virtual void put_str(const StringPiece &k, const StringPiece& v) = 0;
  virtual int64_t size() = 0;

  // Returns a view on the global table containing only local values.
  virtual TableIterator* get_iterator() = 0;
  const TableInfo& info() { return info_; }

  TableInfo info_;
};

template <class K, class V>
class TypedTable : public Table {
public:
  TypedTable(const TableInfo& tinfo) : Table(tinfo) {}

  // Functions for locating and accumulating data.
  typedef V (*AccumFunction)(const V& a, const V& b);
  typedef int (*ShardingFunction)(const K& k, int num_shards);

  virtual V get(const K& k) = 0;
  virtual void put(const K& k, const V &v) = 0;
  virtual void remove(const K& k) { }

  virtual TypedTableIterator<K, V>* get_typed_iterator() = 0;

  string get_str(const StringPiece &k) {
    return Marshal<V>::to_string(get(Marshal<K>::from_string(k)));
  }

  void put_str(const StringPiece &k, const StringPiece &v) {
    const V& vt = Marshal<V>::from_string(v);
    const K& kt = Marshal<K>::from_string(k);
    put(kt, vt);
  }

  void remove_str(const StringPiece &k) {
    remove(Marshal<K>::from_string(k));
  }

  int get_shard(const K& k) {
    return ((typename TypedTable<K, V>::ShardingFunction)info_.sf)(k, info_.num_threads);
  }

  V accumulate(const V& a, const V& b) {
    return ((typename TypedTable<K, V>::AccumFunction)info_.af)(a, b);
  }
};

}
#endif
