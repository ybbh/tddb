#include "concurrency/tx_context.h"
#include "concurrency/violate.h"
#include "common/shard2node.h"
#ifdef DB_TYPE_NON_DETERMINISTIC
#include "common/define.h"
#include "common/logger.hpp"
#include "common/utils.h"
#include <boost/assert.hpp>
#include <utility>

template<>
enum_strings<rm_state>::e2s_t enum_strings<rm_state>::enum2str = {
    {RM_IDLE, "RM_IDLE"},
    {RM_COMMITTING, "RM_COMMITTING"},
    {RM_ABORTING, "RM_ABORTING"},
    {RM_PREPARE_COMMITTING, "RM_PREPARE_COMMITTING"},
    {RM_PREPARE_ABORTING, "RM_PREPARE_ABORTING"},
    {RM_ENDED, "RM_ENDED"},
};

tx_context::tx_context(boost::asio::io_context::strand s, uint64_t xid,
                       uint32_t node_id, std::optional<node_id_t> dsb_node_id,
                       const std::unordered_map<shard_id_t, node_id_t> &shard2node,
                       uint64_t cno,
                       bool distributed, lock_mgr_global *mgr,
                       access_mgr *access,
                       net_service *service,
                       ptr<connection> conn, write_ahead_log *write_ahead_log,
                       fn_tx_state fn, deadlock *dl)
    : tx_rm(s, xid), cno_(cno), node_id_(node_id),
      node_name_(id_2_name(node_id)), ctx_opt_dsb_node_id_(dsb_node_id),
      shard_id_2_node_id_(shard2node),
      xid_(xid),
      distributed_(distributed), coord_node_id_(0), oid_(1), max_ops_(0),
      mgr_(mgr), access_(access), service_(service), cli_conn_(std::move(conn)),
      error_code_(EC::EC_OK), state_(rm_state::RM_IDLE), lock_acquire_(nullptr),
      wal_(write_ahead_log), has_respond_(false), fn_tx_state_(std::move(fn)),
      prepare_commit_log_synced_(false), commit_log_synced_(false), dl_(dl),
      victim_(false), log_rep_delay_(0), latency_read_dsb_(0),
      num_read_violate_(0), num_write_violate_(0), num_lock_(0), timeout_invoked_(false),
      read_only_(false)
      {
  BOOST_ASSERT(node_id != 0);
  BOOST_ASSERT(dsb_node_id != 0);
  start_ = steady_clock_ms_since_epoch();
  part_time_tracer_.begin();
  LOG(trace) << node_name_ << " transaction RM " << xid_ << " construct";
}

void tx_context::begin() {
#ifdef TX_TRACE
  auto rm = shared_from_this();
  auto fn_timeout = [rm] {
    if (rm->timeout_invoked_) {
      return;
    }

    auto ms = steady_clock_ms_since_epoch();

    if (ms < rm->start_ + TX_TIMEOUT_MILLIS) {
      std::string trace = rm->trace_message_.str();
      if (trace.find("RESP;") == std::string::npos) {
        LOG(warning) << "no RESP" << trace;
      }

      if (rm->state_ == RM_ENDED ||
           rm->state_ == RM_ABORTING ||
           rm->state_ == RM_COMMITTING) {
        rm->timer_tick_->cancel();
        return;
      }

      rm->timeout_invoked_ = true;
      LOG(warning) << rm->node_name_ <<
          " tx: " << ms - rm->start_ <<
          " wait ms, " << rm->xid_ <<
          " trace" << trace;
    }
  };
  ptr<timer> t(new timer(
      get_strand(), boost::asio::chrono::milliseconds(TX_TIMEOUT_MILLIS),
      fn_timeout));
  timer_tick_ = t;
  t->async_tick();
#endif
}

void tx_context::notify_lock_acquire(
    EC ec, const ptr<std::vector<ptr<tx_context>>> &in) {
  auto ctx = shared_from_this();
  auto fn = [ctx, ec, in]() {
    scoped_time _t("tx_context lock_acquire");
#ifdef TX_TRACE
    ctx->trace_message_ << "lk ntf;";
#endif
    if (ctx->lock_acquire_) {
      auto fn = ctx->lock_acquire_;
      ctx->lock_acquire_ = nullptr;
      fn(ec);
    } else {
      BOOST_ASSERT(false);
    }
  };
  boost::asio::post(ctx->get_strand(), fn);
}

void tx_context::async_lock_acquire(EC ec, oid_t) {
  notify_lock_acquire(ec, nullptr);
}

