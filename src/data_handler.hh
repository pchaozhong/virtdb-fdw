#pragma once

#include <string>
#include <vector>

#include "query.hh"
#include "util/value_type.hh"

namespace virtdb {
    class data_handler {
        private:
            const int n_columns;
            const std::string queryid;
            // Not a vector as we may not get the columns in order
            std::map<int, std::vector<virtdb::interface::pb::Column> > data;
            std::map<std::string, int>  column_names;
            std::vector<int> vec_column_ids;
            mutable bool has_received_data = false;
            int cursor = -1;
            int inner_cursor = -1;
            int current_chunk = 0;

            // int data_length() const;

            virtdb::interface::pb::Column& chunk(int column_index, int chunk_index)
            {
                return data.find(column_index)->second[chunk_index];
            }

            void push(int column_id, virtdb::interface::pb::Column new_data);
        public:
            int data_length() const;
            data_handler(const query& query_data);

            const std::string& query_id() const;
            void push(std::string name, virtdb::interface::pb::Column new_data);

            // Returns true if we have received data for all needed columns
            bool received_data() const;

            // Returns true if at least one row is completely available
            // and not all rows have been read.
            bool has_data() const;

            // Sets the cursor to the next row if there is more.
            // Returns false if end of daa.
            bool read_next();

            inline int columns_count() const { return n_columns; }
            const std::vector<int>& column_ids() const
            {
                return vec_column_ids;
            }
            bool is_null(int column_number) const;

            // Returns the value of the given column in the actual row or NULL
            template<typename T>
            const T* const get(int column_id)
            {
                try {
                    if (is_null(column_id))
                        return NULL;
                    virtdb::interface::pb::ValueType* value = data.find(column_id)->second[current_chunk].mutable_data();
                    return &virtdb::util::value_type<T>::get(*value, inner_cursor);
                }
                catch(const ::google::protobuf::FatalException & e)
                {
                    std::string error_string = "Error [" + std::to_string(__LINE__) + "]! Inner_cursor: " + std::to_string(inner_cursor) + " " + e.what();
                    throw std::invalid_argument(error_string);
                }
            }
    };
}
