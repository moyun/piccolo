#include "client/client.h"
#include "examples/examples.pb.h"

#include <sys/time.h>
#include <sys/resource.h>
#include <algorithm>
#include <libgen.h>

using namespace dsm;
using namespace std;

static int NUM_WORKERS = 2;
#define MAXCOST RAND_MAX

DEFINE_int32(tleft_vertices, 100, "Number of left-side vertices");
DEFINE_int32(tright_vertices, 100, "Number of right-side vertices");
DEFINE_double(tedge_probability, 0.5, "Probability of edge between vertices");
DEFINE_bool(tedge_costs, false, "Set to true to have edges have costs");

static TypedGlobalTable<int, vector<int> >*   leftoutedges = NULL;
static TypedGlobalTable<int, vector<int> >*   leftoutcosts = NULL;
static TypedGlobalTable<int, int>*             leftmatches = NULL;
static TypedGlobalTable<int, int>*            rightmatches = NULL;
static TypedGlobalTable<int, int>*		        rightcosts = NULL;
static TypedGlobalTable<string, string>*        StatsTable = NULL;

//-----------------------------------------------
namespace dsm{
	template <> struct Marshal<vector<int> > : MarshalBase {
		static void marshal(const vector<int>& t, string *out) {
			int i,j;
			int len = t.size();
			out->append((char*)&len,sizeof(int));
			for(i = 0; i < len; i++) {
				j = t[i];
				out->append((char*)&j,sizeof(int));
			}
		}
		static void unmarshal(const StringPiece &s, vector<int>* t) {
			int i,j;
			int len;
			memcpy(&len,s.data,sizeof(int));
			t->clear();
			for(i = 0; i < len; i++) {
				memcpy(&j,s.data+(i+1)*sizeof(int),sizeof(int));
				t->push_back(j);
			}
		}
	};
}
			

//-----------------------------------------------

class BPMTKernel : public DSMKernel {
	public:
		void InitTables() {
			vector<int> v;		//for left nodes' neighbors
			vector<int> v2;	//for left nodes' edge costs

			v.clear();
			v2.clear();

			leftmatches->resize(FLAGS_tleft_vertices);
			rightmatches->resize(FLAGS_tright_vertices);
			leftoutedges->resize(FLAGS_tleft_vertices);
			leftoutcosts->resize(FLAGS_tleft_vertices);
			for(int i=0; i<FLAGS_tleft_vertices; i++) {
				leftmatches->update(i,-1);
				leftoutedges->update(i,v);
				leftoutcosts->update(i,v2);
			}
			for(int i=0; i<FLAGS_tright_vertices; i++) {
				rightmatches->update(i,-1);
				rightcosts->update(i,MAXCOST);
			}
			StatsTable->update("quiescent","t");
			StatsTable->SendUpdates();
		}

		void PopulateLeft() {
			TypedTableIterator<int, vector<int> > *it = 
				leftoutedges->get_typed_iterator(current_shard());
            CHECK(it != NULL);
			TypedTableIterator<int, vector<int> > *it2 = 
				leftoutcosts->get_typed_iterator(current_shard());
            CHECK(it2 != NULL);
			int cost = 0;
			for(; !it->done() && !it2->done(); it->Next(),it2->Next()) {
				vector<int> v  =  it->value();
				vector<int> v2 = it2->value();
				for(int i=0; i<FLAGS_tright_vertices; i++) {
					if ((float)rand()/(float)RAND_MAX < 
							FLAGS_tedge_probability) {
						v.push_back(i);					//add neighbor
						cost = ((FLAGS_tedge_costs)?rand():(RAND_MAX));
						v2.push_back(cost);
					}
				}
				leftoutedges->update(it->key(),v);		//store list of neighboring edges
				leftoutcosts->update(it2->key(),v2);	//store list of neighbor edge costs
			}
		}

