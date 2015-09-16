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
    #include <catalog/pg_user_mapping.h>
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

endpoint_client::sptr    ep_client;
log_record_client::sptr  log_client;

namespace virtdb_fdw_priv {

  class provider
  {
    std::string             name_;
    client_context::sptr    cli_ctx_;
    endpoint_client::sptr   ep_cli_;
    receiver_thread::sptr   worker_thread_;
    query_client::sptr      query_push_client_;
    column_client::sptr     column_sub_client_;

    provider() = delete;
    provider(const provider &) = delete;
    provider & operator=(const provider &) = delete;

  public:
    typedef std::shared_ptr<provider> sptr;

    provider(const std::string name,
             endpoint_client::sptr epcli_sptr)
    : name_{name},
      cli_ctx_{new client_context},
      ep_cli_{epcli_sptr},
      worker_thread_{new receiver_thread},
      query_push_client_{new query_client{cli_ctx_, *ep_cli_, name}},
      column_sub_client_{new column_client{cli_ctx_, *ep_cli_, name}}
    {
      uint64_t timeout_ms = 10000;

      if( !query_push_client_->wait_valid(timeout_ms) )
      {
        LOG_ERROR("failed to connect query client" <<
                  V_(ep_client->name()) <<
                  V_(name) <<
                  V_(timeout_ms));

        THROW_("failed to connect query client");
      }

      if( !column_sub_client_->wait_valid(timeout_ms) )
      {
        LOG_ERROR("failed to connect column client" <<
                  V_(ep_client->name()) <<
                  V_(name) <<
                  V_(timeout_ms));

        THROW_("failed to connect column client");
      }

      // rethrow errors if any
      column_sub_client_->rethrow_error();
    }

    const std::string & name() const { return name_; }

    receiver_thread::sptr   worker_thread()     { return worker_thread_; }
    query_client::sptr      query_push_client() { return query_push_client_; }
    column_client::sptr     column_sub_client() { return column_sub_client_; }

    void
    send_query(long node,
               const virtdb::engine::query& query)
    {
      if( worker_thread_ && query_push_client_ && column_sub_client_ )
      {
        worker_thread_->send_query(query_push_client_,
                                   column_sub_client_,
                                   node,
                                   query);
      }
      else
      {
        THROW_("cannot send query, invalid state");
      }
    }

    void
    stop_query(const std::string& table_name,
               long node,
               const std::string& segment_id="")
    {
      if( worker_thread_ && query_push_client_ )
      {
        worker_thread_->stop_query(table_name,
                                   query_push_client_,
                                   node,
                                   segment_id);
      }
      else
      {
        THROW_("cannot stop query, invalid state");
      }
    }

    void
    remove_query(long node)
    {
      if( worker_thread_ && column_sub_client_ )
      {
        worker_thread_->remove_query(column_sub_client_,
                                     node);
      }
      else
      {
        THROW_("cannot stop query, invalid state");
      }
    }

    receiver_thread::handler_sptr
    get_data_handler(long node)
    {
      receiver_thread::handler_sptr ret;
      if( worker_thread_ )
      {
        ret = worker_thread_->get_data_handler(node);
      }
      else
      {
        THROW_("cannot get handler. invalid state");
      }
      return ret;
    }

    ~provider() {}
  };

  std::map<std::string, provider::sptr> providers;

  static void
  onError(std::string message = "")
  {
    // log_client.reset();
    // ep_client.reset();
    elog(ERROR, "Error happened %s", message.c_str());
  }

  std::string
  getOption(const std::string& option_name,
            List* list)
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

  std::string
  getTableOption(const std::string& option_name,
                 Oid foreigntableid)
  {
    auto table = GetForeignTable(foreigntableid);
    return getOption(option_name, table->options);
  }

  std::string
  getFDWOption(const std::string& option_name,
               Oid foreigntableid)
  {
    auto table = GetForeignTable(foreigntableid);
    ListCell *cell;
    auto server = GetForeignServer(table->serverid);
    auto fdw = GetForeignDataWrapper(server->fdwid);
    return getOption(option_name, fdw->options);
  }

