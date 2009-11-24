#include <upc.h>
#include "test/file-helper.h"

/* caveats
1. all web pages are numbered from 1..N
   there is no hash table implementation of pr on shared memory.
2. the entire pagerank array must be ble to fit in the memory of a single node
*/

#define N 100
#define BLK 1
#define NBRS 10 //completely bogus, fixed number of neighbors per node
#define ITERN 10
#define TOTALRANK 10.0

#define PROP 0.8

#define EIDX(p) ((p/(BLK*THREADS))*BLK + (p % BLK))

//shared [BLK*NBRS] int m[N/BLK][BLK][NBRS];

shared [N] double tmp_pr[THREADS][N/BLK][BLK];
shared [BLK] double pr[N/BLK][BLK];

GraphEntry entries[N/THREADS];

void
load_graph() {

	char srcfile[1000];
  int current_entry = 0;
  struct RFile *r;
  GraphEntry *e;

	sprintf(srcfile, "testdata/nodes.rec-%05d-of-%05d", MYTHREAD, THREADS);

  r = RecordFile_Open(srcfile, "r");
  while ((e = RecordFile_ReadGraphEntry(r))) {
    entries[current_entry++] = *e;
  }

	assert(current_entry == N/THREADS);
  RecordFile_Close(r);
}

int 
main(int argc, char **argv) {
	int i, j, k, iter;
	double *local_pr;
	double (*local_tmp_pr)[BLK];
	double buf[BLK];
  GraphEntry *e;

	load_graph();

	srand(0);

	upc_forall(i = 0; i < N/BLK; i++; &pr[i][0]) {
		local_pr = (double (*)) pr[i];
		for (j = 0; j < BLK; j++) {
			local_pr[j] = TOTALRANK/N;
		}
	}

	upc_barrier;

	if (MYTHREAD == 0) {
		printf("finish initialization ..pr[0]=%.2f\n", pr[0][0]);
	}

	//hopefully, this is legal
	local_tmp_pr = (double (*)[BLK]) tmp_pr[MYTHREAD];

	for (iter = 0; iter <  ITERN; iter++) {
		bzero(local_tmp_pr, sizeof(double)*N);

		upc_forall(i = 0; i < N/BLK; i++; &pr[i][0]) {
			local_pr = (double (*))pr[i]; 
			e = &entries[EIDX(i*BLK)];
			for (j = 0; j < BLK; j++) {
				assert(e->id == i * BLK + j);
				for (k = 0; k < e->num_neighbors; k++) {
					local_tmp_pr[e->neighbors[k]/BLK][e->neighbors[k]%BLK] += PROP*(local_pr[j]/e->num_neighbors); //this should be all local
				}
				e++;
			}
		}

		upc_barrier;

		upc_forall(i = 0; i < N/BLK; i++; &pr[i][0]) {
			local_pr = (double (*))pr[i];
			bzero(local_pr, sizeof(double)*BLK);
			for (j = 0; j < THREADS; j++) {
				upc_memget(buf, &tmp_pr[j][i], BLK*sizeof(double));
				for (k = 0; k < BLK; k++) {
					local_pr[k] += buf[k];
				}
			}
			for (k = 0; k < BLK; k++) {
				local_pr[k] += (1-PROP)*(TOTALRANK/N);
			}
		}

		upc_barrier;

		printf("finish %d-th iteration pr[0][0] %.3f\n", iter, pr[0][0]);
/*
		upc_forall(i = 0; i < N; i++; &pr[i][0]) {
			for (k = 0; k < BLK; k++) 
				printf("finish %d-th iteration in %.2f seconds pr[%d][%d] %.3f\n", iter, second()-sec, i, k, pr[i][k]);
		}
	*/
	}

}