void tx_context::async_read(uint32_t table_id, shard_id_t shard_id, tuple_id_t key,
                            bool read_for_write, fn_ec_tuple fn_read_done) {
  uint32_t oid = oid_++;
  lock_mode lt = lock_mode::LOCK_READ_ROW;
  if (read_for_write) {
    lt = lock_mode::LOCK_WRITE_ROW;
  }
  auto s = shared_from_this();
  BOOST_ASSERT(lock_acquire_ == nullptr);
  ptr<lock_item> l(new lock_item(s->xid_, oid, lt, table_id, shard_id, predicate(key)));
  auto r = s->locks_.insert(std::make_pair(oid, l));
  if (r.second) {
    num_lock_++;
  }
  lock_acquire_ = [table_id, shard_id, key, oid, s, fn_read_done](EC ec) {
    s->lock_wait_time_tracer_.end();

    if (ec == EC::EC_OK) {
      std::pair<tuple_pb, bool> r = s->access_->get(table_id, shard_id, key);

      if (r.second) {
        BOOST_ASSERT(not(ec == EC::EC_OK && is_tuple_nil(r.first)));
        fn_read_done(ec, std::move(r.first)); // tuple would be moved
      } else {                                // read from DSB
        auto fn_read_from_dsb = [fn_read_done](EC ec, tuple_pb &&tuple) {
          BOOST_ASSERT(not(ec == EC::EC_OK && is_tuple_nil(tuple)));
          fn_read_done(ec, std::move(tuple));
        };

        s->read_data_from_dsb(table_id, shard_id, key, oid, fn_read_from_dsb);
      }
    } else { // error
      LOG(trace) << "cannot find tuple, table id:" << table_id
                 << " tuple id:" << (key);
      fn_read_done(ec, tuple_pb());
    }
  };

  BOOST_ASSERT(mgr_);
#ifdef TX_TRACE
  trace_message_ << "lk " << table_id << " : " <<
                    key << " : " << oid <<";";
#endif
  lock_wait_time_tracer_.begin();
  if (read_only_) {
    lock_acquire_(EC::EC_OK);
    lock_acquire_ = nullptr;
  } else {
    mgr_->lock_row(xid_, oid, lt, table_id, shard_id, predicate(key), shared_from_this());
  }
}

void tx_context::async_update(uint32_t table_id, shard_id_t shard_id, tuple_id_t key,
                              tuple_pb &&tuple, fn_ec fn_update_done) {
  uint32_t oid = oid_++;
  auto s = shared_from_this();
  BOOST_ASSERT(lock_acquire_ == nullptr);
  ptr<lock_item> l(
      new lock_item(s->xid_, oid, LOCK_WRITE_ROW, table_id, shard_id, predicate(key)));
  auto r = s->locks_.insert(std::make_pair(oid, l));
  if (r.second) {
    num_lock_++;
  }
  lock_acquire_ = [table_id, shard_id, key, oid, s, fn_update_done,
      tuple = std::move(tuple)](EC ec) {
    s->lock_wait_time_tracer_.end();

    if (ec == EC::EC_OK) {
      std::pair<tuple_pb, bool> r = s->access_->get(table_id, shard_id, key);
      if (r.second) {
        fn_update_done(EC::EC_OK);
      } else {
        auto fn_read_done = [s, fn_update_done](EC ec, const tuple_pb &) {
          fn_update_done(ec);
        };

        s->read_data_from_dsb(table_id, shard_id, key, oid, fn_read_done);
      }
    } else {
      LOG(trace) << "cannot find tuple, table id:" << table_id
                 << " tuple id:" << (key);
      fn_update_done(ec);
    }
  };

#ifdef TX_TRACE
  trace_message_ << "lk" << table_id << ":" <<
                    key << ":" << oid << ";";
#endif
  lock_wait_time_tracer_.begin();
  mgr_->lock_row(xid_, oid, LOCK_WRITE_ROW, table_id, shard_id, predicate(key),
                 shared_from_this());
}

void tx_context::async_insert(uint32_t table_id, shard_id_t shard_id, tuple_id_t key,
                              tuple_pb &&tuple, fn_ec fn_write_done) {
  uint32_t oid = oid_++;
  auto s = shared_from_this();
  BOOST_ASSERT(lock_acquire_ == nullptr);
  ptr<lock_item> l(
      new lock_item(s->xid_, oid, LOCK_WRITE_ROW, table_id, shard_id, predicate(key)));
  auto r = s->locks_.insert(std::make_pair(oid, l));
  if (r.second) {
    num_lock_++;
  }
  lock_acquire_ = [table_id, shard_id, key, oid, s, tuple = std::move(tuple),
      fn_write_done](EC ec) {
    s->lock_wait_time_tracer_.end();

    if (ec == EC::EC_OK) {
      std::pair<tuple_pb, bool> r = s->access_->get(table_id, shard_id, key);
      if (r.second) {
        fn_write_done(EC::EC_DUPLICATION_ERROR);
      } else {
        auto fn_read_done = [s, fn_write_done, tuple = std::move(tuple)](
            EC ec, const tuple_pb &&) {
          if (ec == EC::EC_OK) {
            fn_write_done(EC::EC_DUPLICATION_ERROR);
          } else if (ec == EC::EC_NOT_FOUND_ERROR) {
            // BOOST_ASSERT(is_tuple_nil(tuple_found));
            fn_write_done(EC::EC_OK);
          } else {
            fn_write_done(ec);
          }
        };

        s->read_data_from_dsb(
            table_id,
            shard_id,
            key, oid, fn_read_done);
      }
    } else {
      fn_write_done(ec);
    }
  };

#ifdef TX_TRACE
  trace_message_ << "lk" << table_id << ":" <<
                    key << ":" << oid << ";";
#endif
  lock_wait_time_tracer_.begin();
  mgr_->lock_row(xid_, oid, LOCK_WRITE_ROW,
                 table_id,
                 shard_id,
                 predicate(key),
                 shared_from_this());
}

