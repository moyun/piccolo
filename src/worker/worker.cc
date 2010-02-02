#include <boost/bind.hpp>
#include <signal.h>
#ifdef CPUPROF
#include <google/profiler.h>
#endif

#include "util/common.h"
#include "worker/worker.h"
#include "worker/registry.h"

namespace dsm {
static const int kMaxNetworkChunk = 1 << 22;
static const int kNetworkTimeout = 5.0;

struct Worker::Peer {

  // An update request containing changes to apply to a remote table.
  struct Request {
    int target;
    int rpc_type;
    string payload;
    MPI::Request mpi_req;
    double start_time;

    Request() : start_time(Now()) {}
    ~Request() {}

    double elapsed() { return Now() - start_time; }
    double timed_out() { return elapsed() > kNetworkTimeout; }
  };

  HashUpdate write_scratch;
  unordered_set<Request*> outgoing_requests_;

  int32_t id;
  RPCHelper *helper;
  int64_t pending_out_;

  Peer(int id, RPCHelper* rpc) : id(id), helper(rpc), pending_out_(0) {}

  // Send the given message type and data to this peer.
  Request* Send(int rpc_type, const RPCMessage& ureq) {
    Request* r = new Request();
    r->target = id;
    r->rpc_type = rpc_type;
    ureq.AppendToString(&r->payload);

    r->mpi_req = helper->SendData(r->target, r->rpc_type, r->payload);
    outgoing_requests_.insert(r);
    pending_out_ += r->payload.size();

    return r;
  }

  void CollectPendingSends() {
    unordered_set<Request*>::iterator i = outgoing_requests_.begin();
    while (i != outgoing_requests_.end()) {
      Request *r = (*i);
      if (r->mpi_req.Test() || r->timed_out()) {
        VLOG(2) << "Request of size " << r->payload.size() << " finished.";

        if (r->timed_out()) {
          LOG_EVERY_N(INFO, 100) << "Send of " << r->payload.size() << " to " << r->target << " timed out.";
          r->mpi_req.Cancel();
        }

        pending_out_ -= r->payload.size();
        delete r;
        i = outgoing_requests_.erase(i);
      } else {
        ++i;
      }
    }
#undef REMOVE
  }

