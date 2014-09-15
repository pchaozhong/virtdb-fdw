#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wold-style-cast"

#include "expression.hh"
#include "query.hh"
#include "receiver_thread.hh"
#include "data_handler.hh"
#include "virtdb_fdw.h" // pulls in some postgres headers
#include "postgres_util.hh"

// ZeroMQ
#include "cppzmq/zmq.hpp"

// more postgres headers
extern "C" {
    #include <utils/memutils.h>
    #include <foreign/fdwapi.h>
    #include <foreign/foreign.h>
    #include <utils/rel.h>
    #include <utils/builtins.h>
    #include <utils/date.h>
    #include <utils/syscache.h>
    #include <optimizer/pathnode.h>
    #include <optimizer/planmain.h>
    #include <optimizer/restrictinfo.h>
    #include <optimizer/clauses.h>
    #include <catalog/pg_type.h>
    #include <catalog/pg_operator.h>
    #include <catalog/pg_foreign_data_wrapper.h>
    #include <catalog/pg_foreign_table.h>
    #include <access/transam.h>
    #include <access/htup_details.h>
    #include <access/reloptions.h>
    #include <funcapi.h>
    #include <nodes/print.h>
    #include <nodes/makefuncs.h>
    #include <miscadmin.h>
    #include <commands/defrem.h>
}

#include "filter.hh"
#include "anyallfilter.hh"
#include "boolexprfilter.hh"
#include "defaultfilter.hh"
#include "nulltestfilter.hh"
#include "opexprfilter.hh"

#include <logger.hh>
#include <util.hh>
#include <connector.hh>

// standard headers
#include <atomic>
#include <exception>
#include <sstream>
#include <string.h>
#include <ctype.h>
#include <stdlib.h>

#include <memory>
#include <future>

using namespace virtdb;
using namespace virtdb::connector;

zmq::context_t* zmq_context = new zmq::context_t(1);

endpoint_client* ep_client;
log_record_client* log_client;