void tx_context::async_remove(uint32_t table_id, shard_id_t shard_id, tuple_id_t key,
                              fn_ec_tuple fn_removed) {
  oid_t oid = oid_++;
  auto s = shared_from_this();
  ptr<lock_item> l(
      new lock_item(s->xid_, oid, LOCK_WRITE_ROW, table_id, shard_id, predicate(key)));
  auto r = s->locks_.insert(std::make_pair(oid, l));
  if (r.second) {
    num_lock_++;
  }
  BOOST_ASSERT(lock_acquire_ == nullptr);
  lock_acquire_ = [s, table_id, shard_id, key, fn_removed](EC ec) {
    s->lock_wait_time_tracer_.end();

    std::pair<tuple_pb, bool> r = s->access_->get(table_id, shard_id, key);
    if (r.second) {
      fn_removed(ec, std::move(r.first));
    } else {
      fn_removed(EC::EC_NOT_FOUND_ERROR, tuple_pb());
    }
  };

#ifdef TX_TRACE
  trace_message_ << "lk" << table_id << ":" <<
                    key << ":" << oid << ";";
#endif
  lock_wait_time_tracer_.begin();
  mgr_->lock_row(xid_, oid, LOCK_WRITE_ROW,
                 table_id,
                 shard_id,
                 predicate(key),
                 shared_from_this());
}

void tx_context::read_data_from_dsb(uint32_t table_id, shard_id_t shard_id, tuple_id_t key,
                                    uint32_t oid, fn_ec_tuple fn_read_done) {
#ifdef TX_TRACE
  trace_message_ << "rd dsb;";
#endif
  LOG(trace) << node_name_ << " tx " << xid_ << " read key from DSB, table id:" << table_id
             << " tuple id:" << key;
  node_id_t dest_node_id = shard2node(shard_id);
  auto req = std::make_shared<ccb_read_request>();
  req->set_source(node_id_);
  req->set_dest(dest_node_id);
  req->set_xid(xid_);
  req->set_oid(oid);
  req->set_shard_id(shard_id);
  req->set_table_id(table_id);
  req->set_cno(cno_);

  ds_read_handler_[oid] = fn_read_done;
  BOOST_ASSERT(fn_read_done);
  req->set_tuple_id(key);
  BOOST_ASSERT(dest_node_id != 0);
  read_time_tracer_.begin();

  result<void> r = service_->async_send(dest_node_id, C2D_READ_DATA_REQ, req, true);
  if (!r) {
    LOG(error) << "node " << dest_node_id << " async_send error "
               << r.error().message();
  }
}

void tx_context::read_data_from_dsb_response(
    const ptr<dsb_read_response> response,
    std::chrono::steady_clock::time_point ts) {

#ifdef TX_TRACE
  trace_message_ << "dsb rsp;";
#endif
  tuple_id_t key(response->tuple_row().tuple_id());
  shard_id_t shard_id = response->tuple_row().shard_id();
  EC ec = EC(response->error_code());
  tuple_pb tuple;
  auto table_id = response->tuple_row().table_id();
  LOG(trace) << node_name_ << " tx " << xid_
             << " read key from DSB response, table_id:" << table_id
             << " tuple id:" << (key);

  auto oid = response->oid();
  auto latency = response->latency_read_dsb();
  bool has_tuple = false;
  if (response->has_tuple_row() && !response->tuple_row().tuple().empty()) {
    has_tuple = true;
    tuple.swap(*response->mutable_tuple_row()->mutable_tuple());
  } else {
  }

  latency_read_dsb_ += latency;
  read_time_tracer_.end_ts(ts);

  BOOST_ASSERT(oid != 0);
  auto i = ds_read_handler_.find(oid);
  if (i != ds_read_handler_.end()) {

    fn_ec_tuple fn = i->second;
    // clone a new tuple
    tuple_pb tuple_cloned = tuple;
    fn(ec, std::move(tuple_cloned));
    ds_read_handler_.erase(i);
  } else {
    BOOST_ASSERT(false);
  }

  if (ec == EC::EC_OK) {
    if (has_tuple) {
      BOOST_ASSERT(!is_tuple_nil(tuple));
      access_->put(table_id, shard_id, key, std::move(tuple));
      // auto pair = mgr_->get(table_id, key);
      // BOOST_ASSERT(pair.second);
      LOG(trace) << node_name_ << " cached table:" << table_id << " key:" << key;
    } else {
      LOG(trace) << node_name_ << " no tuple:" << table_id << " key:" << key;
    }
  } else {
    LOG(trace) << node_name_ << " read error:" << ec << "" << table_id << " key:" << key;
  }
}

