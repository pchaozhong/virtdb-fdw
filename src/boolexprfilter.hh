#pragma once

#include "filter.hh"

namespace virtdb {

class bool_expr_filter : public filter
{
public:
    virtual std::shared_ptr<engine::expression> apply(const Expr* clause, const AttInMetadata* meta) override
    {
        using virtdb::engine::expression;

        if (!(IsA(clause, BoolExpr)))
        {
            return filter::apply(clause, meta);
        }
        else // Turned off as it is not working yet
        {
            const BoolExpr* bool_expression = reinterpret_cast<const BoolExpr*>(clause);

            // List	   *args;			/* arguments to this expression */
            if (bool_expression->args->length != 2)
            {
                ereport(ERROR, (
                    errmsg("BoolExpression number of arguments is [%d] insted of 2.",
                            bool_expression->args->length)));
            }
            else {
                ListCell* current = bool_expression->args->head;
                std::shared_ptr<expression> left = get_head()->apply((Expr *)current->data.ptr_value, meta);
                std::shared_ptr<expression> right = get_head()->apply((Expr *)current->next->data.ptr_value, meta);
                std::shared_ptr<expression> ret (new expression);
                ret->set_composite(left, right);
                switch (bool_expression->boolop) {
                    case AND_EXPR:
                        ret->set_operand("AND");
                        break;
                    case OR_EXPR:
                        ret->set_operand("OR");
                        break;
                    case NOT_EXPR:
                        ret->set_operand("NOT");
                        break;
                    default:
                        return NULL;
                        break;
                }
                return ret;
            }

            return NULL;
        }
    }
};

} // namespace  pgext
