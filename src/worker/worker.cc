#include <boost/bind.hpp>
#include <signal.h>
#ifdef CPUPROF
#include <google/profiler.h>
#endif

#include "util/common.h"
#include "worker/worker.h"
#include "worker/registry.h"

namespace upc {
static const int kMaxNetworkChunk = 1 << 20;

struct Worker::Peer {

  // An update request containing changes to apply to a remote table.
  struct Request {
    int target;
    int rpc_type;
    string payload;
    MPI::Request mpi_req;
    double startTime;

    Request() {
      startTime = Now();
    }

    ~Request() {}
  };

  HashUpdate writeScratch;
  list<Request*> outgoingRequests;

  // Incoming data from this peer that we have read from the network, but has yet to be
  // processed by the kernel.
  deque<HashUpdate*> incomingData;
  deque<HashRequest*> incomingRequests;

  mutable boost::recursive_mutex pending_lock_;

  int32_t id;
  Peer(int id) : id(id) {
  }

  void CollectPendingSends() {
    boost::recursive_mutex::scoped_lock sl(pending_lock_);
    for (list<Request*>::iterator i = outgoingRequests.begin(); i != outgoingRequests.end(); ++i) {
      Request *r = (*i);
      if (r->mpi_req.Test()) {
        VLOG(2) << "Request of size " << r->payload.size() << " finished.";
        delete r;
        i = outgoingRequests.erase(i);
      }
    }
  }

  void ReceiveIncomingData(RPCHelper *rpc) {
    while (rpc->HasData(id, MTYPE_KERNEL_DATA)) {
      HashUpdate *req = new HashUpdate;
      rpc->Read(id, MTYPE_KERNEL_DATA, req);
      incomingData.push_back(req);
    }

    while (rpc->HasData(id, MTYPE_GET_REQUEST)) {
      HashRequest *req = new HashRequest;
      rpc->Read(id, MTYPE_GET_REQUEST, req);
      VLOG(1) << "Read get request....";
      incomingRequests.push_back(req);
    }
  }

  bool write_pending() const {
    return !outgoingRequests.empty();
  }

  // Send the given message type and data to this peer.
  Request* Send(int rpc_type, RPCMessage* ureq, RPCHelper *rpc) {
    boost::recursive_mutex::scoped_lock sl(pending_lock_);

    Request* r = new Request();
    r->target = id;
    r->rpc_type = rpc_type;
    ureq->AppendToString(&r->payload);
    ureq->ParseFromString(r->payload);

    r->mpi_req = rpc->SendData(r->target, r->rpc_type, r->payload);
    outgoingRequests.push_back(r);

    return r;
  }

  HashUpdate *pop_data() {
    HashUpdate *r = incomingData.front();
    incomingData.pop_front();
    return r;
  }