void tx_context::process_tx_request(const tx_request &req) {
  read_only_ = req.read_only();
  begin();

#ifdef TX_TRACE
  trace_message_ << "tx_rm rq;";
#endif

  // LOG(debug) << "tx_rm: " << xid_ << ", request ";
  if (req.distributed()) {
    coord_node_id_ = req.source();
  }
  max_ops_ = req.operations().size();
  if (req.oneshot()) {
    for (const tx_operation &op : req.operations()) {
      ops_.emplace_back(op);
    }
    handle_next_operation();
  } else {
    // TODO non oneshot tx_rm
  }
}

void tx_context::handle_next_operation() {

  if (state_ != RM_IDLE) {
    return;
  }

  if (error_code_ == EC::EC_OK) {
    if (!ops_.empty()) {
      auto s = shared_from_this();
      auto op_done = [s](EC ec) {
        s->ops_.pop_front();
        if (s->read_only_ && ec == EC_NOT_FOUND_ERROR) {
          s->error_code_ = EC::EC_OK;
        } else {
          s->error_code_ = ec;
        }
        s->handle_next_operation();
      };
      tx_operation &op = ops_.front();
      handle_operation(op, std::move(op_done));
    } else {
      assert(oid_ == max_ops_ + 1);
      if (distributed_) {
#ifdef DB_TYPE_SHARE_NOTHING
        if (is_shared_nothing()) {
          handle_finish_tx_phase1_prepare_commit();
        }
#endif // DB_TYPE_SHARE_NOTHING
      } else {

        handle_finish_tx_phase1_commit();
      }
    }
  } else {

    LOG(trace) << xid_ << " abort , " << enum2str(error_code_);
    if (distributed_) {
#ifdef DB_TYPE_SHARE_NOTHING
      if (is_shared_nothing()) {
        handle_finish_tx_phase1_prepare_abort();
      }
#endif // DB_TYPE_SHARE_NOTHING
    } else {
      handle_finish_tx_phase1_abort();
    }
  }
}

void tx_context::handle_operation(tx_operation &op, const fn_ec op_done) {
#ifdef TX_TRACE
  trace_message_ << "h op;";
#endif

  switch (op.op_type()) {
  case TX_OP_READ:
  case TX_OP_READ_FOR_WRITE: {
    table_id_t table_id = op.tuple_row().table_id();
    shard_id_t shard_id = op.tuple_row().shard_id();
    tuple_id_t key = op.tuple_row().tuple_id();
    auto s = shared_from_this();
    auto read_done = [s, table_id, shard_id, key, op_done](EC ec, tuple_pb &&tuple) {
      if (ec == EC::EC_NOT_FOUND_ERROR) {
        tuple_id_t tid = (key);

        LOG(trace) << s->node_name_ << " cannot find, table_id=" << table_id
                   << ", tuple_id=" << tid;
      }
      BOOST_ASSERT(not(ec == EC::EC_OK && is_tuple_nil(tuple)));
      tx_operation *op_response = s->response_.add_operations();
      tuple.swap(*op_response->mutable_tuple_row()->mutable_tuple());
      LOG(trace) << s->node_name_ << " handle read table " << table_id << "  ";
      s->invoke_done(op_done, ec);
    };
    bool read_for_write = op.op_type() == TX_OP_READ_FOR_WRITE;
    async_read(table_id, shard_id, key, read_for_write, read_done);
    return;
  }
  case TX_OP_UPDATE: {
    table_id_t table_id = op.tuple_row().table_id();
    tuple_id_t key = op.tuple_row().tuple_id();
    shard_id_t shard_id = op.tuple_row().shard_id();
    const tuple_pb tuple = op.tuple_row().tuple();
    auto s = shared_from_this();
    auto update_done = [s, op, table_id, shard_id, key, tuple, op_done](EC ec) {
      if (ec == EC::EC_NOT_FOUND_ERROR) {
        LOG(debug) << s->node_name_ << " cannot find, table_id=" << table_id
                   << ", tuple_id=" << (key);
      }
      // LOG(debug)
      //   << "handle update table " << table_id << " tuple: ";
      s->append_operation(op);
      s->invoke_done(op_done, ec);
    };
    tuple_pb tp;
    tp.swap(*op.mutable_tuple_row()->mutable_tuple());
    async_update(table_id, shard_id, key, std::move(tp), update_done);
    return;
  }

  case TX_OP_INSERT: {
    table_id_t table_id = op.tuple_row().table_id();
    tuple_id_t key = op.tuple_row().tuple_id();
    shard_id_t shard_id = op.tuple_row().shard_id();
    const tuple_pb tuple = op.tuple_row().tuple();
    auto s = shared_from_this();
    auto insert_done = [s, op, table_id, shard_id, key, tuple, op_done](EC ec) {
      if (ec == EC::EC_DUPLICATION_ERROR) {
        LOG(debug) << s->node_name_ << " find, table_id=" << table_id
                   << ", tuple=" << (key);
      }
      // LOG(debug)
      //   << "handle insert table " << table_id << " tuple ";
      s->append_operation(op);
      s->invoke_done(op_done, ec);
    };
    tuple_pb tp;
    tp.swap(*op.mutable_tuple_row()->mutable_tuple());
    async_insert(table_id, shard_id, key, std::move(tp), insert_done);
    return;
  }
  default:BOOST_ASSERT(false);
  }
}

