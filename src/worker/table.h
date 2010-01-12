#ifndef ACCUMULATOR_H
#define ACCUMULATOR_H

#include "util/common.h"

#include "worker/hash-msgs.h"
#include "worker/worker.pb.h"

#include <algorithm>

namespace upc {

static int StringSharding(const string& k, int shards) { return StringPiece(k).hash() % shards; }
static int ModSharding(const int& key, int shards) { return key % shards; }

template <class V>
struct Accumulator {
  static V min(const V& a, const V& b) { return std::min(a, b); }
  static V max(const V& a, const V& b) { return std::max(a, b); }
  static V sum(const V& a, const V& b) { return a + b; }
  static V replace(const V& a, const V& b) { return b; }
};

struct Data {
  // strings
  static void marshal(const string& t, string *out) { *out = t; }
  static void unmarshal(const StringPiece& s, string *t) { t->assign(s.data, s.len); }

  // protocol messages
  static void marshal(const google::protobuf::Message& t, string *out) { t.SerializePartialToString(out); }
  static void unmarshal(const StringPiece& s, google::protobuf::Message* t) { t->ParseFromArray(s.data, s.len); }

  template <class T>
  static void marshal(const T& t, string* out) {
    out->assign(reinterpret_cast<const char*>(&t), sizeof(t));
  }

  template <class T>
  static void unmarshal(const StringPiece& s, T *t) {
    *t = *reinterpret_cast<const T*>(s.data);
  }


  template <class T>
  static string to_string(const T& t) {
    string t_marshal;
    marshal(t, &t_marshal);
    return t_marshal;
  }

  template <class T>
  static T from_string(const StringPiece& t) {
    T t_marshal;
    unmarshal(t, &t_marshal);
    return t_marshal;
  }
};

struct TableInfo {
public:
  int table_id;
  int num_shards;

  // For local tables, the shard of the global table they represent.
  int shard;

  // We use void* to pass around the various accumulation and sharding
  // functions; they are cast to the appropriate type at the time of use.
  void *accum_function;
  void *sharding_function;

  // Used when fetching remote keys.
  RPCHelper *rpc;
};

class Table {
public:
  struct Iterator {
    virtual void key_str(string *out) = 0;
    virtual void value_str(string *out) = 0;
    virtual bool done() = 0;
    virtual void Next() = 0;

    virtual Table *owner() = 0;
  };

  Table(TableInfo tinfo) : info_(tinfo) {}
  virtual ~Table() {}

  // Generic routines to fetch and set entries as serialized strings.
  virtual string get_str(const StringPiece &k) = 0;
  virtual void put_str(const StringPiece &k, const StringPiece& v) = 0;

  // Clear the local portion of a shared table.
  virtual void clear() = 0;

  // Returns a view on the global table containing only local values.
  virtual Iterator* get_iterator() = 0;

  const TableInfo& info() {
    return info_;
  }

  void set_info(const TableInfo& t) {
    info_ = t;
  }

  int id() { return info_.table_id; }
  int shard() { return info_.shard; }

  TableInfo info_;

  // The following functions are only available on partitioned tables:
  virtual bool is_local_shard(int shard) { LOG(FATAL) << "Not implemented."; }
  virtual bool is_local_key(const StringPiece &k) { LOG(FATAL) << "Not implemented."; }

  virtual void local_shards(vector<int>* v) { LOG(FATAL) << "Not implemented."; }

  // Check only the local table for 'k'.  Abort if lookup would case a remote fetch.
  virtual string get_local(const StringPiece &k) { LOG(FATAL) << "Not implemented."; }

  // Append to 'out' the list of accumulators that have pending network data.  Return
  // true if any updates were appended.
  virtual bool GetPendingUpdates(deque<Table*> *out) { LOG(FATAL) << "Not implemented."; }

  // Give this table back to the partitioned table for use.
  virtual void Free(Table* t) { LOG(FATAL) << "Not implemented."; }

  virtual void ApplyUpdates(const upc::HashUpdate& req) { LOG(FATAL) << "Not implemented."; }
  virtual int pending_write_bytes() { LOG(FATAL) << "Not implemented."; }

};

template <class K, class V>
class TypedTable : public Table {
public:
  struct Iterator : public Table::Iterator {
    virtual const K& key() = 0;
    virtual V& value() = 0;
  };

  TypedTable(const TableInfo& tinfo) : Table(tinfo) {}

  // Functions for locating and accumulating data.
  typedef V (*AccumFunction)(const V& a, const V& b);
  typedef int (*ShardingFunction)(const K& k, int num_shards);

  virtual V get(const K& k) = 0;
  virtual void put(const K& k, const V &v) = 0;
  virtual void remove(const K& k) { }

  virtual Iterator* get_typed_iterator() = 0;

  string get_str(const StringPiece &k) {
    return Data::to_string<V>(get(Data::from_string<K>(k)));
  }

  void put_str(const StringPiece &k, const StringPiece &v) {
    const K& kt = Data::from_string<K>(k);
    const V& vt = Data::from_string<V>(v);
    put(kt, vt);
  }

  void remove_str(const StringPiece &k) {
    remove(Data::from_string<K>(k));
  }

  int get_shard(StringPiece k) {
    return get_shard(Data::from_string<K>(k));
  }

  int get_shard(const K& k) {
    return ((typename TypedTable<K, V>::ShardingFunction)info_.sharding_function)(k, info_.num_shards);
  }

  V accumulate(const V& a, const V& b) {
    return ((typename TypedTable<K, V>::AccumFunction)info_.accum_function)(a, b);
  }
};

}
#endif
