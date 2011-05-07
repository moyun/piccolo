#include "examples/examples.h"
#include "kernel/disk-table.h"

using namespace dsm;
DEFINE_int32(tnum_nodes_record, 10000, "");
DEFINE_bool(tdump_output, false, "");

static int NUM_WORKERS = 0;
static TypedGlobalTable<int, double>* distance_map;
static RecordTable<PathNode>* nodes_record;
static TypedGlobalTable<int, vector<double> >* nodes;

namespace dsm{
	template <> struct Marshal<vector<double> > : MarshalBase {
		static void marshal(const vector<double>& t, string *out) {
			int i;
			double j;
			int len = t.size();
			out->append((char*)&len,sizeof(int));
			for(i = 0; i < len; i++) {
				j = t[i];
				out->append((char*)&j,sizeof(double));
			}
		}
		static void unmarshal(const StringPiece &s, vector<double>* t) {
			int i;
			double j;
			int len;
			memcpy(&len,s.data,sizeof(int));
			if (len < 0)
				LOG(FATAL) << "Unmarshalled vector of size < 0" << endl;
			t->clear();
			for(i = 0; i < len; i++) {
				memcpy(&j,s.data+(i+1)*sizeof(double),sizeof(double));
				t->push_back(j);
			}
		}
	};
}
// This is the trigger. In order to experiment with non-trigger version,
// I limited the maximum distance will be 20.

class SSSPTrigger : public Trigger<int, double> {
	public:
		bool Fire(const int& key, const double& value, double& newvalue) {
			cout << "TRIGGER: k=" << key <<", v="<< value << ",newvalue=" <<newvalue<<endl;
			if (value <= newvalue || newvalue >= 20)
				return false;
			vector<double> thisnode = nodes->get(key);
			vector<double>::iterator it = thisnode.begin();
			for(; it!=thisnode.end(); it++)
				distance_map->enqueue_update((*it), newvalue+1);
			return true;
		}
		bool LongFire(const int& key) {
			return false;
		}
};

static void BuildGraph(int shards, int nodes_record, int density) {
	vector<RecordFile*> out(shards);
	File::Mkdirs("testdata/");
	for (int i = 0; i < shards; ++i) {
		out[i] = new RecordFile(StringPrintf("testdata/sp-graph.rec-%05d-of-%05d", i, shards), "w");
	}

	fprintf(stderr, "Building graph: ");

	for (int i = 0; i < nodes_record; i++) {
		PathNode n;
		n.set_id(i);

		for (int j = 0; j < density; j++) {
			n.add_target(random() % nodes_record);
		}

		out[i % shards]->write(n);
		if (i % (nodes_record / 50) == 0) { fprintf(stderr, "."); }
	}
	fprintf(stderr, "\n");

	for (int i = 0; i < shards; ++i) {
		delete out[i];
	}
}

int ShortestPathTrigger(ConfigData& conf) {
	NUM_WORKERS = conf.num_workers();

	distance_map = CreateTable(0, FLAGS_shards, new Sharding::Mod, new Accumulators<double>::Min);
	nodes_record = CreateRecordTable<PathNode>(1, "testdata/sp-graph.rec*", false);
	nodes        = CreateTable(2, FLAGS_shards, new Sharding::Mod, new Accumulators<vector<double> >::Replace);
	TriggerID trigid = distance_map->register_trigger(new SSSPTrigger);

	StartWorker(conf);
	Master m(conf);

	if (FLAGS_build_graph) {
		BuildGraph(FLAGS_shards, FLAGS_tnum_nodes_record, 4);
		return 0;
	}

	m.enable_trigger(trigid, 0, false);

	PRunOne(distance_map, {
			for (int i = 0; i < FLAGS_tnum_nodes_record; ++i) {
			distance_map->update(i, 1e9);
			}

			});
	PRunOne(nodes, {
			vector<double> v;
			v.clear();

			nodes->resize(FLAGS_tnum_nodes_record);
			for(int i=0; i<FLAGS_tnum_nodes_record; i++)
				nodes->update(i,v);
	});

	PMap({n: nodes_record}, {
			vector<double> v=nodes->get(n.id());
			for(int i=0; i < n.target_size(); i++)
				v.push_back(n.target(i));
			cout << "writing node " << n.id() << endl;
			nodes->update(n.id(),v);
			});

	m.enable_trigger(trigid, 0, true);
	PRunOne(distance_map, {
			// Initialize a root node.
			// and enable the trigger.
			distance_map->update(0, 0);
			});
	m.enable_trigger(trigid, 0, false);

	PRunOne(distance_map, {
			for (int i = 0; i < FLAGS_tnum_nodes_record; ++i) {
			if (i % 30 == 0) {
			fprintf(stderr, "\n%5d: ", i);
			}

			int d = (int)distance_map->get(i);
			if (d >= 1000) { d = -1; }
			fprintf(stderr, "%3d ", d);
			}
			fprintf(stderr, "\n");
			});
	return 0;
}
REGISTER_RUNNER(ShortestPathTrigger);