void tx_context::invoke_done(fn_ec op_done, EC ec) {
  // dispatch is ok
  // no lock
  boost::asio::post(get_strand(), [ec, op_done]() {
    scoped_time _t("tx_context op_done");
    op_done(ec);
  });
  // op_done(ec);
}

void tx_context::send_tx_response() {
  if (has_respond_) {
    return;
  }
  trace_message_ << "RESP;";
  has_respond_ = true;
  LOG(trace) << node_name_ << " tx " << xid_ << " send response: " << enum2str(error_code_);

  part_time_tracer_.end();

  BOOST_ASSERT(cli_conn_);
  auto response = std::make_shared<tx_response>();
  response->set_error_code(uint32_t(error_code_));

  response->set_latency_append(append_time_tracer_.microseconds());
  response->set_latency_read_dsb(latency_read_dsb_);
  response->set_latency_read(read_time_tracer_.microseconds());
  response->set_latency_lock_wait(lock_wait_time_tracer_.microseconds());
  response->set_latency_replicate(log_rep_delay_);
  response->set_latency_part(part_time_tracer_.microseconds());
  response->set_access_part(1);
  response->set_num_lock(num_lock_);
  response->set_num_read_violate(num_read_violate_);
  response->set_num_write_violate(num_write_violate_);
  if (response->latency_read_dsb() > response->latency_read()) {
    LOG(error) << "read DSB" << response->latency_read_dsb() << "ms";
    LOG(error) << "read" << response->latency_read() << "ms";
    BOOST_ASSERT(false);
  }

  service_->conn_async_send(cli_conn_, CLIENT_TX_RESP, response);
}

void tx_context::abort_tx_1p() {
#ifdef TX_TRACE
  trace_message_ << "a1p;";
#endif
  if (state_ == rm_state::RM_IDLE) {
    state_ = rm_state::RM_ABORTING;
    set_tx_cmd_type(TX_CMD_RM_ABORT);
    LOG(trace) << node_name_ << " transaction RM " << xid_ << "phase1 aborted";
    async_force_log();
  } else if (state_ == rm_state::RM_ABORTING) {
    send_tx_response();
  } else {
    BOOST_ASSERT(false);
  }
}

void tx_context::on_committed_log_commit() {
  commit_log_synced_ = true;

#ifdef DB_TYPE_NON_DETERMINISTIC
  tx_committed();
#endif
}

void tx_context::on_log_entry_commit(
    tx_cmd_type type, std::chrono::steady_clock::time_point end_ts) {

#ifdef TX_TRACE
  trace_message_ << "lg cmt " << type << ";";
#endif
  log_entry_.clear();
  switch (type) {
  case TX_CMD_RM_COMMIT: {
    append_time_tracer_.end_ts(end_ts);
    on_committed_log_commit();
    break;
  }
  case TX_CMD_RM_ABORT: {
    on_aborted_log_commit();
    break;
  }
#ifdef DB_TYPE_SHARE_NOTHING
  case TX_CMD_RM_PREPARE_ABORT: {
    on_prepare_aborted_log_commit();
    break;
  }
  case TX_CMD_RM_PREPARE_COMMIT: {
    append_time_tracer_.end_ts(end_ts);
    on_prepare_committed_log_commit();
    break;
  }
#endif // DB_TYPE_SHARE_NOTHING
  default:break;
  }
}

void tx_context::on_aborted_log_commit() { tx_aborted(); }

void tx_context::tx_committed() {
  if (!distributed_) {
#ifdef TX_TRACE
    trace_message_ << "tx_rm C;";
#endif

    LOG(trace) << "tx_rm: " << xid_ << ", commit ";
    send_tx_response();
    release_lock();
  } else {
#ifdef DB_TYPE_SHARE_NOTHING
    if (is_shared_nothing()) {

#ifdef TX_TRACE
      trace_message_ << "tx_rm C;";
#endif
      LOG(trace) << "tx_rm TM : " << xid_ << ", phase 2 commit: ";
      send_ack_message(true);
      release_lock();
    }
#endif // DB_TYPE_SHARE_NOTHING
  }
}

void tx_context::tx_aborted() {
  if (!distributed_) {
#ifdef TX_TRACE
    trace_message_ << "tx_rm A;";
#endif

    LOG(trace) << "tx_rm RM : " << xid_ << ", phase 1 abort: ";
    if (error_code_ == EC::EC_OK) {
      error_code_ = EC::EC_TX_ABORT;
    }
    send_tx_response();
    release_lock();
  } else {
#ifdef DB_TYPE_SHARE_NOTHING
    if (is_shared_nothing()) {

      LOG(trace) << "tx_rm TM : " << xid_ << ", phase 2 abort: ";
      send_ack_message(false);
      release_lock();
    }
#endif // DB_TYPE_SHARE_NOTHING
  }

}

