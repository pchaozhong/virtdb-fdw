#pragma once

#include "filter.hh"
#include "postgres_util.hh"

namespace virtdb {

class op_expr_filter : public filter
{
public:
    virtual std::shared_ptr<engine::expression> apply(const Expr* clause, const AttInMetadata* meta) override
    {
        using virtdb::engine::expression;

        ereport(LOG, (errmsg("Checking in OPEXPR filter")));
        if (!(IsA(clause, OpExpr)))
        {
            return filter::apply(clause, meta);
        }
        else
        {
            const OpExpr * oe = reinterpret_cast<const OpExpr*>(clause);

            size_t filter_id = get_filter_id(clause);
            std::string filter_op = get_filter_op(oe->opno);
            if (filter_op.empty())
            {
                return NULL;
            }

            char * filter_colname  = 0;
            char * filter_val      = 0;

            if( meta &&
                meta->tupdesc &&
                static_cast<size_t>(meta->tupdesc->natts) >= filter_id &&
                meta->tupdesc->attrs &&
                meta->tupdesc->attrs[filter_id] )
            {
                filter_colname  = meta->tupdesc->attrs[filter_id]->attname.data;
            }

            if( filter_colname && (!filter_op.empty()) )
            {
                Node * rop = get_rightop(clause);
                if(!rop || (!IsA(rop,Const)))
                {
                    ereport(LOG, (errmsg("rop && (IsA(rop,Const)")));
                }
                else
                {
                    Const * cp  = reinterpret_cast<Const *>(rop);
                    if( cp->constisnull == false )
                    {
                        Datum cpv = cp->constvalue;
                        char * tmpx = reinterpret_cast<char *>(palloc(50));
                        switch( cp->consttype )
                        {
                            case INT4OID:
                                { snprintf(tmpx,50,"%d",DatumGetInt32(cpv)); filter_val = tmpx; break; }
                            case INT8OID:
                                { snprintf(tmpx,50,"%ld",DatumGetInt64(cpv)); filter_val = tmpx; break; }
                            case FLOAT8OID:
                                { snprintf(tmpx,50,"%20.20f",DatumGetFloat8(cpv)); filter_val = tmpx; break; }
                            case TEXTOID:
                            case BYTEAOID:
                            case CSTRINGOID:
                            case BPCHAROID:
                            case VARCHAROID:
                                { filter_val = TextDatumGetCString(cpv); break; }
                            case NUMERICOID:
                                { filter_val = DatumGetCString(DirectFunctionCall1( numeric_out, cpv )); break; }
                            case DATEOID:
                            {
                                char * tmpdate = DatumGetCString(DirectFunctionCall1( date_out, cpv ));
                                size_t j=0;
                                for( size_t i=0;(i<11 && tmpdate[i]!=0);++i )
                                if( tmpdate[i] >= '0' && tmpdate[i] <= '9' ) tmpx[j++] = tmpdate[i];
                                tmpx[j] = 0;
                                filter_val = tmpx;
                                break;
                            }
                            case TIMEOID:
                            {
                                char * tmptime = DatumGetCString(DirectFunctionCall1( time_out, cpv ));
                                size_t j=0;
                                for( size_t i=0;(i<11 && tmptime[i]!=0);++i )
                                if( tmptime[i] >= '0' && tmptime[i] <= '9' ) tmpx[j++] = tmptime[i];
                                tmpx[j] = 0;
                                filter_val = tmpx;
                                break;
                            }
                            default:
                            {
                                ereport(LOG, (errmsg("\"%s\": OMIT claus expr: %s[%ld] %s %s  OID:%d not handled XXX",
                                    __func__, filter_colname, filter_id, filter_op.c_str(), filter_val, cp->consttype)));
                                break;
                            }
                        };
                    }
                }

                if( filter_val && filter_id != 9999999 )
                {
                    std::shared_ptr<expression> ret (new expression);
                    ret->set_variable(filter_id, filter_colname);
                    ret->set_operand(filter_op);
                    ret->set_value(filter_val);
                    return ret;
                }
            }

            return NULL;
        }
    }
};

} // namespace  pgext
