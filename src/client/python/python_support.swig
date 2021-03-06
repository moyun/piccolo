%module piccolo

#include "client/python/python_support.h"

using namespace std;

struct PyObject;

namespace google {
  namespace protobuf {
    typedef int int32;
    typedef long int64;
  }
}

%typemap(in) long& { $1 = PyInt_AsLong($input); }
%typemap(out) long& { $result = PyInt_FromLong(*$1); }

%typemap(in) long { $1 = PyInt_AsLong($input); }
%typemap(out) long { $result = PyInt_FromLong($1); }

%typemap(in) PyObject* { $1 = $input; Py_IncRef($1); }
%typemap(out) PyObject* { $result = $1; }

%typemap(in) PyObject*& { $1 = $input; Py_IncRef($1); }
%typemap(out) PyObject*& { $result = *$1; }

%typemap(in) const PyObject*& { $1 = $input; Py_IncRef($1); }
%typemap(out) const PyObject*& { $result = *$1; }

namespace std {
  %typemap(in) string& (string temp) {
    if (PyString_Check($input)) {
      temp = PyString_AsString($input);
      $1 = &temp;
     } else{
       PyErr_SetString(PyExc_TypeError,"Expected a string type!");
       return NULL; 
     }
  }
  
  %typemap(in) string (string temp) {
    if (PyString_Check($input)) {
      $1 = PyString_AsString($input);
     } else{
       PyErr_SetString(PyExc_TypeError,"Expected a string type!");
       return NULL; 
     }
  }

  %typemap(out) string { 
    $result = PyString_FromString(($1).c_str());
  }
  
  %typemap(out) string& { 
    $result = PyString_FromString((*$1).c_str());
  }
}

%{
#include "client/python/python_support.h"
#include "kernel/table.h"

namespace dsm {
typedef TypedGlobalTable<string, int64_t> IntTable;
typedef TypedGlobalTable<string, string> StringTable;

typedef TypedTableIterator<string, int64_t> IntIterator;
typedef TypedTableIterator<string, string> StringIterator;

}%}

#define GOOGLE_PROTOBUF_VERSION 2003000
#define LIBPROTOBUF_EXPORT

typedef long int64_t;

%include "util/rpc.h"

%include "kernel/kernel.h"
%include "kernel/table-registry.h"

%include "master/master.h"
%include "client/python/python_support.h"

%inline %{
  namespace dsm {
  struct IntAccum { 
    static Accumulator<int64_t>* Min() { return new dsm::Accumulators<int64_t>::Min; }
    static Accumulator<int64_t>* Max() { return new dsm::Accumulators<int64_t>::Max; }
    static Accumulator<int64_t>* Sum() { return new dsm::Accumulators<int64_t>::Sum; }
    static Accumulator<int64_t>* Replace() { return new dsm::Accumulators<int64_t>::Replace; }
  };
  
  struct StringAccum { 
    static Accumulator<string>* Min() { return new dsm::Accumulators<string>::Min; }
    static Accumulator<string>* Max() { return new dsm::Accumulators<string>::Max; }
    static Accumulator<string>* Sum() { return new dsm::Accumulators<string>::Sum; }
    static Accumulator<string>* Replace() { return new dsm::Accumulators<string>::Replace; }
  };
  
  static Sharding::String* StringSharding() { return new Sharding::String; }
  static Sharding::Mod* ModSharding() { return new Sharding::Mod; }
  static Sharding::UintMod* UintModSharding() { return new Sharding::UintMod; }
  
  struct SwigDomainSharding : public Sharder<string> {
    int operator()(const string& in, int num_shards) {
      int d_end = in.find(" ");
      return SuperFastHash(in.data(), d_end) % num_shards;
    }
  };
  
  static Sharder<string>* DomainSharding() { return new SwigDomainSharding; }
}
%};

namespace dsm {
class ConfigData {
public:
  ConfigData();
  virtual ~ConfigData();
  ConfigData(const ConfigData& from);
  ConfigData& operator=(const ConfigData& from);
  
  int num_workers() const;
  void set_num_workers(int value);

  int worker_id() const;
  void set_worker_id(int value);

  int master_id() const;
  void set_master_id(int value);
};

class RecordFile {
public:
  enum CompressionType {
    NONE = 0,
    LZO = 1
  };

  RecordFile(const string& path, const string& mode, int compression=NONE);
  ~RecordFile();

  void write(const google::protobuf::Message &m);
  bool read(google::protobuf::Message *m);
  const char* name();

  bool eof();
  void sync();
};

bool StartWorker(const ConfigData& conf);

template <class K, class V>
struct TypedTableIterator {
private:
  TypedIterator();
public:
  void key_str(string *out);
  void value_str(string *out);
  bool done();
  void Next();

  const K& key();
  V& value();
}; 

template <class K, class V> 
struct TypedGlobalTable<K, V>  {
private:
  TypedGlobalTable();
public:
  
  V get(const K& k);
  bool contains(const K& k);
  void update (const K& k, const V&);
  void put(const K& k, const V&);
  
  TypedTableIterator<K, V>* get_iterator(int shard);
  
  void SendUpdates();
  void HandlePutRequests();
};

%template(IntTable) TypedGlobalTable<string, int64_t>;
%template(StringTable) TypedGlobalTable<string, string>;

%template(IntIterator) TypedTableIterator<string, int64_t>;
%template(StringIterator) TypedTableIterator<string, string>;

%extend DSMKernel {
  TypedGlobalTable<string, string>* GetStringTable(int idx) { return $self->get_table<string, string>(idx); }
  TypedGlobalTable<string, int64_t>* GetIntTable(int idx) { return $self->get_table<string, int64_t>(idx); }
}

%template(CreateStringTable) CreateTable<string, string>;
%template(CreateIntTable) CreateTable<string, int64_t>;

%extend Master {
  void py_run_all(const string& code, void *locality) {
    RunDescriptor r("PythonKernel", "run_python_code", (GlobalTable*)locality);
    r.params.put<string>("python_code", code);
    r.shards = range(((GlobalTable*)locality)->num_shards());
    $self->run(r);
  }

  // Run the given kernel function on one (arbitrary) worker node.
  void py_run_one(const string& code, void*locality) {
    RunDescriptor r("PythonKernel", "run_python_code", (GlobalTable*)locality);
    r.params.put<string>("python_code", code);
    r.shards = range(1);
    $self->run(r);
  }
}
}