rm_state tx_context::state() const { return state_; }

void tx_context::tx_ended() {
  state_ = RM_ENDED;
  LOG(trace) << node_name_ << " xid " << xid_ << " end";
  if (fn_tx_state_) {
    fn_tx_state_(xid_, state_);
  }
}

void tx_context::release_lock() {
  set_end();
#ifdef TX_TRACE
  trace_message_ << "rl;";
#endif
  if (!read_only_) {
    for (const auto &kv : locks_) {
      ptr<lock_item> l = kv.second;
      mgr_->unlock(l->xid(), l->type(), l->table_id(), l->shard_id(), l->get_predicate());
    }
  }

  if (dl_) {
    dl_->tx_finish(xid_);
  }
  tx_ended();
  locks_.clear();
}

void tx_context::async_force_log() {
#ifdef TX_TRACE
  trace_message_ << "fc lg;";
#endif
  LOG(trace) << node_name_ << " xid:" << xid_ << ", force log";
  std::vector<tx_log_binary> entries;
  for (tx_log_proto &log : log_entry_) {
    tx_log_binary log_binary = tx_log_proto_to_binary(log);
    entries.emplace_back(log_binary);
  }
  wal_->async_append(entries);
  log_entry_.clear();
  append_time_tracer_.begin();
}

void tx_context::append_operation(const tx_operation &op) {
  if (op.op_type() == tx_op_type::TX_OP_INSERT ||
      op.op_type() == tx_op_type::TX_OP_UPDATE) {
    BOOST_ASSERT(op.has_tuple_row() && !is_tuple_nil(op.tuple_row().tuple()));
  }
  if (log_entry_.empty()) {
    log_entry_.emplace_back();
  }

  tx_operation *o = log_entry_.rbegin()->add_operations();
  *o = op;
  o->set_xid(xid_);
  o->set_sd_id(TO_RG_ID(node_id_));
}

void tx_context::set_tx_cmd_type(tx_cmd_type type) {
  if (log_entry_.empty()) {
    log_entry_.emplace_back();
  }
  BOOST_ASSERT(not log_entry_.empty());
  tx_log_proto &log = *log_entry_.rbegin();
  log.set_xid(xid_);
  log.set_log_type(type);
}

void tx_context::handle_finish_tx_phase1_commit() {
#ifdef TX_TRACE
  trace_message_ << "c1p;";
#endif
  if (state_ == rm_state::RM_IDLE || state_ == rm_state::RM_PREPARE_COMMITTING) {
    state_ = rm_state::RM_COMMITTING;

    set_tx_cmd_type(TX_CMD_RM_COMMIT);

    LOG(trace) << node_name_ << " transaction RM " << xid_ << " commit";
    if (read_only_) {
      on_committed_log_commit();
    } else {
      async_force_log();
    }
  } else if (state_ == rm_state::RM_COMMITTING) {
    send_tx_response();
  } else {
    BOOST_ASSERT(false);
  }
}

void tx_context::handle_finish_tx_phase1_abort() { abort_tx_1p(); }

void tx_context::abort(EC ec) {
  if (ec == EC_VICTIM) {
    if (not victim_ && not distributed_) {
      victim_ = true;
#ifdef TX_TRACE
      trace_message_ << "victim;";
#endif
    }
  }
  if (not distributed_) {
    if (state_ == RM_IDLE) {
      error_code_ = ec;
      abort_tx_1p();
    }
  } else {
    ptr<tx_victim> msg = cs_new<tx_victim>();
    msg->set_xid(xid_);
    msg->set_source(node_id_);
    msg->set_dest(coord_node_id_);
    result<void> r = service_->async_send(coord_node_id_, TX_VICTIM, msg, true);
    if (!r) {
      LOG(error) << "async send tx victim error" << xid_;
    }
  }
}

#ifdef DB_TYPE_SHARE_NOTHING

void tx_context::on_prepare_committed_log_commit() {
  prepare_commit_log_synced_ = true;

#ifdef DB_TYPE_NON_DETERMINISTIC
  tx_prepare_committed();
#endif
}

void tx_context::on_prepare_aborted_log_commit() { tx_prepare_aborted(); }

void tx_context::tx_prepare_committed() {
#ifdef TX_TRACE
  trace_message_ << "tx_rm PC;";
#endif
  LOG(trace) << node_name_ << " tx_rm: " << xid_ << ", prepare commit: ";

  send_prepare_message(true);
}

void tx_context::tx_prepare_aborted() {
#ifdef TX_TRACE
  trace_message_ << "tx_rm PA;";
#endif
  LOG(trace) << "tx_rm: " << xid_ << ", prepare abort: ";
  send_prepare_message(false);
}

