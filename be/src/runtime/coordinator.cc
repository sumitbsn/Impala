// Copyright (c) 2011 Cloudera, Inc. All rights reserved.

#include "runtime/coordinator.h"

#include <limits>
#include <glog/logging.h>
#include <gflags/gflags.h>
#include <transport/TTransportUtils.h>
#include <boost/algorithm/string/join.hpp>
#include <boost/accumulators/accumulators.hpp>
#include <boost/accumulators/statistics/stats.hpp>
#include <boost/accumulators/statistics/min.hpp>
#include <boost/accumulators/statistics/mean.hpp>
#include <boost/accumulators/statistics/median.hpp>
#include <boost/accumulators/statistics/max.hpp>
#include <boost/accumulators/statistics/variance.hpp>
#include <boost/bind.hpp>

#include "exec/data-sink.h"
#include "runtime/client-cache.h"
#include "runtime/data-stream-sender.h"
#include "runtime/data-stream-mgr.h"
#include "runtime/exec-env.h"
#include "runtime/plan-fragment-executor.h"
#include "runtime/row-batch.h"
#include "runtime/parallel-executor.h"
#include "sparrow/scheduler.h"
#include "exec/exec-stats.h"
#include "exec/data-sink.h"
#include "util/debug-util.h"
#include "gen-cpp/ImpalaInternalService.h"
#include "gen-cpp/ImpalaInternalService_types.h"

using namespace std;
using namespace boost;
using namespace boost::accumulators;
using namespace apache::thrift::transport;

DECLARE_int32(be_port);
DECLARE_string(host);