  int64_t pending_out_bytes() const { return pending_out_; }
};

Worker::Worker(const ConfigData &c) {
  config_.CopyFrom(c);
  config_.set_worker_id(MPI::COMM_WORLD.Get_rank() - 1);

  world_ = MPI::COMM_WORLD;
  rpc_ = new RPCHelper(&world_);

  num_peers_ = config_.num_workers();
  peers_.resize(num_peers_);
  for (int i = 0; i < num_peers_; ++i) {
    peers_[i] = new Peer(i + 1, rpc_);
  }

  running_ = true;

  kernel_thread_ = network_thread_ = NULL;

  // HACKHACKHACK - register ourselves with any existing tables
  Registry::TableMap &t = Registry::get_tables();
  for (Registry::TableMap::iterator i = t.begin(); i != t.end(); ++i) {
    i->second->info_.worker = this;
  }

  LOG(INFO) << "Worker " << config_.worker_id() << " started.";
}

int Worker::peer_for_shard(int table, int shard) const {
  return Registry::get_tables()[table]->get_owner(shard);
}

void Worker::Run() {
  KernelLoop();
}

Worker::~Worker() {
  running_ = false;
  delete kernel_thread_;
  delete network_thread_;

  for (int i = 0; i < peers_.size(); ++i) {
    delete peers_[i];
  }
}

void Worker::Send(int peer, int type, const RPCMessage& msg) {
  peers_[peer - 1]->Send(type, msg);
}

void Worker::Read(int peer, int type, RPCMessage* msg) {
  while (!rpc_->HasData(peer, type)) {
    PollPeers();
  }

  rpc_->Read(peer, type, msg);
}

void Worker::SendUpdate(LocalTable *t) {
  Peer *p = peers_[peer_for_shard(t->id(), t->shard())];

  Table::Iterator *i = t->get_iterator();
  while (!i->done()) {
    SendPartial(p, i);
    PollPeers();
  }
  delete i;

  PollPeers();
}

void Worker::KernelLoop() {
  MPI::Intracomm world = MPI::COMM_WORLD;

  while (running_) {
    if (kernel_queue_.empty()) {
      PERIODIC(0.1,  { PollMaster(); } );
      PollPeers();
      continue;
    }

    KernelRequest k = kernel_queue_.front();
    kernel_queue_.pop_front();

    VLOG(1) << "Received run request for kernel id: " << k.kernel() << ":" << k.method() << ":" << k.shard();

    if (peer_for_shard(k.table(), k.shard()) != config_.worker_id()) {
      LOG(FATAL) << "Received a shard I can't work on! : " << k.shard() << " : " << peer_for_shard(k.table(), k.shard());
    }

    KernelInfo *helper = Registry::get_kernel_info(k.kernel());

    KernelId id(k.kernel(), k.table(), k.shard());
    DSMKernel* d = kernels_[id];

    if (!d) {
      d = helper->create();
      kernels_[id] = d;
      d->Init(this, k.table(), k.shard());
      d->KernelInit();
    }

    helper->invoke_method(d, k.method());
    kernel_done_.push_back(k);

    for (Registry::TableMap::iterator i = Registry::get_tables().begin();
         i != Registry::get_tables().end(); ++i) {
      i->second->SendUpdates();
    }

    VLOG(1) << "Kernel done.";
#ifdef CPUPROF
    ProfilerFlush();
#endif
  }
}

int64_t Worker::pending_network_bytes() const {
  int64_t t = 0;

  for (int i = 0; i < peers_.size(); ++i) {
    t += peers_[i]->pending_out_bytes();
  }

  return t;
}

int64_t Worker::pending_kernel_bytes() const {
  int64_t t = 0;

  Registry::TableMap &tmap = Registry::get_tables();
  for (Registry::TableMap::iterator i = tmap.begin(); i != tmap.end(); ++i) {
    t += ((GlobalTable*)i->second)->pending_write_bytes();
  }

  return t;
}

bool Worker::network_idle() const {
  return pending_network_bytes() == 0;
}

void Worker::SendPartial(Peer *p, Table::Iterator *it) {
  HashUpdate &r = p->write_scratch;
  r.Clear();

  r.set_shard(it->owner()->shard());
  r.set_source(config_.worker_id());
  r.set_table_id(it->owner()->info().table_id);

  int bytesUsed = 0;
  int count = 0;
  string k, v;
  for (; !it->done() && bytesUsed < kMaxNetworkChunk; it->Next()) {
    it->key_str(&k);
    it->value_str(&v);

    r.add_put(k, v);
    ++count;
    bytesUsed += k.size() + v.size();
  }

  VLOG(2) << "Prepped " << count << " taking " << bytesUsed;

  p->Send(MTYPE_PUT_REQUEST, r);

  stats_.set_put_out(stats_.put_out() + 1);
  stats_.set_bytes_out(stats_.bytes_out() + r.ByteSize());
  ++count;
}

void Worker::PollPeers() {
  for (int i = 0; i < peers_.size(); ++i) {
    Peer *p = peers_[i];
    p->CollectPendingSends();
  }

  HashUpdate put_req;
  while (rpc_->TryRead(MPI::ANY_SOURCE, MTYPE_PUT_REQUEST, &put_req)) {
    stats_.set_put_in(stats_.put_in() + 1);
    stats_.set_bytes_in(stats_.bytes_in() + put_req.ByteSize());

    Table *t = Registry::get_table(put_req.table_id());
    t->ApplyUpdates(put_req);
  }

  MPI::Status status;
  HashRequest get_req;
  HashUpdate get_resp;
  while (rpc_->HasData(MPI::ANY_SOURCE, MTYPE_GET_REQUEST, status)) {
    rpc_->Read(MPI::ANY_SOURCE, MTYPE_GET_REQUEST, &get_req);

    stats_.set_get_in(stats_.get_in() + 1);
    stats_.set_bytes_in(stats_.bytes_in() + get_req.ByteSize());

    get_resp.Clear();
    get_resp.set_source(config_.worker_id());
    get_resp.set_table_id(get_req.table_id());

    VLOG(3) << "Returning result for " << get_req.key() << " :: table " << get_req.table_id();
    string v;
    Registry::get_table(get_req.table_id())->get_local(get_req.key(), &v);

    get_resp.add_put(get_req.key(), v);

    peers_[status.Get_source() - 1]->Send(MTYPE_GET_RESPONSE, get_resp);
  }
}

void Worker::PollMaster() {
  // Check for shutdown.
  EmptyMessage msg;
  if (rpc_->TryRead(config_.master_id(), MTYPE_WORKER_SHUTDOWN, &msg)) {
    VLOG(1) << "Shutting down worker " << config_.worker_id();
    running_ = false;
    return;
  }

  ShardAssignmentRequest shard_req;
  if (rpc_->TryRead(config_.master_id(), MTYPE_SHARD_ASSIGNMENT, &shard_req)) {
    for (int i = 0; i < shard_req.assign_size(); ++i) {
      const ShardAssignment &a = shard_req.assign(i);
      VLOG(1)  << "Setting owner of " << a.table() << "," << a.shard() << " to " << a.new_worker();

      Registry::get_table(a.table())->set_owner(a.shard(), a.new_worker());
    }
  }

  // Check for new kernels to run, and report finished kernels to the master.
  if (network_idle()) {
    KernelRequest k;
    if (rpc_->TryRead(config_.master_id(), MTYPE_RUN_KERNEL, &k)) {
      kernel_queue_.push_back(k);
    }

    while (!kernel_done_.empty()) {
      rpc_->Send(config_.master_id(), MTYPE_KERNEL_DONE, kernel_done_.front());
      kernel_done_.pop_front();
    }
  }
}

} // end namespace