void tx_context::abort_tx_2p() {

  if (state_ == rm_state::RM_IDLE || state_ == rm_state::RM_PREPARE_ABORTING ||
      state_ == rm_state::RM_PREPARE_COMMITTING) {
    state_ = rm_state::RM_ABORTING;
#ifdef TX_TRACE
    trace_message_ << "a2p;";
#endif
    set_tx_cmd_type(TX_CMD_RM_ABORT);
    LOG(trace) << node_name_ << " transaction RM " << xid_ << "phase2 aborted";
    async_force_log();
  } else if (state_ == RM_ABORTING || state_ == RM_ENDED) {
    send_ack_message(false);
  } else {
    BOOST_ASSERT_MSG(false, "error state");
  }
}

void tx_context::handle_finish_tx_phase1_prepare_commit() {
#ifdef TX_TRACE
  trace_message_ << "pc1p;";
#endif
  prepare_commit_tx();
  auto s = shared_from_this();
  async_force_log();
}

void tx_context::handle_finish_tx_phase1_prepare_abort() {
#ifdef TX_TRACE
  trace_message_ << "pa1p;";
#endif
  prepare_abort_tx();
  async_force_log();
}

void tx_context::prepare_commit_tx() {

  if (state_ == rm_state::RM_IDLE) {
    state_ = rm_state::RM_PREPARE_COMMITTING;
    tx_operation prepare_commit_op;

    set_tx_cmd_type(TX_CMD_RM_PREPARE_COMMIT);
    LOG(trace) << node_name_ << " transaction RM " << xid_ << " prepare commit";
  }
}

void tx_context::prepare_abort_tx() {

  state_ = rm_state::RM_PREPARE_ABORTING;
  tx_operation prepare_commit_op;

  set_tx_cmd_type(TX_CMD_RM_PREPARE_ABORT);
  LOG(trace) << node_name_ << " transaction RM " << xid_ << " prepare commit";
}

void tx_context::send_prepare_message(bool commit) {
  part_time_tracer_.end();
  auto msg = std::make_shared<tx_rm_prepare>();
  msg->set_xid(xid_);
  msg->set_source_node(node_id_);
  msg->set_source_rg(TO_RG_ID(node_id_));
  msg->set_dest_node(coord_node_id_);
  msg->set_dest_rg(TO_RG_ID(coord_node_id_));
  msg->set_commit(commit);
  if (commit) {
    msg->set_latency_append(append_time_tracer_.microseconds());
    msg->set_latency_read(read_time_tracer_.microseconds());
    msg->set_latency_lock_wait(lock_wait_time_tracer_.microseconds());
    msg->set_latency_replicate(log_rep_delay_);
    msg->set_latency_part(part_time_tracer_.microseconds());
    msg->set_num_write_violate(num_write_violate_);
    msg->set_num_read_violate(num_read_violate_);
    msg->set_num_lock(num_lock_);
  }

  result<void> r = service_->async_send(coord_node_id_, TX_RM_PREPARE, msg, true);
  if (!r) {
    LOG(error) << "async send Prepare error" << xid_;
  }
}

void tx_context::send_ack_message(bool commit) {
  auto msg = std::make_shared<tx_rm_ack>();
  msg->set_xid(xid_);
  msg->set_source_node(node_id_);
  msg->set_source_rg(TO_RG_ID(node_id_));
  msg->set_dest_node(coord_node_id_);
  msg->set_dest_rg(TO_RG_ID(coord_node_id_));
  msg->set_commit(commit);
  result<void> r = service_->async_send(coord_node_id_, TX_RM_ACK, msg, true);
  if (!r) {
    LOG(error) << "async send ACK error";
  }
}

void tx_context::handle_tx_tm_commit(const tx_tm_commit &msg) {
  BOOST_ASSERT(msg.xid() == xid_);
  if (msg.xid() != xid_) {
    return;
  }
  handle_finish_tx_phase2_commit();
}
void tx_context::handle_tx_tm_abort(const tx_tm_abort &msg) {
  BOOST_ASSERT(msg.xid() == xid_);
  if (msg.xid() != xid_) {
    return;
  }
  handle_finish_tx_phase2_abort();
}

void tx_context::handle_finish_tx_phase2_commit() {

#ifdef TX_TRACE
  trace_message_ << "c2p;";
#endif
  if (state_ == rm_state::RM_PREPARE_COMMITTING) {
    state_ = rm_state::RM_COMMITTING;
    set_tx_cmd_type(TX_CMD_RM_COMMIT);
    LOG(trace) << node_name_ << " transaction RM " << xid_ << " commit";
    async_force_log();
  } else if (state_ == rm_state::RM_COMMITTING) {
    send_ack_message(true);
  } else {
    BOOST_ASSERT(false);
  }
}

void tx_context::handle_finish_tx_phase2_abort() { abort_tx_2p(); }


#ifdef DB_TYPE_GEO_REP_OPTIMIZE

