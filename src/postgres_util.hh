#pragma once

#include <logger.hh>
#include <vector>

extern "C" {
    #include <nodes/primnodes.h>
    #include <optimizer/clauses.h>
    #include <utils/elog.h>
}

namespace virtdb {

const Var *
get_variable(const Expr* expr,
             std::vector<const Var *> & results)
{
    if( !expr ) return nullptr;
    
    switch (expr->type)
    {
        case T_TargetEntry:
            {
                const TargetEntry* target_entry = reinterpret_cast<const TargetEntry*>(expr);
                return get_variable(target_entry->expr, results);
            }
        case T_OpExpr:
            {
                const OpExpr* opexpr = reinterpret_cast<const OpExpr*>(expr);
                const Expr* lop = reinterpret_cast<const Expr*>(get_leftop(&opexpr->xpr));
                const Expr* rop = reinterpret_cast<const Expr*>(get_rightop(&opexpr->xpr));
                const Var * left_ret = get_variable(lop, results);
                const Var * right_ret = get_variable(rop, results);
                if( left_ret )
                  return left_ret;
                else
                  return right_ret;
            }
        case T_RelabelType:
            {
                const RelabelType* rl = reinterpret_cast<const RelabelType*>(expr);
                const Var* v = reinterpret_cast<const Var*>(rl->arg);
                results.push_back(v);
                return v;
            }
        case T_Var:
            {
                const Var* v = reinterpret_cast<const Var*>(expr);
                results.push_back(v);
                return v;
            }
        case T_Const:
            {
                // constant is not a variable
                return nullptr;
            }
        case T_CoerceViaIO:
            {
                const CoerceViaIO* coerce = reinterpret_cast<const CoerceViaIO*>(expr);
                const Var* v = reinterpret_cast<const Var*>(coerce->arg);
                results.push_back(v);
                return v;
            }
        case T_NullTest:
            {
                const NullTest* null_test = reinterpret_cast<const NullTest*>(expr);
                const Var* v = reinterpret_cast<const Var*>(null_test->arg);
                results.push_back(v);
                return v;
            }
        case T_FuncExpr:
            {
                const FuncExpr* func_expr = reinterpret_cast<const FuncExpr*>(expr);
                {
                  ListCell * l;
                  if(func_expr->args && IsA(func_expr->args, List) )
                  {
                    foreach(l, func_expr->args)
                    {
                      const Expr* extmp = reinterpret_cast<const Expr*>(lfirst(l));
                      if( extmp )
                      {
                        const Var * retvar = get_variable(extmp, results);
                        if( retvar )
                        {
                          return retvar;
                        }
                      }
                    }
                  }
                  return nullptr;
                }
            }
        default:
        {
            LOG_ERROR( "Unhandled node type in get_variable" << V_((int64_t)(expr->type)));
            return nullptr;
        }
    }
    return nullptr;
}

} // namespace virtdb
