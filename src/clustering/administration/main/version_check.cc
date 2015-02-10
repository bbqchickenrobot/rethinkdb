// Copyright 2010-2014 RethinkDB, all rights reserved.
#include "clustering/administration/main/version_check.hpp"

#include <math.h>

#include "logger.hpp"

#include "clustering/administration/metadata.hpp"
#include "extproc/http_runner.hpp"
#include "rdb_protocol/env.hpp"

namespace ql {
void dispatch_http(ql::env_t *env,
                   const http_opts_t &opts,
                   http_runner_t *runner,
                   http_result_t *res_out,
                   const ql::pb_rcheckable_t *parent);
};

static const int64_t day_in_ms = 24 * 60 * 60 * 1000;

version_checker_t::version_checker_t(rdb_context_t *_rdb_ctx,
                                     version_checker_t::metadata_ptr_t _metadata,
                                     const std::string &_uname) :
    rdb_ctx(_rdb_ctx),
    seen_version(),
    metadata(_metadata),
    uname(_uname),
    timer(day_in_ms, this) {
    rassert(rdb_ctx != NULL);
    coro_t::spawn_sometime(std::bind(&version_checker_t::do_check,
                                     this, true, drainer.lock()));
}

void version_checker_t::do_check(bool is_initial, auto_drainer_t::lock_t keepalive) {
    const cluster_semilattice_metadata_t snapshot = metadata->get();
    ql::env_t env(rdb_ctx,
                  ql::return_empty_normal_batches_t::NO,
                  keepalive.get_drain_signal(),
                  std::map<std::string, ql::wire_func_t>(),
                  nullptr);
    http_opts_t opts;
    opts.limits = env.limits();
    opts.result_format = http_result_format_t::JSON;
    if (is_initial) {
        opts.url = strprintf("http://update.rethinkdb.com/update_for/%s",
                             RETHINKDB_VERSION);
    } else {
        opts.method = http_method_t::POST;
        opts.url = "http://update.rethinkdb.com/checkin";
        opts.header.push_back("Content-Type: application/x-www-form-urlencoded");
        opts.form_data["Version"] = RETHINKDB_VERSION;
        opts.form_data["Number-Of-Servers"]
            = strprintf("%zu", snapshot.servers.servers.size());
        opts.form_data["Uname"] = uname;
        opts.form_data["Cooked-Number-Of-Tables"]
            = strprintf("%" PR_RECONSTRUCTABLE_DOUBLE,
                        cook(snapshot.rdb_namespaces->namespaces.size()));
        //opts.form_data["Cooked-Size-Of-Shards"]
        //    = strprintf("%" PR_RECONSTRUCTABLE_DOUBLE, cook(0.0)); // XXX
    }

    http_runner_t runner(env.get_extproc_pool());
    http_result_t result;

    try {
        dispatch_http(&env, opts, &runner, &result, nullptr);
    } catch (const ql::base_exc_t &ex) {
        logWRN("Problem when checking for new versions of RethinkDB: HTTP request to "
            "update.rethinkdb.com failed.");
        logDBG("%s", ex.what());
        return;
    }

    try {
        process_result(result);
    } catch (const ql::base_exc_t &ex) {
        logWRN("Problem when checking for new versions of RethinkDB: "
            "update.rethinkdb.com returned an invalid result.");
        logDBG("Result body: %s\nHeaders: %s\nError message: %s",
            result.body.trunc_print().c_str(), result.header.trunc_print().c_str(),
            ex.what());
    }
}

// sort of anonymize the input; specifically we want $2^(round(log_2(n)))$
double version_checker_t::cook(double n) {
    return exp2(round(log2(n)));
}

void version_checker_t::process_result(const http_result_t &result) {
    rcheck_datum(result.error.empty(), ql::base_exc_t::GENERIC,
        strprintf("protocol error: %s", result.error.c_str()));
    rcheck_datum(result.body.has(), ql::base_exc_t::GENERIC,
        "no body returned");

    ql::datum_t status = result.body.get_field("status", ql::THROW);
    const datum_string_t &str = status.as_str();
    if (str == "ok") {
        logDBG("Finished checking for newer versions of RethinkDB. We are already "
            "running the most up-to-date version.");
    } else if (str == "error") {
        ql::datum_t reason = result.body.get_field("error", ql::THROW);
        rfail_datum(ql::base_exc_t::GENERIC,
            "update server reports error: %s", reason.as_str().to_std().c_str());
    } else if (str == "need_update") {
        ql::datum_t new_version_datum = result.body.get_field("last_version", ql::THROW);
        datum_string_t new_version = new_version_datum.as_str();
        if (seen_version != new_version) {
            ql::datum_t changelog = result.body.get_field("link_changelog", ql::NOTHROW);
            std::string changelog_msg;
            if (changelog.has() && changelog.get_type() == ql::datum_t::R_STR) {
                changelog_msg = strprintf(" You can read the changelog at <%s>.",
                    changelog.as_str().to_std().c_str());
            }
            logNTC("A newer version of the RethinkDB server is available: %s.%s",
                new_version.to_std().c_str(), changelog_msg.c_str());
            seen_version = new_version;
        } else {
            // already logged an update for that version, so no point
            // in spamming them.
            logDBG("Finished checking for newer versions of RethinkDB. No new versions "
                "are available since we last checked.");
        }
    } else {
        rfail_datum(ql::base_exc_t::GENERIC, "unexpected status code");
    }
}
