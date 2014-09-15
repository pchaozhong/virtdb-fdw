// Standard headers
#include <chrono>

// Google Protocol Buffers
#include "data.pb.h"

// ZeroMQ
#include "cppzmq/zmq.hpp"

// VirtDB headers
#include "receiver_thread.hh"
#include "data_handler.hh"
#include <logger.hh>

using namespace virtdb;

receiver_thread::receiver_thread(zmq::context_t* context) :
    zmq_context(context)
{
}

receiver_thread::~receiver_thread()
{
    delete data_socket;
}

void receiver_thread::send_query(const ForeignScanState* const node, const virtdb::query& query_data)
{
    add_query(node, query_data);

    std::unique_lock<std::mutex> cv_lock(query_url_cv.mutex);
    while (query_address == "")
    {
        if (query_url_cv.variable.wait_for(cv_lock, std::chrono::milliseconds(10000)) == std::cv_status::timeout)
        {
            LOG_ERROR("Could not determine QUERY_ADDRESS in time.");
            return;
        }
    }

    try {
        LOG_TRACE("Sending message to:" << V_(query_address));
        zmq::socket_t socket (*zmq_context, ZMQ_PUSH);
        socket.connect (query_address.c_str());
        std::string str;
        query_data.get_message().SerializeToString(&str);
        int sz = str.length();
        zmq::message_t query_message(sz);
        memcpy(query_message.data (), str.c_str(), sz);
        socket.send (query_message);
    }
    catch (const zmq::error_t& err)
    {
        LOG_ERROR("Error num:" << V_(err.num()));
        remove_query(node);
    }
}

void receiver_thread::set_query_url(const std::string& url)
{
    query_address = url;
    query_url_cv.variable.notify_all();
}

void receiver_thread::set_data_url(const std::string& url)
{
    data_address = url;
    data_socket = new zmq::socket_t (*zmq_context, ZMQ_SUB);
    data_socket->connect(data_address.c_str());
    data_url_cv.variable.notify_all();
}


void receiver_thread::add_query(const ForeignScanState* const node, const virtdb::query& query_data)
{
    std::unique_lock<std::mutex> cv_lock(data_url_cv.mutex);
    while (data_address == "")
    {
        if (data_url_cv.variable.wait_for(cv_lock, std::chrono::milliseconds(10000)) == std::cv_status::timeout)
        {
            LOG_ERROR("Could not determine DATA_ADDRESS in time.");
            return;
        }
    }

    LOG_TRACE("Added query: " << V_(query_data.id()));
    active_queries[node] = new data_handler(query_data);
    LOG_TRACE("Data handler created.");
    data_socket->setsockopt( ZMQ_SUBSCRIBE, query_data.id().c_str(), 0);
    LOG_TRACE("Socket options set.");
}

void receiver_thread::remove_query(const ForeignScanState* const node)
{
    std::unique_lock<std::mutex> cv_lock(data_url_cv.mutex);
    while (data_address == "")
    {
        if (data_url_cv.variable.wait_for(cv_lock, std::chrono::milliseconds(10000)) == std::cv_status::timeout)
        {
            LOG_ERROR("Could not determine DATA_ADDRESS in time.");
            return;
        }
    }

    data_handler* handler = active_queries[node];
    data_socket->setsockopt( ZMQ_UNSUBSCRIBE, handler->query_id().c_str(), 0);
    LOG_TRACE("Removed query: " << V_(handler->query_id()));
    active_queries.erase(node);
    delete handler;
}

void receiver_thread::stop()
{
    done = true;
}

data_handler* receiver_thread::get_data_handler(const ForeignScanState* const node)
{
    return active_queries[node];
}

data_handler* receiver_thread::get_data_handler(std::string queryid)
{
    for (auto it = active_queries.begin(); it != active_queries.end(); it++)
    {
        if (it->second->query_id() == queryid)
        {
            return it->second;
        }
    }
    return NULL;
}

bool receiver_thread::wait_for_data(const ForeignScanState* const node)
{
    data_handler* handler = get_data_handler(node);
    std::unique_lock<std::mutex> cv_lock(cv[handler->query_id()].mutex);
    while (!handler->received_data())
    {
        if (cv[handler->query_id()].variable.wait_for(cv_lock, std::chrono::milliseconds(10000)) == std::cv_status::timeout)
        {
            // elog(ERROR, "wait_for_data timed out.");
            return handler->received_data() != 0;
        }
    }
    return true;
}

void receiver_thread::run()
{
    try
    {
        zmq::message_t query_id;
        zmq::message_t update;

        while (!done)
        {
            try
            {
                if (data_socket != nullptr)
                {
                    // QueryID
                    data_socket->recv(&query_id);

                    // Column data
                    data_socket->recv(&update);
                    virtdb::interface::pb::Column column;
                    column.ParseFromArray(update.data(), update.size());
                    data_handler* handler = get_data_handler(column.queryid());
                    std::unique_lock<std::mutex> cv_lock(cv[handler->query_id()].mutex);
                    handler->push(column.name(), column);
                    if (handler->received_data())
                    {
                        cv[column.queryid()].variable.notify_all();
                    }
                }
            }
            catch(const std::exception & e)
            {
                // elog(ERROR, "[%s:%d] internal error in %s: %s",__FILE__,__LINE__,__func__,e.what());
            }
        }
    }
    catch(const std::exception & e)
    {
        // elog(ERROR, "[%s:%d] internal error in %s: %s",__FILE__,__LINE__,__func__,e.what());
    }
    // elog(LOG, "Receiver stopped");
}