  HashRequest *pop_request() {
    HashRequest *r = incomingRequests.front();
    incomingRequests.pop_front();
    return r;
  }
};

Worker::Worker(const ConfigData &c) {
  config.CopyFrom(c);

  num_peers_ = config.num_workers();
  peers.resize(num_peers_);
  for (int i = 0; i < num_peers_; ++i) {
    peers[i] = new Peer(i + 1);
  }

  running_ = true;

  world_ = MPI::COMM_WORLD;
  rpc_ = new RPCHelper(&world_);

  kernel_thread_ = network_thread_ = NULL;
  kernel_done_ = false;
}

void Worker::Run() {
  kernel_thread_ = new boost::thread(boost::bind(&Worker::KernelLoop, this));

  NetworkLoop();
  kernel_thread_->join();
}

Worker::~Worker() {
  running_ = false;
  delete kernel_thread_;
  delete network_thread_;

  for (int i = 0; i < pending_writes_.size(); ++i) {delete pending_writes_[i];}
  for (int i = 0; i < peers.size(); ++i) {
    delete peers[i];
  }
}


void Worker::NetworkLoop() {
  deque<Table*> work;
  while (running_) {
    PERIODIC(10, {
        VLOG(1) << "Network loop working - " << pending_kernel_bytes() << " bytes in the processing queue.";
    });

    Poll();

    if (work.empty()) {
      for (TableMap::iterator i = tables.begin(); i != tables.end(); ++i) {
        i->second->GetPendingUpdates(&work);
      }
    }

    if (work.empty() && pending_network_writes() == 0) {
      Sleep(0.01);
    }

    while (!work.empty()) {
      Table* t = work.front();
      work.pop_front();

      VLOG(2) << "Accum " << t->shard() << " : " << t->size();
      Peer * p = peers[peer_for_shard(t->id(), t->shard())];

      Table::Iterator *i = t->get_iterator();
      while (!i->done()) {
        ComputeUpdates(p, i);
        Poll();
      }
      delete i;

      tables[make_pair(t->id(), t->shard())]->Free(t);

      Poll();
    }
  }
}

void Worker::KernelLoop() {
  MPI::Intracomm world = MPI::COMM_WORLD;

	while (running_) {
	  if (kernel_requests_.empty()) {
	    Sleep(0.01);
	    continue;
	  }


	  RunKernelRequest k;
	  {
	    boost::recursive_mutex::scoped_lock l(kernel_lock_);
	    k = kernel_requests_.front();
	    kernel_requests_.pop_front();
	  }

		VLOG(1) << "Received run request for kernel id: " << k.kernel_id();
		KernelFunction f = KernelRegistry::get_kernel(k.kernel_id());
		f();
		VLOG(1) << "Waiting for network to finish: " << pending_network_writes() << " : " << pending_kernel_bytes();

	  while (pending_kernel_bytes() > 0 || pending_network_writes()) {
	    Sleep(0.01);
	  }

	  kernel_done_ = true;

	  VLOG(1) << "Kernel done.";
#ifdef CPUPROF
    ProfilerFlush();
#endif
	}
}

bool Worker::pending_network_writes() const {
  for (int i = 0; i < peers.size(); ++i) {
    if (peers[i]->write_pending()) {
      return true;
    }
  }
  return false;
}

int64_t Worker::pending_kernel_bytes() const {
  int64_t t = 0;

  for (TableMap::const_iterator i = tables.begin(); i != tables.end(); ++i) {
    t += i->second->pending_write_bytes();
  }

  return t;
}

void Worker::ComputeUpdates(Peer *p, Table::Iterator *it) {
  HashUpdate *r = &p->writeScratch;
  r->Clear();

  r->set_source(config.worker_id());
  r->set_table_id(it->owner()->info().table_id);

  int bytesUsed = 0;
  int count = 0;
  string k, v;
  for (; !it->done() && bytesUsed < kMaxNetworkChunk; it->Next()) {
    it->key_str(&k);
    it->value_str(&v);

    r->add_put(k, v);
    ++count;
  }

  VLOG(1) << "Prepped " << count << " taking " << r->ByteSize();

  p->Send(MTYPE_KERNEL_DATA, r, rpc_);

  stats_.set_put_out(stats_.put_out() + 1);
  stats_.set_bytes_out(stats_.bytes_out() + r->ByteSize());
  ++count;
}

void Worker::Poll() {
  HashUpdate scratch;

  for (int i = 0; i < peers.size(); ++i) {
    Peer *p = peers[i];
    p->CollectPendingSends();
    p->ReceiveIncomingData(rpc_);

    while (!p->incomingData.empty()) {
      HashUpdate *r = p->pop_data();
      stats_.set_put_in(stats_.put_in() + 1);
      stats_.set_bytes_in(stats_.bytes_in() + r->ByteSize());

      Table *t = get_table(r->table_id(), r->shard());
      t->ApplyUpdates(*r);
      delete r;
    }

    while (!p->incomingRequests.empty()) {
      HashRequest *r = p->pop_request();
      stats_.set_get_in(stats_.get_in() + 1);
      stats_.set_bytes_in(stats_.bytes_in() + r->ByteSize());

      scratch.Clear();
      scratch.set_source(config.worker_id());
      scratch.set_table_id(r->table_id());

      VLOG(1) << "Returning result for " << r->key() << " :: table " << r->table_id();
      string v = tables[make_pair(r->table_id(), r->shard())]->get_local(r->key());

      scratch.add_put(r->key(), v);

      p->Send(MTYPE_GET_RESPONSE, &scratch, rpc_);
      delete r;
    }
  }

  // Check for shutdown.
  {
    EmptyMessage msg;
    ProtoWrapper wrapper(msg);

    if (rpc_->TryRead(config.master_id(), MTYPE_WORKER_SHUTDOWN, &wrapper)) {
      VLOG(1) << "Shutting down worker " << config.worker_id();
      running_ = false;
      return;
    }
  }

  // Check for new kernels to run.
  {
    RunKernelRequest k;
    ProtoWrapper wrapper(k);
    if (rpc_->TryRead(config.master_id(), MTYPE_RUN_KERNEL, &wrapper)) {
      boost::recursive_mutex::scoped_lock sl(kernel_lock_);
      kernel_requests_.push_back(k);
    }
  }

  // If the current kernel has completed, send a message to master indicating
  // that we've completed our kernel and we are done transmitting.
  if (kernel_done_) {
    EmptyMessage m;
    rpc_->Send(config.master_id(), MTYPE_KERNEL_DONE, ProtoWrapper(m));

    kernel_done_ = false;
  }
}

} // end namespace
