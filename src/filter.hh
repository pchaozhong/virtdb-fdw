#pragma once

// #include <postgres_ext.h>     // Oid
// #include <nodes/primnodes.h>  // Var

#include "postgres_util.hh"

namespace virtdb {

class filter
{
private:
  filter* head;
  filter* next;
private:
  void add(filter* new_filter, filter* head_)
  {
    if (next)
    {
      next->add(new_filter, head_);
    }
    else
    {
      new_filter->head = head_;
      next = new_filter;
    }
  }

public:
  void add(filter* new_filter)
  {
    head = this;
    add(new_filter, this);
  }
  virtual std::shared_ptr<engine::expression> apply(const Expr* clause, const AttInMetadata* meta)
  {
    if (next)
    {
        return next->apply(clause, meta);
    }
    else
    {
        return NULL;
    }
  }

protected:
  filter* get_head() const
  {
    return head;
  }

  size_t get_filter_id(const Expr * clause) const
  {
    size_t filter_id = 9999999;
    const Var * vp   = get_variable(clause);

    if (vp && vp->varattno)
    {
      filter_id       = vp->varattno-1;
    }

    return filter_id;
  }

  std::string get_filter_op(const Oid& oid)
  {
    if (oid >= FirstBootstrapObjectId )
    {
      return "";
    }

    HeapTuple htup = SearchSysCache1(OPEROID, ObjectIdGetDatum(oid));
    if (!HeapTupleIsValid(htup))
    {
      return "";
    }

    Form_pg_operator form = reinterpret_cast<Form_pg_operator>(GETSTRUCT(htup));
    if( !form )
    {
      ReleaseSysCache(htup);
      return "";
    }

    std::string ret(form->oprname.data);
    ReleaseSysCache(htup);

    return ret;

  }

};

} // namespace  pgext