		//Set a random right neighbor of each left vertex to be
		//matched.  If multiple lefts set the same right, the triggers
		//will sort it out.
		void LeftBPMT() {
			TypedTableIterator<int, vector<int> > *it = 
				leftoutedges->get_typed_iterator(current_shard());
			TypedTableIterator<int, vector<int> > *it2 = 
				leftoutcosts->get_typed_iterator(current_shard());
			TypedTableIterator<int, int> *it3 =
				leftmatches->get_typed_iterator(current_shard());
			for(; !it->done() && !it2->done() && !it3->done(); it->Next(),it2->Next(),it3->Next()) {
				vector<int>  v =  it->value();
				vector<int> v2 = it2->value();
				if (v.size() <= 0 || it3->value() != -1)
					continue;

				//try to find a random or best match
				int j;
				if (FLAGS_tedge_costs) {
					//edges have associated costs
					vector<int>::iterator  inner_it =  v.begin();
					vector<int>::iterator inner_it2 = v2.begin();
					j = -1;
					float mincost = MAXCOST;
					for(; inner_it != v.end() && inner_it2 != v2.end(); inner_it++, inner_it2++) {
						if ((*inner_it2) < mincost) {
							mincost = *inner_it2;
							j = *inner_it;
						}
					}
				} else {
					//all edges equal; pick one at random
					j = v.size()*((float)rand()/(float)RAND_MAX);
					j = (j>=v.size())?v.size()-1:j;
					j = v[j];
				}
				VLOG(2) << "Attempted match: left " << it->key() << " <--> right " << j << endl;
				rightmatches->update(j,it->key());
				leftmatches->update(it->key(),j);
			}
		}

		void RightBPMT() {
			int rightset[FLAGS_tright_vertices];
			bool quiescent = true;

			for(int i=0; i<FLAGS_tright_vertices; i++)
				rightset[i] = 0;

			//Check if parallelization set multiple lefts matching the same right
			int i=0;
			for(int j=0; j<leftoutedges->num_shards(); j++) {
				TypedTableIterator<int, int> *it =
					 leftmatches->get_typed_iterator(j);
				for(; !it->done(); it->Next()) {
					int rightmatch = it->value();
					if (-1 < rightmatch) {
						rightset[rightmatch]++;
						if (rightset[rightmatch] > 1) {
							VLOG(2) << "Denying match on " << rightmatch << "from " << it->key() << endl;
							leftmatches->update(it->key(),-1);

							//state
							quiescent = false;
							i++;
						}
					}
				}
			}
			VLOG(0) << "Total of " << i << " left nodes were overlapped and fixed." << endl;
			StatsTable->update("quiescent",(quiescent?"t":"f"));
		}

		void EvalPerformance() {
			int left_matched=0, right_matched=0;
			int rightset[FLAGS_tright_vertices];

			//float edgecost = 0.f;
			//float worstedgecost = 0.f;

			for(int i=0; i<FLAGS_tright_vertices; i++) {
				rightset[i] = 0;
				right_matched += (-1 < rightmatches->get(i));

				//TODO calculate how the costs worked out
			}

			for(int i=0; i<FLAGS_tleft_vertices; i++) {
				int rightmatch = leftmatches->get(i);
				if (-1 < rightmatch) {
					left_matched++;
					rightset[rightmatch]++;
					if (rightset[rightmatch] > 1)
						cout << rightset[rightmatch] << " left vertices have right vertex " <<
							rightmatch << " as a match: one is " << i << endl;
				}
			}
			printf("Performance: [LEFT]  %d of %d matched.\n",left_matched,FLAGS_tleft_vertices);
			printf("Performance: [RIGHT] %d of %d matched.\n",right_matched,FLAGS_tright_vertices);
		}
};

class MatchRequestTrigger : public Trigger<int, int> {
	public:
		bool Fire(const int& key, const int& value, int& newvalue ) {
			int newcost = MAXCOST;
			if (newvalue != -1) {
				vector<int> v  = leftoutedges->get(newvalue);	//get the vector for this left key
				vector<int> v2 = leftoutcosts->get(newvalue);
				vector<int>::iterator it = find(v.begin(), v.end(), key);
				vector<int>::iterator it2;
				
				//Grab cost from left node
				if (it != v.end()) {
					it2 = v2.begin() + (it - v.begin());
					newcost = *it2;
				}
			}
			if (value != -1) {

				//cost check
				if (newcost < rightcosts->get(key)) {
					//found better match!
					leftmatches->enqueue_update(value,-1);	//remove old match
					rightcosts->enqueue_update(key,newcost);
					return true;
				}

				VLOG(2) << "Denying match on " << key << " from " << newvalue << endl;
				leftmatches->enqueue_update(newvalue,-1);
				return false;
			} else {
				//Else this match is acceptable.  Set new cost.
				VLOG(2) << "Accepting match on " << key << " from " << newvalue << endl;
				rightcosts->enqueue_update(key,newcost);
			}
			return true;
		}
};