void tx_context::register_dependency(const ptr<tx_context> &out) {
  if (xid_ == out->xid_) {
    BOOST_LOG_TRIVIAL(error) << "cannot register the same transaction";
    return;
  }
  if (xid_ < out->xid_) {
    mutex_.lock();
    out->mutex_.lock();
  } else {
    out->mutex_.lock();
    mutex_.lock();
  }
  do {
    if (out->state_ == RM_ABORTING || out->state_ == RM_COMMITTING) {
      break;
    }
    if (state_ == RM_COMMITTING || state_ == RM_ABORTING) {
      break;
    } else {
      auto i = dep_out_set_.find(out->xid_);
      if (i == dep_out_set_.end()) {
        out->dep_in_count_++;
        dep_out_set_[out->xid_] = out;
        out->dep_in_set_[xid_] = shared_from_this();
      }
    }
  } while (false);

  if (xid_ < out->xid_) {
    out->mutex_.lock();
    mutex_.lock();
  } else {
    mutex_.lock();
    out->mutex_.lock();
  }
}

void tx_context::report_dependency() {
  mutex_.lock();
  for (const auto &kv : dep_out_set_) {
    ptr<tx_context> ctx = kv.second;
    xid_t xid = xid_;
    auto fn = [ctx, xid]() {
      ctx->mutex_.lock();
      auto i = ctx->dep_in_set_.find(xid);
      if (i != ctx->dep_in_set_.end()) {
        if (ctx->dep_in_count_ > 0) {
          ctx->dep_in_count_--;
          if (ctx->dep_in_count_ == 0) {
            auto fn = [ctx]() {
              ctx->dependency_commit();
            };
            boost::asio::post(ctx->get_strand(), fn);
          }
        }
      }
      ctx->mutex_.unlock();
    };
    boost::asio::post(ctx->get_strand(), fn);
  }
  mutex_.unlock();
}

void tx_context::dependency_commit() {
  std::scoped_lock l(mutex_);
  dependency_committed_ = true;
  if (distributed_) {
    dlv_try_tx_prepare_commit();
  } else {
    dlv_try_tx_commit();
  }
}

void tx_context::dlv_try_tx_commit() {
#ifdef TX_TRACE
  trace_message_ += "dlv try C;";
#endif
  if (dep_in_count_ == 0 && commit_log_synced_ && not dlv_commit_) {
    dlv_commit_ = true;
    tx_committed();
  }
}

void tx_context::dlv_try_tx_prepare_commit() {
#ifdef TX_TRACE
  trace_message_ += "dlv try PC;";
#endif
  if (dep_in_count_ == 0 && prepare_commit_log_synced_ && not dlv_prepare_) {
    dlv_prepare_ = true;
    tx_prepare_committed();
  }
}

void tx_context::dlv_abort() {
  if (is_geo_rep_optimized()) {
#ifdef TX_TRACE
    trace_message_ += "dlv A;";
#endif
    for (const auto &kv : dep_out_set_) {
      kv.second->dlv_abort();
    }
    if (dep_in_count_ > 0) {
      error_code_ = EC::EC_CASCADE;
    }
  }
}

void tx_context::dlv_make_violable() {
#ifdef TX_TRACE
  trace_message_ += "dlv V;";
#endif
  for (const auto &l : locks_) {
    violate v;
    mgr_->make_violable(l.second->xid(),
                        l.second->type(),
                        l.second->table_id(),
                        l.second->key(), v);
    num_read_violate_ += v.read_v_;
    num_write_violate_ += v.write_v_;
  }
}

void tx_context::handle_tx_enable_violate() {
  dlv_make_violable();
}

void tx_context::send_tx_enable_violate() {
  auto msg = std::make_shared<tx_enable_violate>();
  msg->set_source(node_id_);
  msg->set_dest(coord_node_id_);
  msg->set_violable(true);
  auto r = service_->async_send(msg->dest(), RM_ENABLE_VIOLATE, msg);
  if (!r) {
    BOOST_LOG_TRIVIAL(error) << "report RM enable violate " << msg->violable();
  }
}
#endif // DB_TYPE_GEO_REP_OPTIMIZE
#endif // DB_TYPE_SHARE_NOTHING

void tx_context::timeout_clean_up() {
  auto ms = steady_clock_ms_since_epoch();

  if (ms + 1000 < start_) {
    return;
  }

  std::scoped_lock l(mutex_);
  if (state_ == rm_state::RM_PREPARE_COMMITTING ||
      state_ == rm_state::RM_COMMITTING) {
    return;
  } else {
    if (!distributed_) {
      abort_tx_1p();
      send_tx_response();

    } else {
#ifdef DB_TYPE_SHARE_NOTHING
      if (is_shared_nothing()) {
        abort_tx_2p();
      }
#endif // DB_TYPE_SHARE_NOTHING
    }
  }
}

void tx_context::debug_tx(std::ostream &os) {
  os << node_name_ << " RM : " << xid_ << " state: " << enum2str(state_)
     << std::endl;

  auto ms = steady_clock_ms_since_epoch();
  os << "    -> after begin: " << ms - start_ << "ms";
  os << "    -> trace: " << trace_message_.str() << std::endl;
}

void tx_context::log_rep_delay(uint64_t us) { log_rep_delay_ += us; }

node_id_t tx_context::shard2node(shard_id_t shard_id) {
  return node_id_of_shard(shard_id, ctx_opt_dsb_node_id_, shard_id_2_node_id_);
}

#endif // #ifdef DB_TYPE_NON_DETERMINISTIC