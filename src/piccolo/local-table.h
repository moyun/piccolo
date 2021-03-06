#ifndef LOCALTABLE_H_
#define LOCALTABLE_H_

#include "piccolo/table.h"
#include "piccolo/file.h"
#include "piccolo/rpc.h"

namespace dsm {

// Represents a single shard of a partitioned global table.
class LocalTable :
  public TableBase,
  public Checkpointable,
  public Serializable,
  public UntypedTable {
public:
  LocalTable() : delta_file_(NULL) {}
  bool empty() { return size() == 0; }

  void start_checkpoint(const string& f);
  void finish_checkpoint();
  void restore(const string& f);
  void write_delta(const TableData& put);

  virtual int64_t size() = 0;
  virtual void clear() = 0;
  virtual void resize(int64_t size) = 0;

  virtual TableIterator* get_iterator() = 0;
protected:
  friend class GlobalTable;
  TableCoder *delta_file_;
};

}

#endif /* LOCALTABLE_H_ */
