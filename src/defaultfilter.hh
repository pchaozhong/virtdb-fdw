#pragma once

#include "filter.hh"

namespace virtdb {

class default_filter : public filter
{
public:
  virtual std::shared_ptr<engine::expression> apply(const Expr* clause, const AttInMetadata* meta) override
  {
    // Do not pass limit argument if we can't handle any of the where conditions
    // as in that case data provider would return the limited amount of not well filtered results
    // from which postgres would filter further causing to result less data than it really should.
    // pstate.limit_ = 0;
    ereport(LOG, (errmsg("Not handled node type: %d ", clause->type)));

    return NULL;

    // Not calling further as this filter is always able to handle the clause
    // filter::apply(clause, meta);
  }
};

} // namespace  pgext
