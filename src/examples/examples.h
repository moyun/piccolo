#include "client.h"
#include "examples/examples.pb.h"

using namespace dsm;

namespace dsm { namespace data {
  template <>
  void marshal(const Bucket& t, string *out) {
    t.SerializePartialToString(out);
  }

  template <>
  void unmarshal(const StringPiece& s, Bucket* t) {
    t->ParseFromArray(s.data, s.len);
  }
} }

static void BucketMerge(Bucket *l, const Bucket &r) {
  l->MergeFrom(r);
}