namespace virtdb_fdw_priv {

struct provider {
    std::string name = "";
    receiver_thread* worker_thread = nullptr;
    std::string query_address;
    std::string data_address;
};

provider* current_provider;
std::map<std::string, provider> providers;

// We dont't do anything here right now, it is intended only for optimizations.
static void
cbGetForeignRelSize( PlannerInfo *root,
                     RelOptInfo *baserel,
                     Oid foreigntableid )
{
    try
    {
        auto table = GetForeignTable(foreigntableid);
        ListCell *cell;
        foreach(cell, table->options)
        {
            DefElem *def = (DefElem *) lfirst(cell);
            std::string option_name = def->defname;
            if (option_name == "provider")
            {
                current_provider = &providers[defGetString(def)];
            }
        }

        if (current_provider && current_provider->worker_thread == nullptr)
        {
            current_provider->worker_thread = new receiver_thread(zmq_context);
            auto thread = new std::thread(&receiver_thread::run, current_provider->worker_thread);
            thread->detach();
        }

        if (ep_client == nullptr || log_client == nullptr)
        {
            std::string config_server_url = "";
            auto server = GetForeignServer(table->serverid);
            auto fdw = GetForeignDataWrapper(server->fdwid);
            foreach(cell, fdw->options)
            {
                DefElem *def = (DefElem *) lfirst(cell);
                std::string option_name = def->defname;
                if (option_name == "url")
                {
                    config_server_url = defGetString(def);
                }
            }
            elog(LOG, "Config server url: %s", config_server_url.c_str());
            if (config_server_url != "")
            {
                ep_client = new endpoint_client(config_server_url, "generic_fdw");
                log_client = new log_record_client(*ep_client, "diag-service");

                LOG_TRACE("ForeignDataWrapper is being initialized.");
                ep_client->watch(virtdb::interface::pb::ServiceType::QUERY,
                                [](const virtdb::interface::pb::EndpointData & ep) {
                                    LOG_TRACE("Endpoint watch got QUERY endpoint with name" << V_(ep.name()));
                                    for (auto connection : ep.connections())
                                    {
                                        for (auto address : connection.address())
                                        {
                                            LOG_TRACE("Address to QUERY endpoint"<<V_(ep.name()) << V_(address));
                                            providers[ep.name()].worker_thread->set_query_url(address);
                                        }
                                    }
                                    return true;
                                });

                ep_client->watch(virtdb::interface::pb::ServiceType::COLUMN,
                                [](const virtdb::interface::pb::EndpointData & ep) {
                                    LOG_TRACE("Endpoint watch got COLUMN endpoint with name" << V_(ep.name()));
                                    for (auto connection : ep.connections())
                                    {
                                        for (auto address : connection.address())
                                        {
                                            LOG_TRACE("Address to COLUMN endpoint"<<V_(ep.name()) << V_(address));
                                            providers[ep.name()].worker_thread->set_data_url(address);
                                        }
                                    }
                                    return true;
                                });

            }
        }
    }
    catch(const std::exception & e)
    {
        elog(ERROR, "[%s:%d] internal error in %s: %s",__FILE__,__LINE__,__func__,e.what());
    }

}

// We also don't intend to put this to the public API for now so this
// default implementation is enough.
static void
cbGetForeignPaths( PlannerInfo *root,
                    RelOptInfo *baserel,
                    Oid foreigntableid)
{
    Cost startup_cost = 0;
    Cost total_cost = startup_cost + baserel->rows * baserel->width;

    add_path(baserel,
             reinterpret_cast<Path *>(create_foreignscan_path(
                 root,
                 baserel,
                 baserel->rows,
                 startup_cost,
                 total_cost,
                 // no pathkeys:  TODO! check-this!
                 NIL,
                 NULL,
                 // no outer rel either:  TODO! check-this!
                 NIL
            )));
}

// Maybe in a later version we could provide API for extracting clauses
// but this is good enough for now to just leave in all of them.
static ForeignScan
*cbGetForeignPlan( PlannerInfo *root,
                   RelOptInfo *baserel,
                   Oid foreigntableid,
                   ForeignPath *best_path,
                   List *tlist,
                   List *scan_clauses )
{
    Index scan_relid = baserel->relid;
    if (scan_clauses != NULL)
    {
        elog(LOG, "[%s] - Length of clauses BEFORE extraction: %d",
                    __func__, scan_clauses->length);
    }

    // Remove pseudo-constant clauses
    scan_clauses = extract_actual_clauses(scan_clauses, false);
    if (scan_clauses != NULL)
    {
        elog(LOG, "[%s] - Length of clauses AFTER extraction: %d",
                    __func__, scan_clauses->length);
    }

    // 1. make sure floating point representation doesn't trick us
    // 2. only set the limit if this is a single table
    List* limit = NULL;
    size_t nrels = bms_num_members(root->all_baserels);
    if( nrels == 1 && root->limit_tuples > 0.9 )
    {
        limit = list_make1_int(0.1+root->limit_tuples);
    }

    ForeignScan * ret =
        make_foreignscan(
            tlist,
            scan_clauses,
            scan_relid,
            NIL,
            limit);

    return ret;
}

static void
cbBeginForeignScan( ForeignScanState *node,
                    int eflags )
{
    // elog_node_display(INFO, "node: ", node->ss.ps.plan, true);

    ListCell   *l;
    struct AttInMetadata * meta = TupleDescGetAttInMetadata(node->ss.ss_currentRelation->rd_att);
    filter* filterChain = new op_expr_filter();
    filterChain->add(new nulltest_filter());
    filterChain->add(new any_all_filter());
    filterChain->add(new bool_expr_filter());
    filterChain->add(new default_filter());
    try
    {
        virtdb::query query_data;

        // Table name
        std::string table_name{RelationGetRelationName(node->ss.ss_currentRelation)};
        for (auto& c: table_name)
        {
            c = ::toupper(c);
        }
        query_data.set_table_name( table_name );

        // Columns
        int n = node->ss.ps.plan->targetlist->length;
        ListCell* cell = node->ss.ps.plan->targetlist->head;
        for (int i = 0; i < n; i++)
        {
            if (!IsA(lfirst(cell), TargetEntry))
            {
                continue;
            }
            Expr* expr = reinterpret_cast<Expr*> (lfirst(cell));
            const Var* variable = get_variable(expr);
            if (variable != nullptr)
            {
                // elog(LOG, "Column: %s (%d)", meta->tupdesc->attrs[variable->varattno-1]->attname.data, variable->varattno-1);
                query_data.add_column( variable->varattno-1, meta->tupdesc->attrs[variable->varattno-1]->attname.data );
            }
            cell = cell->next;
        }

        // Filters
        foreach(l, node->ss.ps.plan->qual)
        {
            Expr* clause = (Expr*) lfirst(l);
            query_data.add_filter( filterChain->apply(clause, meta) );
        }

        // Limit
        // From: http://www.postgresql.org/docs/9.2/static/fdw-callbacks.html
        // Information about the table to scan is accessible through the ForeignScanState node
        // (in particular, from the underlying ForeignScan plan node, which contains any
        // FDW-private information provided by GetForeignPlan).
        ForeignScan *plan = reinterpret_cast<ForeignScan *>(node->ss.ps.plan);
        if (plan->fdw_private)
        {
            query_data.set_limit( lfirst_int(plan->fdw_private->head) );
        }

        // Schema

        // UserToken

        // AccessInfo

        // Prepare for getting data
        LOG_TRACE("Before add_query");
        current_provider->worker_thread->send_query(node, query_data);
        LOG_TRACE("After add_query");
    }
    catch(const std::exception & e)
    {
        elog(ERROR, "[%s:%d] internal error in %s: %s",__FILE__,__LINE__,__func__,e.what());
    }
}

static TupleTableSlot *
cbIterateForeignScan(ForeignScanState *node)
{
    struct AttInMetadata * meta = TupleDescGetAttInMetadata(node->ss.ss_currentRelation->rd_att);
    data_handler* handler = current_provider->worker_thread->get_data_handler(node);
    if (!handler->received_data())
    {
        current_provider->worker_thread->wait_for_data(node);
    }
    if (handler->has_data())
    {
        TupleTableSlot *slot = node->ss.ss_ScanTupleSlot;
        ExecClearTuple(slot);
        try
        {
            handler->read_next();

            for (int column_id : handler->column_ids())
            {
                if (handler->is_null(column_id))
                {
                    slot->tts_isnull[column_id] = true;
                }
                else
                {
                    slot->tts_isnull[column_id] = false;
                    switch( meta->tupdesc->attrs[column_id]->atttypid )
                    {
                        case VARCHAROID: {
                            const std::string* const data = handler->get<std::string>(column_id);
                            bytea *vcdata = reinterpret_cast<bytea *>(palloc(data->size() + VARHDRSZ));
                            ::memcpy( VARDATA(vcdata), data->c_str(), data->size() );
                            SET_VARSIZE(vcdata, data->size() + VARHDRSZ);
                            slot->tts_values[column_id] = PointerGetDatum(vcdata);
                            break;
                        }
                        case INT4OID: {
                            const int32_t* const data = handler->get<int32_t>(column_id);
                            slot->tts_values[column_id] = Int32GetDatum(*data);
                            break;
                        }
                        case INT8OID: {
                            const int64_t* const data = handler->get<int64_t>(column_id);
                            slot->tts_values[column_id] = Int64GetDatum(*data);
                            break;
                        }
                        case FLOAT8OID:  {
                            const double* const data = handler->get<double>(column_id);
                            slot->tts_values[column_id] = Float8GetDatum(*data);
                            break;
                        }
                        case FLOAT4OID:  {
                            const float* const data = handler->get<float>(column_id);
                            slot->tts_values[column_id] = Float4GetDatum(*data);
                            break;
                        }
                        case NUMERICOID: {
                            const std::string* const data = handler->get<std::string>(column_id);
                            slot->tts_values[column_id] =
                                DirectFunctionCall3( numeric_in,
                                    CStringGetDatum(data->c_str()),
                                    ObjectIdGetDatum(InvalidOid),
                                    Int32GetDatum(meta->tupdesc->attrs[column_id]->atttypmod) );
                            break;
                        }
                        case DATEOID: {
                            const std::string* const data = handler->get<std::string>(column_id);
                            slot->tts_values[column_id] =
                                DirectFunctionCall1( date_in,
                                    CStringGetDatum(data->c_str()));
                            break;
                        }
                        case TIMEOID: {
                            const std::string* const data = handler->get<std::string>(column_id);
                            slot->tts_values[column_id] =
                                DirectFunctionCall1( time_in,
                                    CStringGetDatum(data->c_str()));
                            break;
                        }
                        default: {
                            LOG_ERROR("Unhandled attribute type: " << V_(meta->tupdesc->attrs[column_id]->atttypid));
                            slot->tts_isnull[column_id] = true;
                            break;
                        }
                    }
                }
            }
            ExecStoreVirtualTuple(slot);
        }
        catch(const std::logic_error & e)
        {
            elog(ERROR, "[%s:%d] internal error in %s: %s",__FILE__,__LINE__,__func__, e.what());
        }
        return slot;
    }
    else
    {
        // return NULL if there is no more data.
        return NULL;
    }
}

static void
cbReScanForeignScan( ForeignScanState *node )
{
    cbBeginForeignScan(node, 0);
}

static void
cbEndForeignScan(ForeignScanState *node)
{
    current_provider->worker_thread->remove_query(node);
}

}