namespace impala {

Coordinator::Coordinator(ExecEnv* exec_env, ExecStats* exec_stats)
  : exec_env_(exec_env),
    has_called_wait_(false),
    executor_(new PlanFragmentExecutor(exec_env)),
    execution_completed_(false),
    exec_stats_(exec_stats) {
}

Coordinator::~Coordinator() {
}

Status Coordinator::Exec(TQueryExecRequest* request) {
  query_id_ = request->query_id;
  VLOG_QUERY << "Coordinator::Exec() stmt=" << request->sql_stmt;

  // fragment 0 is the coordinator/"local" fragment that we're executing ourselves;
  // start this before starting any more plan fragments in backend threads, otherwise
  // they start sending data before the local exchange node had a chance to register
  // with the stream mgr
  DCHECK_GT(request->node_request_params.size(), 0);
  // the first node_request_params list contains exactly one TPlanExecParams
  // (it's meant for the coordinator fragment)
  DCHECK_EQ(request->node_request_params[0].size(), 1);

  // to keep things simple, make async Cancel() calls wait until plan fragment
  // execution has been initiated
  lock_guard<mutex> l(lock_);

  // register data streams for coord fragment
  RETURN_IF_ERROR(executor_->Prepare(
      request->fragment_requests[0], request->node_request_params[0][0]));

  if (request->node_request_params.size() > 1) {
    // for now, set destinations of 2nd fragment to coord host/port
    // TODO: determine execution hosts first, then set destinations to those hosts
    for (int i = 0; i < request->node_request_params[1].size(); ++i) {
      DCHECK_EQ(request->node_request_params[1][i].destinations.size(), 1);
      request->node_request_params[1][i].destinations[0].host = FLAGS_host;
      request->node_request_params[1][i].destinations[0].port = FLAGS_be_port;
    }
  }

  query_profile_.reset(
      new RuntimeProfile(obj_pool(), "Query(id=" + PrintId(request->query_id) + ")"));
  COUNTER_SCOPED_TIMER(query_profile_->total_time_counter());

  // Start non-coord fragments on remote nodes;
  // fragment_requests[i] can receive data from fragment_requests[>i],
  // so start fragments in ascending order.
  int backend_num = 0;
  for (int i = 1; i < request->fragment_requests.size(); ++i) {
    DCHECK(exec_env_ != NULL);
    // TODO: change this in the following way:
    // * add locations to request->node_request_params.scan_ranges
    // * pass in request->node_request_params and let the scheduler figure out where
    // we should be doing those scans, rather than the frontend
    vector<pair<string, int> > hosts;
    RETURN_IF_ERROR(
        exec_env_->scheduler()->GetHosts(request->data_locations[i-1], &hosts));
    DCHECK_EQ(hosts.size(), request->node_request_params[i].size());

    // start individual plan exec requests
    // TODO: to start up more quickly, we need to start fragment_requests[i]
    // on all backends in parallel (ie, we need to have a server-wide pool of threads
    // that we use to start plan fragments at backends)
    for (int j = 0; j < hosts.size(); ++j) {
      // assign fragment id that's unique across all fragment executions;
      // backend_num + 1: backend_num starts at 0, and the coordinator fragment 
      // is already assigned the query id
      TUniqueId fragment_id;
      fragment_id.hi = request->query_id.hi;
      DCHECK_LT(request->query_id.lo, numeric_limits<int64_t>::max() - backend_num - 1);
      fragment_id.lo = request->query_id.lo + backend_num + 1;

      // TODO: pool of pre-formatted BackendExecStates?
      BackendExecState* exec_state =
          obj_pool()->Add(new BackendExecState(fragment_id, backend_num, hosts[j],
                &request->fragment_requests[i],
                &request->node_request_params[i][j]));
      DCHECK_EQ(backend_exec_states_.size(), backend_num);
      backend_exec_states_.push_back(exec_state);
      ++backend_num;
    }
    PrintBackendInfo();
  }
  
  // Issue all rpcs in parallel
  Status fragments_exec_status = ParallelExecutor::Exec(
      bind<Status>(mem_fn(&Coordinator::ExecRemoteFragment), this, _1), 
      reinterpret_cast<void**>(&backend_exec_states_[0]), backend_exec_states_.size());

  // Clear state in backend_exec_states_ that is only guaranteed to exist for the
  // duration of this function
  for (int i = 0; i < backend_exec_states_.size(); ++i) {
    backend_exec_states_[i]->exec_request = NULL;
    backend_exec_states_[i]->exec_params = NULL;
  }

  if (!fragments_exec_status.ok()) {
    // tear down running fragments and return
    Cancel(false);
    return fragments_exec_status;
  }

  return Status::OK;
}

Status Coordinator::Wait() {
  lock_guard<mutex> l(wait_lock_);
  if (has_called_wait_) return Status::OK;
  has_called_wait_ = true;
  // Open() may block
  RETURN_IF_ERROR(executor_->Open());
  return Status::OK;
}

void Coordinator::BackendExecState::ComputeTotalSplitSize(
    const TPlanExecParams& params) {
  if (params.scan_ranges.empty()) return;
  total_split_size = 0;
  for (int i = 0; i < params.scan_ranges[0].hdfsFileSplits.size(); ++i) {
    total_split_size += params.scan_ranges[0].hdfsFileSplits[i].length;
  }
}

void Coordinator::PrintBackendInfo() {
  accumulator_set<int64_t, features<tag::min, tag::max, tag::mean, tag::variance> > acc;
  for (int i = 0; i < backend_exec_states_.size(); ++i) {
    acc(backend_exec_states_[i]->total_split_size);
  }
  double min = accumulators::min(acc);
  double max = accumulators::max(acc);
  // TODO: including the median doesn't compile, looks like some includes are missing
  //double median = accumulators::median(acc);
  double mean = accumulators::mean(acc);
  double stddev = sqrt(accumulators::variance(acc));
  VLOG_QUERY << "split sizes for " << backend_exec_states_.size() << " backends:"
             << " min: " << PrettyPrinter::Print(min, TCounterType::BYTES)
             << ", max: " << PrettyPrinter::Print(max, TCounterType::BYTES)
             //<< ", median: " << PrettyPrinter::Print(median, TCounterType::BYTES)
             << ", avg: " << PrettyPrinter::Print(mean, TCounterType::BYTES)
             << ", stddev: " << PrettyPrinter::Print(stddev, TCounterType::BYTES);
  if (VLOG_FILE_IS_ON) {
    for (int i = 0; i < backend_exec_states_.size(); ++i) {
      BackendExecState* exec_state = backend_exec_states_[i];
      VLOG_FILE << "data volume for host " << exec_state->hostport.first
                << ":" << exec_state->hostport.second << ": "
                << PrettyPrinter::Print(
                  exec_state->total_split_size, TCounterType::BYTES);
    }
  }
}

Status Coordinator::ExecRemoteFragment(void* exec_state_arg) {
  BackendExecState* exec_state = reinterpret_cast<BackendExecState*>(exec_state_arg);
  VLOG_FILE << "making rpc: ExecPlanFragment query_id=" << query_id_
            << " fragment_id=" << exec_state->fragment_id
            << " host=" << exec_state->hostport.first
            << " port=" << exec_state->hostport.second;
  lock_guard<mutex> l(exec_state->lock);

  // this client needs to have been released when this function finishes
  ImpalaInternalServiceClient* backend_client;
  RETURN_IF_ERROR(exec_env_->client_cache()->GetClient(
      exec_state->hostport, &backend_client));
  DCHECK(backend_client != NULL);

  TExecPlanFragmentParams params;
  params.protocol_version = ImpalaInternalServiceVersion::V1;
  // TODO: is this yet another copy? find a way to avoid those.
  params.__set_request(*exec_state->exec_request);
  params.request.fragment_id = exec_state->fragment_id;
  params.__set_params(*exec_state->exec_params);
  params.coord.host = FLAGS_host;
  params.coord.port = FLAGS_be_port;
  params.__isset.coord = true;
  params.__set_backend_num(exec_state->backend_num);

  TExecPlanFragmentResult thrift_result;
  try {
    backend_client->ExecPlanFragment(thrift_result, params);
  } catch (TTransportException& e) {
    stringstream msg;
    msg << "ExecPlanRequest rpc query_id=" << query_id_
        << " fragment_id=" << exec_state->fragment_id 
        << " failed: " << e.what();
    LOG(ERROR) << msg.str();
    exec_state->status = Status(msg.str());
    exec_env_->client_cache()->ReleaseClient(backend_client);
    return exec_state->status;
  }
  exec_state->status = thrift_result.status;
  exec_env_->client_cache()->ReleaseClient(backend_client);
  if (exec_state->status.ok()) exec_state->initiated = true;
  return exec_state->status;
}

Status Coordinator::GetNext(RowBatch** batch, RuntimeState* state) {
  Status status = GetNextInternal(batch, state);
  // close the executor if we see an error status (including cancellation) or 
  // hit the end
  if (!status.ok() || execution_completed_) {
    status.AddError(executor_->Close());
  }
  if (execution_completed_ && VLOG_QUERY_IS_ON) {
    stringstream s;
    query_profile_->PrettyPrint(&s);
    VLOG_QUERY << "cumulative profile for query_id=" << query_id_ << "\n"
               << s.str();
  }
  return status;
}

Status Coordinator::GetNextInternal(RowBatch** batch, RuntimeState* state) {
  DCHECK(has_called_wait_);
  COUNTER_SCOPED_TIMER(query_profile_->total_time_counter());
  VLOG_ROW << "coord.getnext";
  RETURN_IF_ERROR(executor_->GetNext(batch));
  if (*batch == NULL) {
    execution_completed_ = true;
    query_profile_->AddChild(executor_->query_profile());
  } else {
    exec_stats_->num_rows_ += (*batch)->num_rows();
  }
  return Status::OK;
}

void Coordinator::Cancel() {
  Cancel(true);
}

void Coordinator::Cancel(bool get_lock) {
  // if requested, synchronize Cancel() with a possibly concurrently running Exec()
  mutex dummy;
  lock_guard<mutex> l(get_lock ? lock_ : dummy);

  // cancel local fragment
  if (executor_.get() != NULL) {
    executor_->runtime_state()->set_is_cancelled(true);
    // cancel all incoming data streams
    exec_env_->stream_mgr()->Cancel(runtime_state()->fragment_id());
  }

  for (int i = 0; i < backend_exec_states_.size(); ++i) {
    BackendExecState* exec_state = backend_exec_states_[i];

    // lock each exec_state individually to synchronize correctly with
    // UpdateFragmentExecStatus() (which doesn't get the global lock_)
    lock_guard<mutex> l(exec_state->lock);

    // Nothing to cancel if the exec rpc was not sent
    if (!exec_state->initiated) continue;

    // don't cancel if it already finished
    if (exec_state->done) continue;

    // if we get an error while trying to get a connection to the backend,
    // keep going
    ImpalaInternalServiceClient* backend_client;
    Status status =
        exec_env_->client_cache()->GetClient(exec_state->hostport, &backend_client);
    if (!status.ok()) {
      continue;
    }
    DCHECK(backend_client != NULL);

    TCancelPlanFragmentParams params;
    params.protocol_version = ImpalaInternalServiceVersion::V1;
    params.__set_fragment_id(exec_state->fragment_id);
    TCancelPlanFragmentResult res;
    try {
      backend_client->CancelPlanFragment(res, params);
    } catch (TTransportException& e) {
      stringstream msg;
      msg << "CancelPlanFragment rpc query_id=" << query_id_
          << " fragment_id=" << exec_state->fragment_id 
          << " failed: " << e.what();
      // make a note of the error status, but keep on cancelling the other fragments
      if (exec_state->status.ok()) {
        exec_state->status = Status(msg.str());
      } else {
        // if we already recorded a failure, keep on adding error msgs
        exec_state->status.AddErrorMsg(msg.str());
      }
      exec_env_->client_cache()->ReleaseClient(backend_client);
      continue;
    }
    if (res.status.status_code != TStatusCode::OK) {
      if (exec_state->status.ok()) {
        exec_state->status = Status(algorithm::join(res.status.error_msgs, "; "));
      } else {
        // if we already recorded a failure, keep on adding error msgs
        exec_state->status.AddErrorMsg(algorithm::join(res.status.error_msgs, "; "));
      }
    }
    exec_env_->client_cache()->ReleaseClient(backend_client);
  }
}

Status Coordinator::UpdateFragmentExecStatus(
    int backend_num, const TStatus& tstatus, bool done,
    const TRuntimeProfileTree& cumulative_profile) {
  if (backend_num >= backend_exec_states_.size()) {
    return Status(TStatusCode::INTERNAL_ERROR, "unknown backend number");
  }
  BackendExecState* exec_state = backend_exec_states_[backend_num];

  Status status(tstatus);
  {
    lock_guard<mutex> l(exec_state->lock);
    // make sure we don't go from error status to OK
    DCHECK(!status.ok() || exec_state->status.ok())
        << "fragment is transitioning from error status to OK:"
        << " query_id=" << query_id_ << " fragment_id=" << exec_state->fragment_id
        << " status=" << exec_state->status.GetErrorMsg();
    exec_state->status = status;
    exec_state->done = done;
    exec_state->profile =
        RuntimeProfile::CreateFromThrift(obj_pool(), cumulative_profile);
  }
  if (done) {
    DCHECK(exec_state->profile != NULL);
    // TODO: this is not going to work for getting incremental stats.
    // We need a way to modify counters in place (instead of creating a new 
    // RuntimeProfile from thrift) or a way to remove existing profile children
    // TODO: think about the thread safety of query_profile_
    query_profile_->AddChild(exec_state->profile);
    if (VLOG_FILE_IS_ON) {
      stringstream s;
      exec_state->profile->PrettyPrint(&s);
      VLOG_FILE << "profile for query_id=" << query_id_
                << " fragment_id=" << exec_state->fragment_id << "\n" << s.str();
    }
    // also print the cumulative profile
    // TODO: fix the coordinator/PlanFragmentExecutor, so this isn't needed
    if (VLOG_FILE_IS_ON) {
      stringstream s;
      query_profile_->PrettyPrint(&s);
      VLOG_FILE << "cumulative profile for query_id=" << query_id_ 
                << "\n" << s.str();
    }
  }

  // for now, abort the query if we see any error
  if (!status.ok()) Cancel();
  return Status::OK;
}

const RowDescriptor& Coordinator::row_desc() const {
  DCHECK(executor_.get() != NULL);
  return executor_->row_desc();
}

RuntimeState* Coordinator::runtime_state() {
  DCHECK(executor_.get() != NULL);
  return executor_->runtime_state();
}


ObjectPool* Coordinator::obj_pool() {
  DCHECK(executor_.get() != NULL);
  return executor_->runtime_state()->obj_pool();
}

}
