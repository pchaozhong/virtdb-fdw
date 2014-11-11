#pragma once

#include "filter.hh"

namespace virtdb {

class nulltest_filter : public filter
{
public:
    virtual std::shared_ptr<engine::expression> apply(const Expr* clause, const AttInMetadata* meta) override
    {
        using virtdb::engine::expression;

        if (!(IsA(clause, NullTest)))
        {
            return filter::apply(clause, meta);
        }
        else
        {
            const NullTest * null_test  = reinterpret_cast<const NullTest*>(clause);
            size_t filter_id      = get_filter_id(clause);
            char * filter_colname = 0;
            if( meta &&
                meta->tupdesc &&
                static_cast<size_t>(meta->tupdesc->natts) >= filter_id &&
                meta->tupdesc->attrs &&
                meta->tupdesc->attrs[filter_id] )
            {
                filter_colname  = meta->tupdesc->attrs[filter_id]->attname.data;
            }
            std::string filter_op       = "";
            std::string filter_val      = "NULL";
            if (null_test->nulltesttype == NullTestType::IS_NULL)
            {
                filter_op = "IS";
            }
            else
            {
                filter_op = "IS NOT";
            }
            std::shared_ptr<expression> ret (new expression);
            ret->set_variable(filter_id, filter_colname);
            ret->set_operand(filter_op);
            ret->set_value(filter_val);
            return ret;
        }
    }

};




} // namespace  pgext