// C++ implementation of the forward declared function
extern "C" {

void PG_init_virtdb_fdw_cpp(void)
{
}

void PG_fini_virtdb_fdw_cpp(void)
{
    // delete zmq_context;
    // current_provider->worker_thread->stop();
    // delete current_provider->worker_thread;
    // delete log_client;
    // delete ep_client;
}

Datum virtdb_fdw_status_cpp(PG_FUNCTION_ARGS)
{
    char * v = (char *)palloc(4);
    strcpy(v,"XX!");
    PG_RETURN_CSTRING(v);
}

struct fdwOption
{
    std::string   option_name;
    Oid		      option_context;
};

static struct fdwOption valid_options[] =
{

	/* Connection options */
	{ "url",  ForeignDataWrapperRelationId },
    { "provider", ForeignTableRelationId },

	/* Sentinel */
	{ "",	InvalidOid }
};

Datum virtdb_fdw_handler_cpp(PG_FUNCTION_ARGS)
{
    FdwRoutine *fdw_routine = makeNode(FdwRoutine);

    // must define these
    fdw_routine->GetForeignRelSize    = virtdb_fdw_priv::cbGetForeignRelSize;
    fdw_routine->GetForeignPaths      = virtdb_fdw_priv::cbGetForeignPaths;
    fdw_routine->GetForeignPlan       = virtdb_fdw_priv::cbGetForeignPlan;
    fdw_routine->BeginForeignScan     = virtdb_fdw_priv::cbBeginForeignScan;
    fdw_routine->IterateForeignScan   = virtdb_fdw_priv::cbIterateForeignScan;
    fdw_routine->ReScanForeignScan    = virtdb_fdw_priv::cbReScanForeignScan;
    fdw_routine->EndForeignScan       = virtdb_fdw_priv::cbEndForeignScan;

    // optional fields will be NULL for now
    fdw_routine->AddForeignUpdateTargets  = NULL;
    fdw_routine->PlanForeignModify        = NULL;
    fdw_routine->BeginForeignModify       = NULL;
    fdw_routine->ExecForeignInsert        = NULL;
    fdw_routine->ExecForeignUpdate        = NULL;
    fdw_routine->ExecForeignDelete        = NULL;
    fdw_routine->EndForeignModify         = NULL;

    // optional EXPLAIN support is also omitted
    fdw_routine->ExplainForeignScan    = NULL;
    fdw_routine->ExplainForeignModify  = NULL;

    // optional ANALYZE support is also omitted
    fdw_routine->AnalyzeForeignTable  = NULL;

    PG_RETURN_POINTER(fdw_routine);
}

static bool
is_valid_option(std::string option, Oid context)
{
    for (auto opt : valid_options)
	{
		if (context == opt.option_context && opt.option_name == option)
			return true;
	}
	return false;
}

Datum virtdb_fdw_validator_cpp(PG_FUNCTION_ARGS)
{
    elog(LOG, "virtdb_fdw_validator_cpp");
    List      *options_list = untransformRelOptions(PG_GETARG_DATUM(0));
    Oid       catalog = PG_GETARG_OID(1);
    ListCell  *cell;
    foreach(cell, options_list)
	{
        DefElem	   *def = (DefElem *) lfirst(cell);
        std::string option_name = def->defname;
        if (!is_valid_option(option_name, catalog))
        {
            elog(ERROR, "[%s] - Invalid option: %s", __func__, option_name.c_str());
        }
        elog(LOG, "Option name: %s", option_name.c_str());
        if (option_name == "url")
        {
            elog(LOG, "Config server url in validator: %s", defGetString(def));
        }
    }
    PG_RETURN_VOID();
}

}

#pragma GCC diagnostic pop