class MatchDenyTrigger : public Trigger<int, int> {
	public:
		bool Fire(const int& key, const int& value, int& newvalue ) {

			//Don't store the denial!
			if (newvalue == -1) {

				VLOG(2) << "Match from " << key << " denied from " << value << endl;

				//Denied: remove possible right match
				vector<int> v  = leftoutedges->get(key);
				vector<int> v2 = leftoutcosts->get(key);

				vector<int>::iterator it = find(v.begin(), v.end(), value);
				vector<int>::iterator it2;

				if (it != v.end()) {		//remove possible match
					it2 = v2.begin() + (it-v.begin()); //index into cost list
					v.erase(it);
					v2.erase(it2);
				}

				//Enqueue the removal
				leftoutedges->enqueue_update((int)key,v);
				leftoutcosts->enqueue_update((int)key,v2);

				if (v.size() == 0) {		//forget it if no more candidates
					VLOG(2) << "Ran out of right candidates for " << key << endl;
					return true;
				}

				//Pick a new right match
				int j;
				if (FLAGS_tedge_costs) {
					//edges have associated costs
					vector<int>::iterator   inner_it  = v.begin();
					vector<int>::iterator inner_it2 = v2.begin();
					j = -1;
					float mincost = MAXCOST;
					for(; inner_it != v.end() && inner_it2 != v2.end();
							inner_it++, inner_it2++)
					{
						if ((*inner_it2) < mincost) {
							mincost = *inner_it2;
							j = *inner_it;
						}
					}
				} else {
					//all edges equal; pick one at random
					j = v.size()*((float)rand()/(float)RAND_MAX);
					j = (j>=v.size())?v.size()-1:j;
					j = v[j];
				}
				rightmatches->enqueue_update(j,key);
				newvalue = j;
				VLOG(2) << "Re-attempting from " << key << " to " << j << endl;
				return true;
			}
			return true;
		}
};



//-----------------------------------------------

REGISTER_KERNEL(BPMTKernel);
REGISTER_METHOD(BPMTKernel, InitTables);
REGISTER_METHOD(BPMTKernel, PopulateLeft);
REGISTER_METHOD(BPMTKernel, LeftBPMT);
REGISTER_METHOD(BPMTKernel, RightBPMT);
REGISTER_METHOD(BPMTKernel, EvalPerformance);

int Bipartmatch_trigger(ConfigData& conf) {

	leftoutedges  = CreateTable(0,conf.num_workers(),new Sharding::Mod, 
		new Accumulators<vector<int> >::Replace);
	leftmatches   = CreateTable(1,conf.num_workers(),new Sharding::Mod,
		new Accumulators<int>::Replace);
	rightmatches  = CreateTable(2,conf.num_workers(),new Sharding::Mod,
		new Accumulators<int>::Replace);
	leftoutcosts  = CreateTable(3,conf.num_workers(),new Sharding::Mod,
		new Accumulators<vector<int> >::Replace);
	rightcosts    = CreateTable(4,conf.num_workers(),new Sharding::Mod,
		new Accumulators<int>::Replace);
	StatsTable    = CreateTable(10000,1,new Sharding::String, new Accumulators<string>::Replace);//CreateStatsTable();

	TriggerID matchreqid = rightmatches->register_trigger(new MatchRequestTrigger);
	TriggerID matchdenyid = leftmatches->register_trigger(new MatchDenyTrigger);

	StartWorker(conf);
	Master m(conf);

	NUM_WORKERS = conf.num_workers();
	printf("---- Initializing Bipartmatch-trigger on %d workers ----\n",NUM_WORKERS);

	//Disable triggers
	m.enable_trigger(matchreqid,2,false);
	m.enable_trigger(matchdenyid,1,false);

	//Fill in all necessary keys
	m.run_one("BPMTKernel","InitTables",  leftoutedges);
	//Populate edges left<->right
	m.run_all("BPMTKernel","PopulateLeft",  leftoutedges);
	m.barrier();

	//Enable triggers
	m.enable_trigger(matchreqid,2,true);
	m.enable_trigger(matchdenyid,1,true);

	bool unstable;
	do {
		unstable = false;
		m.run_all("BPMTKernel","LeftBPMT", leftoutedges);
		m.run_one("BPMTKernel","RightBPMT", leftmatches);
	} while (0 == strcmp(StatsTable->get("quiescent").c_str(),"f"));

	//Disable triggers
	m.enable_trigger(matchreqid,2,false);
	m.enable_trigger(matchdenyid,1,false);

	m.run_one("BPMTKernel","EvalPerformance",leftmatches);

	return 0;
}
REGISTER_RUNNER(Bipartmatch_trigger);