  provider::sptr
  getProvider(Oid foreigntableid)
  {
    provider::sptr current_provider;
    try
    {
      uint64_t timeout = 10000;
      std::string name = getTableOption("provider",
                                        foreigntableid);

      auto it = providers.find(name);

      if( it == providers.end() )
      {
        current_provider.reset(new provider{name, ep_client});
        providers[name] = current_provider;
      }
      else
      {
        current_provider = it->second;
      }

      return current_provider;
    }
    catch(const std::exception & e)
    {
      onError(e.what());
    }
    return current_provider;
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
      client_context::sptr cli_ctx{new client_context};

      // TODO : move this to module load / initialization
      if (ep_client == nullptr)
      {
        std::string config_server_url = getFDWOption("url", foreigntableid);
        elog(LOG, "Config server url: %s", config_server_url.c_str());
        if (config_server_url != "")
        {
          ep_client.reset(new endpoint_client(cli_ctx, config_server_url, "postgres_generic_fdw"));
        }
      }

      // TODO : move this to module load / initialization
      if (log_client == nullptr)
      {
        log_client.reset(new log_record_client(cli_ctx, *ep_client, "diag-service"));

        if( !log_client->wait_valid_push(timeout) )
        {
          LOG_ERROR("failed to connect log client" <<
                    V_(ep_client->name()) <<
                    V_(ep_client->service_ep()) <<
                    V_(timeout));

          THROW_("failed to connect log client");
        }
      }

      ep_client->rethrow_error();
      log_client->rethrow_error();
    }
    catch(const std::exception & e)
    {
      onError(e.what());
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
             reinterpret_cast<Path *>(create_foreignscan_path(root,
                                                              baserel,
                                                              baserel->rows,
                                                              startup_cost,
                                                              total_cost,
                                                              // no pathkeys:  TODO! check-this!
                                                              NIL,
                                                              nullptr,
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
    if (scan_clauses != nullptr)
    {
      elog(LOG, "[%s] - Length of clauses BEFORE extraction: %d",
           __func__, scan_clauses->length);
    }

    // Remove pseudo-constant clauses
    scan_clauses = extract_actual_clauses(scan_clauses, false);
    if (scan_clauses != nullptr)
    {
      elog(LOG, "[%s] - Length of clauses AFTER extraction: %d",
           __func__, scan_clauses->length);
    }

    // 1. make sure floating point representation doesn't trick us
    // 2. only set the limit if this is a single table
    List* limit = nullptr;
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

  virtdb::interface::pb::Field
  getField(const std::string& name, Oid atttypid)
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
      auto foreign_table_id = RelationGetRelid(node->ss.ss_currentRelation);
      virtdb::engine::query query_data;
      auto current_provider = getProvider(foreign_table_id);
      // LOG_SCOPED(P_(current_provider.get()));

      if( !current_provider )
      {
        THROW_("invalid provider object. not initialized");
      }

      // Table name
      auto table_name = getTableOption("remotename", foreign_table_id);
      query_data.set_table_name(table_name);

      // Filters
      std::vector<const Var *> all_variables;
      foreach(l, node->ss.ps.plan->qual)
      {
        Expr* clause = (Expr*) lfirst(l);
        // make sure we add all columns that are referenced in the query filters
        get_variable(clause, all_variables);
        query_data.add_filter( filterChain->apply(clause, meta) );
      }
      
      // Columns
      std::set<engine::column_id_t> added_columns;
      int n = node->ss.ps.plan->targetlist->length;
      ListCell* cell = node->ss.ps.plan->targetlist->head;
      for (int i = 0; i < n; ++i)
      {
        TargetEntry *tle = (TargetEntry *) lfirst(cell);
        if (!IsA(tle, TargetEntry))
          continue;
      	if (tle->resjunk)
          continue;
        Expr* expr = reinterpret_cast<Expr*> (tle);
        const Var* single_variable = get_variable(expr, all_variables);

        if (single_variable != nullptr )
        {          
          for( const Var* variable : all_variables )
          {
            if( variable->varattno <= meta->tupdesc->natts )
            {
              // elog(LOG, "Column: %s (%d)", meta->tupdesc->attrs[variable->varattno-1]->attname.data, variable->varattno-1);
              engine::column_id_t col_id = static_cast<engine::column_id_t>(variable->varattno-1);
              auto tupdesc_attrs = meta->tupdesc->attrs;
              if( tupdesc_attrs )
              {
                auto var_attr = tupdesc_attrs[variable->varattno-1];
                if( var_attr )
                {
                  if( added_columns.count(col_id) == 0 &&
                      var_attr->attname.data )
                  {
                    query_data.add_column(col_id, var_attr->attname.data);
                    added_columns.insert(col_id);
                  }
                }
                else { elog(LOG, "VIRTDB WARN: var_attr is null"); }
              }
              else { elog(LOG, "VIRTDB WARN: tupdesc_attrs is null"); }
            }
            else { elog(LOG, "VIRTDB WARN natts=%d varattno=%d, variable is not valid",variable->varattno,meta->tupdesc->natts); }
          }
          all_variables.clear();
        }
        else
        {
          elog(LOG, "VIRTDB WARN: variable is null");
        }

        cell = cell->next;
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
      query_data.set_schema(getTableOption("schema", foreign_table_id));

      // UserToken
      auto* table = GetForeignTable(foreign_table_id);
      auto* mapping = GetUserMapping(GetUserId(), table->serverid);
      foreach(cell, mapping->options)
      {
        DefElem  *def = (DefElem *) lfirst(cell);
        std::string option_name = def->defname;
        if (option_name == "token")
        {
          std::string token = defGetString(def);
          if( !token.empty() )
            query_data.set_usertoken(token);
        }
      }

      // AccessInfo

      // Prepare for getting data
      current_provider->send_query(reinterpret_cast<long>(node),
                                   query_data);
    }
    catch(const std::exception & e)
    {
      onError(e.what());
    }
  }
  
  static TupleTableSlot *
  cbIterateForeignScan(ForeignScanState *node)
  {
    struct AttInMetadata * meta = TupleDescGetAttInMetadata(node->ss.ss_currentRelation->rd_att);
    auto foreign_table_id = RelationGetRelid(node->ss.ss_currentRelation);
    try
    {
      auto current_provider = getProvider(foreign_table_id);
      // LOG_SCOPED(P_(current_provider.get()));

      if( !current_provider ) { THROW_("current p has invalid value"); }

      auto handler = current_provider->get_data_handler(reinterpret_cast<long>(node));

      if( !handler ) { THROW_("handler has invalid value"); }
      if( handler->column_id_map().size() == 0 ) { THROW_("handler has no columns"); }

      {
        feeder & fdr = handler->get_feeder();
        TupleTableSlot *slot = node->ss.ss_ScanTupleSlot;
        ExecClearTuple(slot);

        if( !fdr.started() )
        {
          // try to gather first block
          if( !fdr.fetch_next() )
          {
            LOG_TRACE("fetch_next=false started=false. cannot gather first block.");
            return nullptr;
          }
        }
        else if( !fdr.has_more() )
        {
          // try to gather next block
          if( !fdr.fetch_next() )
          {
            LOG_TRACE("fetch_next=false started=true. cannot gather next block.");
            return nullptr;
          }
        }

        for (auto const & cid : handler->column_id_map() )
        {
          int column_id        = cid.first;
          size_t query_col_id  = cid.second;
          bool is_null         = false;

          slot->tts_isnull[column_id] = true;

          switch( meta->tupdesc->attrs[column_id]->atttypid )
          {
            case VARCHAROID:
            {
              char * ptr = nullptr;
              size_t len = 0;
              if( fdr.read_string(query_col_id, &ptr, len, is_null) != feeder::vtr::ok_ )
              {
                LOG_TRACE("read_string failed" << V_(column_id) << V_(query_col_id) << V_(len) << "VARCHAROID");
                return nullptr;
              }

              slot->tts_isnull[column_id] = is_null;
              if( !is_null )
              {
                if (len < VARATT_SHORT_MAX-VARHDRSZ_SHORT)
                {
                  // lucky case: we can reuse the leading tag and length data from the string
                  bytea *vcdata = reinterpret_cast<bytea *>(ptr-VARHDRSZ_SHORT);
                  SET_VARSIZE_SHORT(vcdata, len + VARHDRSZ_SHORT);
                  slot->tts_values[column_id] = PointerGetDatum(vcdata);
                }
                else
                {
                  // bad case: we need to allocate and copy
                  bytea *vcdata = reinterpret_cast<bytea *>(palloc(len + VARHDRSZ));
                  ::memcpy( VARDATA(vcdata), ptr, len );
                  SET_VARSIZE(vcdata, len + VARHDRSZ);
                  slot->tts_values[column_id] = PointerGetDatum(vcdata);
                }
              }
              break;
            }
            case INT4OID:
            {
              int32_t val = 0;
              if( fdr.read_int32(query_col_id, val, is_null) != feeder::vtr::ok_ )
              {
                uint32_t uval = 0;
                if( fdr.read_uint32(query_col_id, uval, is_null) != feeder::vtr::ok_ )
                {
                  LOG_TRACE("read_[u]int32 failed" << V_(column_id) << V_(query_col_id) << "INT4OID");               
                  return nullptr;
                }
                val = uval;
              }

              slot->tts_isnull[column_id] = is_null;
              if( !is_null )
                slot->tts_values[column_id] = Int32GetDatum(val);
              break;
            }
            case INT8OID:
            {
              int64_t val = 0;
              if( fdr.read_int64(query_col_id, val, is_null) != feeder::vtr::ok_ )
              {
                uint64_t uval = 0;
                if( fdr.read_uint64(query_col_id, uval, is_null) != feeder::vtr::ok_ )
                {
                  LOG_TRACE("read_[u]int64 failed" << V_(column_id) << V_(query_col_id) << "INT8OID");                 
                  return nullptr;
                }
                val = uval;
              }

              slot->tts_isnull[column_id] = is_null;
              if( !is_null )
                slot->tts_values[column_id] = Int64GetDatum(val);
              break;
            }
            case FLOAT8OID:
            {
              double val = 0;
              if( fdr.read_double(query_col_id, val, is_null) != feeder::vtr::ok_ )
              {
                LOG_TRACE("read_double failed" << V_(column_id) << V_(query_col_id) << "FLOAT8OID");
                return nullptr;
              }

              slot->tts_isnull[column_id] = is_null;
              if( !is_null )
                slot->tts_values[column_id] = Float8GetDatum(val);
              break;
            }
            case FLOAT4OID:
            {
              float val = 0;
              if( fdr.read_float(query_col_id, val, is_null) != feeder::vtr::ok_ )
              {
                LOG_TRACE("read_float failed" << V_(column_id) << V_(query_col_id) << "FLOAT4OID");
                return nullptr;
              }

              slot->tts_isnull[column_id] = is_null;
              if( !is_null )
                slot->tts_values[column_id] = Float4GetDatum(val);
              break;
            }
            case NUMERICOID:
            {

              char * ptr = nullptr;
              size_t len = 0;
              if( fdr.read_string(query_col_id, &ptr, len, is_null) != feeder::vtr::ok_ )
              {
                LOG_TRACE("read_string failed" << V_(column_id) << V_(query_col_id) << V_(len) << "NUMERICOID");
                return nullptr;
              }

              slot->tts_isnull[column_id] = is_null;
              if( !is_null )
              {
                ptr[len] = 0;
                slot->tts_values[column_id] =
                DirectFunctionCall3( numeric_in,
                                    CStringGetDatum(ptr),
                                    ObjectIdGetDatum(InvalidOid),
                                    Int32GetDatum(meta->tupdesc->attrs[column_id]->atttypmod) );
              }
              break;
            }
            case DATEOID:
            {
              char * ptr = nullptr;
              size_t len = 0;
              if( fdr.read_string(query_col_id, &ptr, len, is_null) != feeder::vtr::ok_ )
              {
                LOG_TRACE("read_string failed" << V_(column_id) << V_(query_col_id) << V_(len) << "DATEOID");
                return nullptr;
              }

              slot->tts_isnull[column_id] = is_null;
              if( !is_null )
              {
                ptr[len] = 0;
                slot->tts_values[column_id] = DirectFunctionCall1( date_in, CStringGetDatum(ptr));
              }
              break;
            }
            case TIMESTAMPOID:
            {
              char * ptr = nullptr;
              size_t len = 0;
              if( fdr.read_string(query_col_id, &ptr, len, is_null) != feeder::vtr::ok_ )
              {
                LOG_TRACE("read_string failed" << V_(column_id) << V_(query_col_id) << V_(len) << "TIMESTAMPOID");
                return nullptr;
              }

              slot->tts_isnull[column_id] = is_null;
              if( !is_null )
              {
                ptr[len] = 0;
                slot->tts_values[column_id] =
                DirectFunctionCall3(timestamp_in,
                                    CStringGetDatum(ptr),
                                    ObjectIdGetDatum(InvalidOid),
                                    Int32GetDatum(meta->tupdesc->attrs[column_id]->atttypmod) );
              }
              break;
            }
            case TIMEOID:
            {
              char * ptr = nullptr;
              size_t len = 0;
              if( fdr.read_string(query_col_id, &ptr, len, is_null) != feeder::vtr::ok_ )
              {
                LOG_TRACE("read_string failed" << V_(column_id) << V_(query_col_id) << V_(len) << "TIMEOID");
                return nullptr;
              }

              slot->tts_isnull[column_id] = is_null;
              if( !is_null )
              {
                ptr[len] = 0;
                slot->tts_values[column_id] = DirectFunctionCall1( time_in, CStringGetDatum(ptr));
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
        return slot;
      }
    }
    catch(const std::exception& e)
    {
#ifndef RELEASE
      elog_node_display(INFO, "node: ", node->ss.ps.plan, true);
#endif
      onError(e.what());
      return nullptr;
    }
  }


  static void
  cbReScanForeignScan( ForeignScanState *node )
  {
    // TODO : make this more efficient !
    cbBeginForeignScan(node, 0);
  }

  static void
  cbEndForeignScan(ForeignScanState *node)
  {
    auto foreign_table_id = RelationGetRelid(node->ss.ss_currentRelation);
    auto current_provider = getProvider(foreign_table_id);
    if( current_provider )
    {
      try
      {
        /* this doesn't ever happen to be in the middle of the query, so
         * this would be better removed from here ....
         */
        /* NOTE : Removed because of the lack of Postgres support
        auto table_name = getTableOption("remotename", foreign_table_id);
        current_provider->stop_query(table_name,
                                     reinterpret_cast<long>(node));
        */
        current_provider->remove_query(reinterpret_cast<long>(node));
      }
      catch(const std::exception & e)
      {
        onError(e.what());
      }
    }
  }
}

// C++ implementation of the forward declared function
extern "C" {

  void PG_init_virtdb_fdw_cpp(void)
  {
    // TODO : check: can't we connect to log and endpoint here
  }

  void PG_fini_virtdb_fdw_cpp(void)
  {
    // TODO : check: can't we connect to log and endpoint here
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
    { "remotename", ForeignTableRelationId },
    /* Sentinel */
    { "",	InvalidOid }
  };

  Datum
  virtdb_fdw_handler_cpp(PG_FUNCTION_ARGS)
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

    // optional fields will be nullptr for now
    fdw_routine->AddForeignUpdateTargets  = nullptr;
    fdw_routine->PlanForeignModify        = nullptr;
    fdw_routine->BeginForeignModify       = nullptr;
    fdw_routine->ExecForeignInsert        = nullptr;
    fdw_routine->ExecForeignUpdate        = nullptr;
    fdw_routine->ExecForeignDelete        = nullptr;
    fdw_routine->EndForeignModify         = nullptr;

    // optional EXPLAIN support is also omitted
    fdw_routine->ExplainForeignScan    = nullptr;
    fdw_routine->ExplainForeignModify  = nullptr;

    // optional ANALYZE support is also omitted
    fdw_routine->AnalyzeForeignTable  = nullptr;

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

  Datum
  virtdb_fdw_validator_cpp(PG_FUNCTION_ARGS)
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
        // TODO : check: can't we connect to log and endpoint here
        elog(LOG, "Config server url in validator: %s", defGetString(def));
      }
    }
    PG_RETURN_VOID();
  }

} // end of virtdb_fdw_priv

#pragma GCC diagnostic pop
