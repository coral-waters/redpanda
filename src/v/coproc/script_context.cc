/*
 * Copyright 2020 Vectorized, Inc.
 *
 * Use of this software is governed by the Business Source License
 * included in the file licenses/BSL.md
 *
 * As of the Change Date specified in that file, in accordance with
 * the Business Source License, use of this software will be governed
 * by the Apache License, Version 2.0
 */

#include "coproc/script_context.h"

#include "coproc/logger.h"
#include "coproc/reference_window_consumer.hpp"
#include "coproc/script_context_backend.h"
#include "coproc/script_context_frontend.h"
#include "coproc/types.h"
#include "model/record_batch_reader.h"
#include "model/timeout_clock.h"
#include "storage/api.h"
#include "storage/parser_utils.h"
#include "storage/types.h"
#include "vlog.h"

#include <seastar/core/coroutine.hh>
#include <seastar/core/sleep.hh>

#include <chrono>
#include <exception>

namespace coproc {

script_context::script_context(
  script_id id, shared_script_resources& resources, routes_t&& routes) noexcept
  : _resources(resources)
  , _routes(std::move(routes))
  , _id(id) {
    vassert(
      !_routes.empty(),
      "Unallowed to create an instance of script_context without having a "
      "valid subscription list");
}

ss::future<> script_context::start() {
    vassert(_gate.get_count() == 0, "Cannot call start() twice");
    return ss::with_gate(_gate, [this] {
        return ss::do_until(
          [this] { return _abort_source.abort_requested(); },
          [this] {
              /// do_execute is by design expected to throw one type of
              /// exception, \ref script_failed_exception for which there is a
              /// handler setup by the invoker of this start() method
              return do_execute().then([this] {
                  return ss::sleep_abortable(
                           _resources.jitter.next_jitter_duration(),
                           _abort_source)
                    .handle_exception_type([](const ss::sleep_aborted&) {});
              });
          });
    });
}

ss::future<> script_context::do_execute() {
    /// This loop executes while there is data to read from the input logs and
    /// while there is a current successful connection to the wasm engine.
    /// If both of those conditions aren't met, the loop breaks, hitting the
    /// sleep_abortable() call in the fiber started by 'start()'
    return ss::repeat([this] {
        if (_abort_source.abort_requested()) {
            return ss::make_ready_future<ss::stop_iteration>(
              ss::stop_iteration::yes);
        }
        notify_waiters();
        return _resources.transport.get_connected(model::no_timeout)
          .then([this](result<rpc::transport*> transport) {
              if (!transport) {
                  /// Failed to connected to the wasm engine for whatever
                  /// reason, exit to yield
                  return ss::make_ready_future<ss::stop_iteration>(
                    ss::stop_iteration::yes);
              }
              supervisor_client_protocol client(*transport.value());
              input_read_args args{
                .id = _id,
                .read_sem = _resources.read_sem,
                .abort_src = _abort_source,
                .inputs = _routes};
              return read_from_inputs(args).then(
                [this, client = std::move(client)](
                  std::vector<process_batch_request::data> requests) mutable {
                    if (requests.empty()) {
                        /// No data to read from all inputs, no need to
                        /// incessently loop, can exit to yield
                        return ss::make_ready_future<ss::stop_iteration>(
                          ss::stop_iteration::yes);
                    }
                    /// Send request to wasm engine
                    process_batch_request req{.reqs = std::move(requests)};
                    return send_request(std::move(client), std::move(req))
                      .then([] { return ss::stop_iteration::no; });
                });
          });
    });
}

void script_context::notify_waiters() {
    auto updates = std::exchange(_updates, {});
    for (auto& [ntp, update] : updates) {
        auto found = _routes.find(update.source);
        if (found != _routes.end()) {
            found->second->wctx.offsets.erase(ntp);
        }
        for (auto& p : update.ps) {
            p.set_value();
        }
    }
}

ss::future<> script_context::remove_output(
  const model::ntp& source, const model::ntp& materialized) {
    auto found = _updates.find(materialized);
    if (found == _updates.end()) {
        found = _updates.emplace(materialized, update{.source = source}).first;
    }
    vassert(found->second.source == source, "Mismatched");
    found->second.ps.emplace_back();
    return found->second.ps.back().get_future();
}

ss::future<> script_context::shutdown() {
    _abort_source.request_abort();
    co_await _gate.close();
    auto updates = std::exchange(_updates, {});
    for (auto& [ntp, update] : updates) {
        for (auto& p : update.ps) {
            p.set_exception(wait_future_stranded(
              ssx::sformat("Failed to fufill event for partition: {}", ntp)));
        }
    }
}

ss::future<> script_context::send_request(
  supervisor_client_protocol client, process_batch_request r) {
    using reply_t = result<rpc::client_context<process_batch_reply>>;
    return client
      .process_batch(
        std::move(r), rpc::client_opts(rpc::clock_type::now() + 5s))
      .then([this](reply_t reply) {
          if (reply) {
              output_write_args args{
                .id = _id,
                .metadata = _resources.rs.metadata_cache,
                .frontend = _resources.rs.mt_frontend,
                .pm = _resources.rs.cp_partition_manager,
                .inputs = _routes,
                .denylist = _resources.in_progress_deletes,
                .locks = _resources.log_mtx};
              return write_materialized(
                std::move(reply.value().data.resps), args);
          }
          vlog(
            coproclog.warn,
            "Error upon attempting to perform RPC to wasm engine, code: {}",
            reply.error());
          return ss::now();
      });
}

bool script_context::is_up_to_date() const {
    for (const auto& [_, src] : _routes) {
        const auto highest = src->rctx.input->last_stable_offset();
        for (const auto& [_, o] : src->wctx.offsets) {
            if (o < highest) {
                return false;
            }
        }
    }
    return true;
}

} // namespace coproc
