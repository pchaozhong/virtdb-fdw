#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wold-style-cast"

#include <engine/expression.hh>
#include <engine/query.hh>
#include <engine/receiver_thread.hh>
#include <engine/data_handler.hh>
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
    #include <utils/timestamp.h>
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
using namespace virtdb::engine;

endpoint_client* ep_client;
log_record_client* log_client;

namespace virtdb_fdw_priv {

struct provider {
    std::string name = "";
    receiver_thread* worker_thread = nullptr;
    push_client<virtdb::interface::pb::Query>* query_push_client = nullptr;
    sub_client<virtdb::interface::pb::Column>* column_sub_client = nullptr;
};

provider* current_provider;
std::map<std::string, provider> providers;

std::string getOption(const std::string& option_name, List* list)
{
    ListCell *cell;
    foreach(cell, list)
    {
        DefElem *def = (DefElem *) lfirst(cell);
        std::string current_option_name = def->defname;
        if (current_option_name == option_name)
        {
            return defGetString(def);
        }
    }
    return "";
}

std::string getTableOption(const std::string& option_name, Oid foreigntableid)
{
    auto table = GetForeignTable(foreigntableid);
    return getOption(option_name, table->options);
}

std::string getFDWOption(const std::string& option_name, Oid foreigntableid)
{
    auto table = GetForeignTable(foreigntableid);
    ListCell *cell;
    auto server = GetForeignServer(table->serverid);
    auto fdw = GetForeignDataWrapper(server->fdwid);
    return getOption(option_name, fdw->options);
}

// We dont't do anything here right now, it is intended only for optimizations.
static void
cbGetForeignRelSize( PlannerInfo *root,
                     RelOptInfo *baserel,
                     Oid foreigntableid )
{
    try
    {
        uint64_t timeout = 10000;
        std::string name = getTableOption("provider", foreigntableid);
        current_provider = &providers[name];
        current_provider->name = name;

        if (current_provider && current_provider->worker_thread == nullptr)
        {
            current_provider->worker_thread = new receiver_thread();
        }


        if (ep_client == nullptr)
        {
            std::string config_server_url = getFDWOption("url", foreigntableid);
            elog(LOG, "Config server url: %s", config_server_url.c_str());
            if (config_server_url != "")
            {
                ep_client = new endpoint_client(config_server_url,
                                                "postgres_generic_fdw",
                                                5,     // retry count on 0MQ exception
                                                false  // shall kill the process by re-throwing?
                                                );
            }
        }

        if (log_client == nullptr)
        {
            log_client = new log_record_client(*ep_client,
                                               "diag-service",
                                               5,     // retry count on 0MQ exception
                                               false  // shall kill the process by re-throwing?
                                               );

            if( !log_client->wait_valid_push(timeout) )
            {
                LOG_ERROR("failed to connect log client" <<
                          V_(ep_client->name()) <<
                          V_(ep_client->service_ep()) <<
                          V_(timeout));

                THROW_("failed to connect log client");
            }

        }

        current_provider->query_push_client =
            new push_client<virtdb::interface::pb::Query>(*ep_client, current_provider->name);

        if( !current_provider->query_push_client->wait_valid(timeout) )
        {
            LOG_ERROR("failed to connect query client" <<
                   V_(ep_client->name()) <<
                   V_(current_provider->name) <<
                   V_(timeout));

            THROW_("failed to connect query client");
        }

        current_provider->column_sub_client =
            new sub_client<virtdb::interface::pb::Column>(*ep_client,
                                                          current_provider->name,
                                                          5,     // retry count on 0MQ exception
                                                          false  // shall kill the process by re-throwing?
                                                          );

        if( !current_provider->column_sub_client->wait_valid(timeout) )
        {
            LOG_ERROR("failed to connect column client" <<
                      V_(ep_client->name()) <<
                      V_(current_provider->name) <<
                      V_(timeout));

            THROW_("failed to connect column client");
        }

        ep_client->rethrow_error();
        log_client->rethrow_error();
        current_provider->column_sub_client->rethrow_error();
    }
    catch(const std::exception & e)
    {
        LOG_ERROR("Internal error." << E_(e));
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

virtdb::interface::pb::Field getField(const std::string& name, Oid atttypid)
{
    virtdb::interface::pb::Field ret;
    ret.set_name(name);
    switch (atttypid)
    {
        case VARCHAROID:
            ret.mutable_desc()->set_type(virtdb::interface::pb::Kind::STRING);
            break;
        case INT4OID:
            ret.mutable_desc()->set_type(virtdb::interface::pb::Kind::INT32);
            break;
        case INT8OID:
            ret.mutable_desc()->set_type(virtdb::interface::pb::Kind::INT64);
            break;
        case FLOAT8OID:
            ret.mutable_desc()->set_type(virtdb::interface::pb::Kind::DOUBLE);
            break;
        case FLOAT4OID:
            ret.mutable_desc()->set_type(virtdb::interface::pb::Kind::FLOAT);
            break;
        case NUMERICOID:
            ret.mutable_desc()->set_type(virtdb::interface::pb::Kind::NUMERIC);
            break;
        case DATEOID:
            ret.mutable_desc()->set_type(virtdb::interface::pb::Kind::DATE);
            break;
        case TIMESTAMPOID:
            ret.mutable_desc()->set_type(virtdb::interface::pb::Kind::DATETIME);
            break;
        case TIMEOID:
            ret.mutable_desc()->set_type(virtdb::interface::pb::Kind::TIME);
            break;
        default:
            ret.mutable_desc()->set_type(virtdb::interface::pb::Kind::STRING);
            break;
    }
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
        virtdb::engine::query query_data;

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
                query_data.add_column( static_cast<engine::column_id_t>(variable->varattno-1),
                    getField(meta->tupdesc->attrs[variable->varattno-1]->attname.data,
                            meta->tupdesc->attrs[variable->varattno-1]->atttypid));
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
        auto foreign_table_id = RelationGetRelid(node->ss.ss_currentRelation);
        query_data.set_schema(getTableOption("schema", foreign_table_id));

        // UserToken

        // AccessInfo

        // Prepare for getting data
        current_provider->worker_thread->send_query(*current_provider->query_push_client,
                                                    *current_provider->column_sub_client,
                                                    reinterpret_cast<long>(node),
                                                    query_data);
    }
    catch(const std::exception & e)
    {
        LOG_ERROR("Internal error" << E_(e));
    }
}

static TupleTableSlot *
cbIterateForeignScan(ForeignScanState *node)
{
    struct AttInMetadata * meta = TupleDescGetAttInMetadata(node->ss.ss_currentRelation->rd_att);
    data_handler* handler = current_provider->worker_thread->get_data_handler(reinterpret_cast<long>(node));
    if (handler->read_next())
    {
        TupleTableSlot *slot = node->ss.ss_ScanTupleSlot;
        ExecClearTuple(slot);
        try
        {
            for (int column_id : handler->column_ids())
            {
                slot->tts_isnull[column_id] = true;
                switch( meta->tupdesc->attrs[column_id]->atttypid )
                {
                    case VARCHAROID: {
                        const auto * data = handler->get<std::string>(column_id);
                        if (data != nullptr)
                        {
                            bytea *vcdata = reinterpret_cast<bytea *>(palloc(data->size() + VARHDRSZ));
                            ::memcpy( VARDATA(vcdata), data->c_str(), data->size() );
                            SET_VARSIZE(vcdata, data->size() + VARHDRSZ);
                            slot->tts_values[column_id] = PointerGetDatum(vcdata);
                            slot->tts_isnull[column_id] = false;
                        }
                        break;
                    }
                    case INT4OID: {
                        const auto * data = handler->get<int32_t>(column_id);
                        if (data != nullptr)
                        {
                            slot->tts_values[column_id] = Int32GetDatum(*data);
                            slot->tts_isnull[column_id] = false;
                        }
                        break;
                    }
                    case INT8OID: {
                        const auto * data = handler->get<int64_t>(column_id);
                        if (data != nullptr)
                        {
                            slot->tts_values[column_id] = Int64GetDatum(*data);
                            slot->tts_isnull[column_id] = false;
                        }
                        break;
                    }
                    case FLOAT8OID:  {
                        const auto * data = handler->get<double>(column_id);
                        if (data != nullptr)
                        {
                            slot->tts_values[column_id] = Float8GetDatum(*data);
                            slot->tts_isnull[column_id] = false;
                        }
                        break;
                    }
                    case FLOAT4OID:  {
                        const auto *  data = handler->get<float>(column_id);
                        if (data != nullptr)
                        {
                            slot->tts_values[column_id] = Float4GetDatum(*data);
                            slot->tts_isnull[column_id] = false;
                        }
                        break;
                    }
                    case NUMERICOID: {
                        const auto * data = handler->get<std::string>(column_id);
                        if (data != nullptr)
                        {
                            slot->tts_values[column_id] =
                                DirectFunctionCall3( numeric_in,
                                    CStringGetDatum(data->c_str()),
                                    ObjectIdGetDatum(InvalidOid),
                                    Int32GetDatum(meta->tupdesc->attrs[column_id]->atttypmod) );
                            slot->tts_isnull[column_id] = false;
                        }
                        break;
                    }
                    case DATEOID: {
                        const auto * data = handler->get<std::string>(column_id);
                        if (data != nullptr)
                        {
                            slot->tts_values[column_id] =
                                DirectFunctionCall1( date_in,
                                    CStringGetDatum(data->c_str()));
                            slot->tts_isnull[column_id] = false;
                        }
                        break;
                    }
                    case TIMESTAMPOID: {
                        const auto * data = handler->get<std::string>(column_id);
                        if (data != nullptr)
                        {
                            slot->tts_values[column_id] =
                                DirectFunctionCall3( timestamp_in,
                                    CStringGetDatum(data->c_str()),
                                    ObjectIdGetDatum(InvalidOid),
                                    Int32GetDatum(meta->tupdesc->attrs[column_id]->atttypmod) );
                            slot->tts_isnull[column_id] = false;
                        }
                        break;
                    }
                    case TIMEOID: {
                        const auto * data = handler->get<std::string>(column_id);
                        if (data != nullptr)
                        {
                            slot->tts_values[column_id] =
                                DirectFunctionCall1( time_in,
                                    CStringGetDatum(data->c_str()));
                            slot->tts_isnull[column_id] = false;
                        }
                        break;
                    }
                    default: {
                        LOG_ERROR("Unhandled attribute type: " << V_(meta->tupdesc->attrs[column_id]->atttypid));
                        break;
                    }
                }
            }
            ExecStoreVirtualTuple(slot);
        }
        // catch(const std::logic_error & e)
        catch(const std::exception& e)
        {
            LOG_ERROR("Internal error" << E_(e));
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
    current_provider->worker_thread->remove_query(*current_provider->column_sub_client, reinterpret_cast<long>(node));
}

}

// C++ implementation of the forward declared function
extern "C" {

void PG_init_virtdb_fdw_cpp(void)
{
}

void PG_fini_virtdb_fdw_cpp(void)
{
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
    { "schema", ForeignTableRelationId },

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
            LOG_ERROR("Invalid option." << V_(option_name));
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
