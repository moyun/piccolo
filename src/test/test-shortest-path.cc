#include "util/common.h"
#include "util/file.h"
#include "worker/worker.h"
#include "master/master.h"
#include "test/test.pb.h"

using namespace upc;
DEFINE_int32(num_nodes, 10000, "");
DEFINE_bool(dump_output, false, "");

static int NUM_WORKERS = 0;
static TypedTable<int, double>* distance;

void BuildGraph(int shards, int nodes, int density) {
  vector<RecordFile*> out(shards);
  Mkdirs("testdata/");
  for (int i = 0; i < shards; ++i) {
    out[i] = new RecordFile(StringPrintf("testdata/sp-graph.rec-%05d-of-%05d", i, shards), "w");
  }

  fprintf(stderr, "Building graph: ");

  for (int i = 0; i < nodes; i++) {
    PathNode n;
    n.set_id(i);

    for (int j = 0; j < density; j++) {
      n.add_target(random() % nodes);
    }

    out[i % shards]->write(n);
    if (i % (nodes / 50) == 0) { fprintf(stderr, "."); }
  }
  fprintf(stderr, "\n");

  for (int i = 0; i < shards; ++i) {
    delete out[i];
  }
}

struct ShortestPathKernel : public DSMKernel {
  vector<PathNode> local_nodes;
  void Initialize() {
    for (int i = 0; i < FLAGS_num_nodes; ++i) {
      distance->put(i, 1e9);
    }

    // Initialize a root node.
    distance->put(0, 0);
  }

  void Propagate() {
    if (local_nodes.empty()) {
      RecordFile r(StringPrintf("testdata/sp-graph.rec-%05d-of-%05d", shard(), NUM_WORKERS), "r");
      PathNode n;
      while (r.read(&n)) {
        local_nodes.push_back(n);
      }
    }

    for (int i = 0; i < local_nodes.size(); ++i) {
      const PathNode &n = local_nodes[i];
      for (int j = 0; j < n.target_size(); ++j) {
        distance->put(n.target(j), distance->get(n.id()) + 1);
      }
    }
  }

  void DumpDistances() {
    for (int i = 0; i < FLAGS_num_nodes; ++i) {
      if (i % 30 == 0) {
        fprintf(stderr, "\n%5d: ", i);
      }

      int d = (int)distance->get(i);
      if (d >= 1000) { d = -1; }
      fprintf(stderr, "%3d ", d);
    }
  }
};

REGISTER_KERNEL(ShortestPathKernel);
REGISTER_METHOD(ShortestPathKernel, Initialize);
REGISTER_METHOD(ShortestPathKernel, Propagate);
REGISTER_METHOD(ShortestPathKernel, DumpDistances);

int main(int argc, char **argv) {
  Init(argc, argv);

  ConfigData conf;
  conf.set_num_workers(MPI::COMM_WORLD.Get_size() - 1);
  NUM_WORKERS = conf.num_workers();

  if (MPI::COMM_WORLD.Get_rank() == 0) {
    BuildGraph(NUM_WORKERS, FLAGS_num_nodes, 4);

    Master m(conf);
    RUN_ONE(m, ShortestPathKernel, Initialize);
    for (int i = 0; i < 20; ++i) {
      RUN_ALL(m, ShortestPathKernel, Propagate);
    }

    if (FLAGS_dump_output) {
      RUN_ONE(m, ShortestPathKernel, DumpDistances);
    }
  } else {
    conf.set_worker_id(MPI::COMM_WORLD.Get_rank() - 1);
    Worker w(conf);

    TableInfo info;
    info.num_shards = 10;
    info.accum_function = (void*)&Accumulator<double>::min;
    info.sharding_function = (void*)&ModSharding;
    distance = w.create_table<int, double>(0, info);
    w.Run();

    LOG(INFO) << "Worker " << conf.worker_id() << " :: " << w.get_stats();
  }
}
