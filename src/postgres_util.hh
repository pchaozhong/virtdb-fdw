#pragma once

#include <logger.hh>

extern "C" {
    #include <nodes/primnodes.h>
    #include <optimizer/clauses.h>
    #include <utils/elog.h>
}

namespace virtdb {

std::vector<Var> get_variables(const Expr* expr);

std::vector<Var> get_variables_from_list(List* list)
{
    ListCell *cell;
    std::vector<Var> variables {};
    foreach(cell, list)
    {
        auto new_variables = get_variables(reinterpret_cast<const Expr*>lfirst(cell));
        variables.insert(variables.end(), new_variables.begin(), new_variables.end());
    }    
    return variables;
}    
    
std::vector<Var> get_variables(const Expr* expr)   
{
    switch (expr->type)
    {
        case T_TargetEntry:
            {
                const TargetEntry* target_entry = reinterpret_cast<const TargetEntry*>(expr);
                return { get_variables(target_entry->expr) };
            }
        case T_OpExpr:
            {
                const OpExpr* opexpr = reinterpret_cast<const OpExpr*>(expr);
                const Expr* lop = reinterpret_cast<const Expr*>(get_leftop(&opexpr->xpr));
                return { get_variables(lop) };
            }
        case T_RelabelType:
            {
                const RelabelType* rl = reinterpret_cast<const RelabelType*>(expr);
                return { *reinterpret_cast<const Var*>(rl->arg) };
            }
        case T_Var:
            {
                return { *reinterpret_cast<const Var*>(expr) };
            }
        case T_CoerceViaIO:
            {
                const CoerceViaIO* coerce = reinterpret_cast<const CoerceViaIO*>(expr);
                return { *reinterpret_cast<const Var*>(coerce->arg) };
            }
        case T_NullTest:
            {
                const NullTest* null_test = reinterpret_cast<const NullTest*>(expr);
                return { *reinterpret_cast<const Var*>(null_test->arg) };
            }
        case T_FuncExpr:
            {
                const FuncExpr*  func_expr = reinterpret_cast<const FuncExpr*>(expr);
                return get_variables_from_list(func_expr->args);
            }
        case T_SubPlan:
            {
                const SubPlan* sub_plan = reinterpret_cast<const SubPlan*>(expr);
                return get_variables_from_list(sub_plan->args);
            }    
        default:
        {
            LOG_ERROR( "Unhandled node type in get_variable" << V_((int64_t)(expr->type)));
            return {};
        }
    }
}

} // namespace virtdb
