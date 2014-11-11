#pragma once

#include "filter.hh"

namespace virtdb {

class any_all_filter : public filter
{
public:
    virtual std::shared_ptr<engine::expression>  apply(const Expr* clause, const AttInMetadata* meta) override
    {
        return filter::apply(clause, meta);

        // Handling Any / All expressions is disabled for now, in these cases the condition is not passed to the external datasource
        /*
        if (!(IsA(clause, ScalarArrayOpExpr)))
        {
            Filter::apply(clause, meta);
        }
        else {
            const ScalarArrayOpExpr* array_expression = reinterpret_cast<const ScalarArrayOpExpr*>(clause);
            std::string filter_op = getFilterOp(array_expression->opno);
            ereport(LOG, (errmsg("xpr: %d", array_expression->xpr.type)));
            ereport(LOG, (errmsg("filter_op: %s", filter_op.c_str())));
            ereport(LOG, (errmsg("opfuncid: %d", array_expression->opfuncid)));
            if (array_expression->useOr)
                ereport(LOG, (errmsg("useOr: true")));
            else
                ereport(LOG, (errmsg("useOr: false")));
            ereport(LOG, (errmsg("inputcollid: %d", array_expression->inputcollid)));
            //for (int i = 0; i < array_expression->args->length; i++)
            ereport(LOG, (errmsg("location: %d", array_expression->location)));
        }
        */
    }
};

} // namespace  pgext
