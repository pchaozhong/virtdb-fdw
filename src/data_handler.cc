#include "data_handler.hh"
#include <logger.hh>

namespace virtdb {

data_handler::data_handler(const query& query_data) :
    n_columns(query_data.columns_size()),
    queryid (query_data.id())
{
    LOG_TRACE("In data_handler ctr."<<V_(n_columns));
    for (int i = 0; i < n_columns; i++)
    {
        std::string colname = query_data.column(i);
        LOG_TRACE("Column name:" << V_(colname));
        column_names[colname] = query_data.column_id(i);
        LOG_TRACE("Column id:" << V_(query_data.column_id(i)));
        vec_column_ids.push_back(query_data.column_id(i));
    }
}

const std::string& data_handler::query_id() const
{
    return queryid;
}

void data_handler::push(std::string name, virtdb::interface::pb::Column new_data)
{
    push(column_names[name], new_data);
}

void data_handler::push(int column_id, virtdb::interface::pb::Column new_data)
{
    if (data.count(column_id) != 1)
    {
        data[column_id] = std::vector<virtdb::interface::pb::Column>();
    }
    data[column_id].push_back(new_data);
}

bool data_handler::received_data() const
{
    if (has_received_data)
    {
        return true;
    }
    if (data.size() != n_columns)
    {
        return false;
    }
    int ended = 0;
    for (auto item : data)
    {
        for (auto column_proto : item.second)
        {
            if (column_proto.endofdata())
            {
                ended += 1;
                break;
            }
        }
    }
    if (ended == n_columns)
    {
        has_received_data = true;
        return true;
    }
    return false;
}


bool data_handler::has_data() const
{
    return received_data() && (cursor < data_length() - 1);
}

bool data_handler::read_next()
{
    if (!has_data())
    {
        return false;
    }
    if (inner_cursor >= data.begin()->second[current_chunk].data().isnull_size() - 1)
    {
        inner_cursor = -1;
        current_chunk += 1;
    }
    inner_cursor++;
    cursor++;
    return true;
}

bool data_handler::is_null(int column_id) const
{
    try {
        if (data.count(column_id) != 1)
        {
            return true;
        }
        if (cursor < 0 || cursor >= data_length())
        {
            std::string error_string = "Error! Invalid row: " + std::to_string(cursor);
            throw std::invalid_argument(error_string);
        }
        return data.find(column_id)->second[current_chunk].data().isnull(inner_cursor);
    }
    catch(const ::google::protobuf::FatalException & e)
    {
        std::string error_string = "Error [" + std::to_string(__LINE__) + "]! Inner_cursor: " + std::to_string(inner_cursor) + " " + e.what();
        throw std::invalid_argument(error_string);
    }
}

int data_handler::data_length() const
{
    int count = 0;
    if (received_data())
    {
        for (auto it = data.begin()->second.begin(); it != data.begin()->second.end(); it++)
        {
            count += it->data().isnull_size();
        }
    }
    return count;
}

} // namespace virtdb
