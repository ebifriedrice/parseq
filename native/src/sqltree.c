/*
** sqltree.c -- parseq's parse-tree serializer, resolve driver, and
** bindings surface for the patched SQLite amalgamation.
**
** Compiled into the same translation unit as sqlite3.c (see
** sqltree_all.c), so it reads SQLite's internal structures directly.
** The parsecov hooks call into this file at the point where a
** statement is fully name-resolved but not yet code-generated; each
** hook walks the tree and serializes it as EDN text that Clojure can
** read with clojure.edn/read-string.
**
** Output conventions (see README for the full node reference):
**   - Every node is an EDN map with a :node keyword.
**   - Every expression carries :tk, the SQLite parser token code
**     name (e.g. "TK_PLUS"), so unknown ops are never ambiguous.
**   - Every statement envelope carries :sql (raw text) and :explain.
*/
#include "sqltree_opnames.h"   /* generated: TK_ code -> name table */

#ifndef SQT_API
# define SQT_API __attribute__((visibility("default")))
#endif

/* ------------------------------------------------------------------ */
/* parse-capture context                                              */
/* ------------------------------------------------------------------ */

typedef struct SqtSession {
  sqlite3 *db;
} SqtSession;

typedef struct SqtCtx {
  Parse *pParse;
  sqlite3_str *out;         /* completed statement envelopes */
  sqlite3_str *node;        /* node under construction */
  const char *zSql;         /* whole input text */
  const char *zStmtStart;   /* start of current statement */
  int hasNode;              /* a verb hook dumped the current statement */
  int depth;                /* tree recursion guard */
} SqtCtx;

static SqtCtx *g_ctx = 0;

/* True when parse capture owns this Parse object. Used by every hook;
** outside sqt_parse() this is false and hooks are inert. */
static SqtCtx *sqtCtx(Parse *pParse){
  SqtCtx *c = g_ctx;
  if( c==0 || c->pParse!=pParse ) return 0;
  if( pParse->nested ) return 0;
  return c;
}

/* ------------------------------------------------------------------ */
/* EDN emission helpers                                               */
/* ------------------------------------------------------------------ */

static void sqtStrN(sqlite3_str *p, const char *z, int n){
  int i;
  sqlite3_str_appendchar(p, 1, '"');
  for(i=0; i<n && z[i]; i++){
    unsigned char ch = (unsigned char)z[i];
    switch( ch ){
      case '"':  sqlite3_str_appendall(p, "\\\"");   break;
      case '\\': sqlite3_str_appendall(p, "\\\\");   break;
      case '\n': sqlite3_str_appendall(p, "\\n");    break;
      case '\r': sqlite3_str_appendall(p, "\\r");    break;
      case '\t': sqlite3_str_appendall(p, "\\t");    break;
      case '\b': sqlite3_str_appendall(p, "\\b");    break;
      case '\f': sqlite3_str_appendall(p, "\\f");    break;
      default:
        if( ch<0x20 ){
          sqlite3_str_appendf(p, "\\u%04x", ch);
        }else{
          sqlite3_str_appendchar(p, 1, (char)ch);
        }
    }
  }
  sqlite3_str_appendchar(p, 1, '"');
}
static void sqtStr(sqlite3_str *p, const char *z){
  if( z==0 ){ sqlite3_str_appendall(p, "nil"); }
  else      { sqtStrN(p, z, 0x7ffffff); }
}
static void sqtTok(sqlite3_str *p, const Token *t){
  if( t==0 || t->z==0 ){ sqlite3_str_appendall(p, "nil"); return; }
  sqtStrN(p, t->z, (int)t->n);
}
static void sqtKw(sqlite3_str *p, const char *z){
  sqlite3_str_appendchar(p, 1, ':');
  sqlite3_str_appendall(p, z ? z : "nil");
}
static void sqtInt(sqlite3_str *p, sqlite3_int64 v){
  sqlite3_str_appendf(p, "%lld", v);
}
static void sqtBool(sqlite3_str *p, int b){
  sqlite3_str_appendall(p, b ? "true" : "false");
}
/* key emitters: write ":key " before the upcoming value */
static void sqtKey(sqlite3_str *p, const char *k){
  sqlite3_str_appendchar(p, 1, ' ');
  sqtKw(p, k);
  sqlite3_str_appendchar(p, 1, ' ');
}
static void sqtKStr(sqlite3_str *p, const char *k, const char *z){
  sqtKey(p, k); sqtStr(p, z);
}
static void sqtKTok(sqlite3_str *p, const char *k, const Token *t){
  sqtKey(p, k); sqtTok(p, t);
}
static void sqtKKw(sqlite3_str *p, const char *k, const char *z){
  sqtKey(p, k); sqtKw(p, z);
}
static void sqtKInt(sqlite3_str *p, const char *k, sqlite3_int64 v){
  sqtKey(p, k); sqtInt(p, v);
}
static void sqtKBool(sqlite3_str *p, const char *k, int b){
  sqtKey(p, k); sqtBool(p, b);
}
static void sqtVec(sqlite3_str *p){ sqlite3_str_appendall(p, " ["); }
static void sqtVecEnd(sqlite3_str *p){ sqlite3_str_appendchar(p, 1, ']'); }
static void sqtMap(sqlite3_str *p){ sqlite3_str_appendall(p, "{"); }
static void sqtMapEnd(sqlite3_str *p){ sqlite3_str_appendchar(p, 1, '}'); }
static void sqtSep(sqlite3_str *p){ sqlite3_str_appendchar(p, 1, ' '); }

/* token-code name, e.g. TK_PLUS -> "TK_PLUS"; never NULL */
static const char *sqtTkName(int tk){
  if( tk>0 && (u32)tk<ArraySize(aTkName) && aTkName[tk] ) return aTkName[tk];
  return "TK_?";
}

/* ------------------------------------------------------------------ */
/* shared semantic mappers                                            */
/* ------------------------------------------------------------------ */

static const char *sqtAffKw(char aff){
  switch( aff ){
    case SQLITE_AFF_BLOB:    return "blob";
    case SQLITE_AFF_TEXT:    return "text";
    case SQLITE_AFF_NUMERIC: return "numeric";
    case SQLITE_AFF_INTEGER: return "integer";
    case SQLITE_AFF_REAL:    return "real";
    case SQLITE_AFF_FLEXNUM: return "flexnum";
    default:                 return "none";
  }
}

static const char *sqtOnConfKw(int oe){
  switch( oe ){
    case OE_Rollback: return "rollback";
    case OE_Abort:    return "abort";
    case OE_Fail:     return "fail";
    case OE_Ignore:   return "ignore";
    case OE_Replace:  return "replace";
    case OE_Update:   return "update";
    case OE_Restrict: return "restrict";
    case OE_SetNull:  return "set-null";
    case OE_SetDflt:  return "set-default";
    case OE_Cascade:  return "cascade";
    case OE_Default:  return "default";
    default:          return "none";
  }
}

/* join-type bits emitted as [:left :outer] etc; plain comma join is [] */
static void sqtJoinType(sqlite3_str *o, u8 jt){
  int n = 0;
  sqlite3_str_appendall(o, " [");
  if( jt & JT_LEFT )  { if(n++)sqtSep(o); sqlite3_str_appendall(o,":left"); }
  if( jt & JT_RIGHT ) { if(n++)sqtSep(o); sqlite3_str_appendall(o,":right"); }
  if( jt & JT_INNER ) { if(n++)sqtSep(o); sqlite3_str_appendall(o,":inner"); }
  if( jt & JT_CROSS ) { if(n++)sqtSep(o); sqlite3_str_appendall(o,":cross"); }
  if( jt & JT_NATURAL){ if(n++)sqtSep(o); sqlite3_str_appendall(o,":natural"); }
  if( jt & JT_OUTER ) { if(n++)sqtSep(o); sqlite3_str_appendall(o,":outer"); }
  sqlite3_str_appendchar(o, 1, ']');
}

static const char *sqtSchemaDbName(sqlite3 *db, Schema *pSchema){
  int i = sqlite3SchemaToIndex(db, pSchema);
  if( i<0 ) return 0;
  return db->aDb[i].zDbSName;
}

/* ------------------------------------------------------------------ */
/* forward declarations                                               */
/* ------------------------------------------------------------------ */

static void sqtExpr(SqtCtx *c, const Expr *p);
static void sqtExprList(SqtCtx *c, const ExprList *p);
static void sqtSelect(SqtCtx *c, const Select *p);
static void sqtTableDef(SqtCtx *c, const Table *p);
struct SqtStepView;
static void sqtDumpTriggerBody(SqtCtx*, const Trigger*, const Table*,
                               const TriggerStep*, const struct SqtStepView*,
                               int, const Expr*);
#ifndef SQLITE_OMIT_WINDOWFUNC
static void sqtWindowFn(SqtCtx *c, const Window *p);
#endif

/* ------------------------------------------------------------------ */
/* expression dumper                                                  */
/* ------------------------------------------------------------------ */

/* binary/unary op token code -> short keyword, used for :op */
static const char *sqtOpKw(int op){
  switch( op ){
    case TK_AND:       return "and";
    case TK_OR:        return "or";
    case TK_PLUS:      return "+";
    case TK_MINUS:     return "-";
    case TK_STAR:      return "*";
    case TK_SLASH:     return "/";
    case TK_REM:       return "%";
    case TK_CONCAT:    return "||";
    case TK_BITAND:    return "&";
    case TK_BITOR:     return "|";
    case TK_LSHIFT:    return "<<";
    case TK_RSHIFT:    return ">>";
    case TK_LT:        return "<";
    case TK_LE:        return "<=";
    case TK_GT:        return ">";
    case TK_GE:        return ">=";
    case TK_EQ:        return "=";
    case TK_NE:        return "!=";
    case TK_IS:        return "is";
    case TK_ISNOT:     return "is-not";
    case TK_ISNULL:    return "is-null";
    case TK_NOTNULL:   return "not-null";
    case TK_NOT:       return "not";
    case TK_BITNOT:    return "bitnot";
    case TK_UMINUS:    return "u-";
    case TK_UPLUS:     return "u+";
    default:           return 0;
  }
}

static void sqtExpr(SqtCtx *c, const Expr *p){
  sqlite3_str *o = c->node;
  if( p==0 ){ sqlite3_str_appendall(o, "nil"); return; }
  if( c->depth>400 ){ sqlite3_str_appendall(o, "\"<too-deep>\""); return; }
  c->depth++;
  sqtMap(o);
  sqtKKw(o, "tk", sqtTkName(p->op));

  switch( p->op ){
    case TK_COLUMN: {
      const Table *pTab = ExprUseYTab(p) ? p->y.pTab : 0;
      sqtKKw(o, "node", "column");
      sqtKInt(o, "cursor", p->iTable);
      if( ExprHasProperty(p, EP_FixedCol) ) sqtKBool(o, "fixed", 1);
      if( ExprHasProperty(p, EP_OuterON|EP_InnerON) ){
        sqtKKw(o, "on-join",
               ExprHasProperty(p, EP_OuterON) ? "outer" : "inner");
      }
      if( pTab ){
        sqtKStr(o, "table", pTab->zName);
        if( pTab->pSchema && c->pParse->db ){
          const char *zDb = sqtSchemaDbName(c->pParse->db, pTab->pSchema);
          if( zDb ) sqtKStr(o, "schema", zDb);
        }
        if( p->iColumn<0 ){
          sqtKKw(o, "name", "rowid");
          sqtKInt(o, "idx", -1);
        }else if( p->iColumn<pTab->nCol ){
          const Column *pCol = &pTab->aCol[p->iColumn];
          sqtKStr(o, "name", pCol->zCnName);
          sqtKInt(o, "idx", p->iColumn);
          sqtKKw(o, "affinity", sqtAffKw(pCol->affinity));
          if( pCol->colFlags & COLFLAG_HIDDEN ) sqtKBool(o, "hidden", 1);
        }else{
          sqtKInt(o, "idx", p->iColumn);
        }
      }else{
        sqtKInt(o, "idx", p->iColumn);
      }
      break;
    }
    case TK_AGG_COLUMN: {
      sqtKKw(o, "node", "agg-column");
      if( ExprHasProperty(p, EP_Reduced)==0 && p->pAggInfo
       && p->iAgg>=0 && p->iAgg<p->pAggInfo->nColumn
      ){
        const struct AggInfo_col *pc = &p->pAggInfo->aCol[p->iAgg];
        sqtKey(o, "expr"); c->depth--; sqtExpr(c, pc->pCExpr); c->depth++;
      }else{
        sqtKInt(o, "idx", p->iColumn);
      }
      break;
    }
    case TK_INTEGER:
    case TK_FLOAT:
    case TK_STRING:
    case TK_BLOB: {
      sqtKKw(o, "node", "literal");
      sqtKKw(o, "kind",
             p->op==TK_INTEGER ? "integer" :
             p->op==TK_FLOAT   ? "float"   :
             p->op==TK_STRING  ? "string"  : "blob");
      if( ExprUseUToken(p) && p->u.zToken ) sqtKStr(o, "value", p->u.zToken);
      if( ExprUseUValue(p) ) sqtKInt(o, "int-value", p->u.iValue);
      break;
    }
    case TK_TRUEFALSE: {
      sqtKKw(o, "node", "literal");
      sqtKKw(o, "kind", "boolean");
      sqtKBool(o, "value", ExprHasProperty(p, EP_IsTrue)!=0);
      break;
    }
    case TK_NULL: {
      sqtKKw(o, "node", "literal");
      sqtKKw(o, "kind", "null");
      break;
    }
    case TK_VARIABLE: {
      sqtKKw(o, "node", "parameter");
      sqtKInt(o, "index", p->iColumn);
      if( ExprUseUToken(p) && p->u.zToken ) sqtKStr(o, "name", p->u.zToken);
      break;
    }
    case TK_ID: {
      sqtKKw(o, "node", "id");
      if( ExprUseUToken(p) ) sqtKStr(o, "name", p->u.zToken);
      sqtKBool(o, "resolved", 0);
      break;
    }
    case TK_DOT: {
      sqtKKw(o, "node", "dot");
      sqtKBool(o, "resolved", 0);
      if( !ExprHasProperty(p, EP_TokenOnly) ){
        sqtKey(o, "left");  sqtExpr(c, p->pLeft);
        sqtKey(o, "right"); sqtExpr(c, p->pRight);
      }
      break;
    }
    case TK_FUNCTION:
    case TK_AGG_FUNCTION: {
      sqtKKw(o, "node", p->op==TK_FUNCTION ? "function" : "agg-function");
      if( ExprUseUToken(p) ) sqtKStr(o, "name", p->u.zToken);
      if( ExprHasProperty(p, EP_Distinct) ) sqtKBool(o, "distinct", 1);
      if( p->pLeft && p->pLeft->op==TK_ORDER ){
        /* aggregate ORDER BY suffix: f(x ORDER BY y) */
        ExprList *pl = ExprUseXList(p->pLeft) ? p->pLeft->x.pList : 0;
        int i;
        sqtKey(o, "order-by"); sqtVec(o);
        if( pl ){
          for(i=0;i<pl->nExpr;i++){ if(i) sqtSep(o); sqtExpr(c, pl->a[i].pExpr); }
        }
        sqtVecEnd(o);
      }
      if( ExprUseXList(p) && p->x.pList ){
        sqtKey(o, "args"); sqtVec(o);
        sqtExprList(c, p->x.pList);
        sqtVecEnd(o);
      }else{
        sqtKey(o, "args"); sqlite3_str_appendall(o, " []");
      }
#ifndef SQLITE_OMIT_WINDOWFUNC
      if( ExprUseYWin(p) && p->y.pWin ){
        if( p->y.pWin->eFrmType==TK_FILTER ){
          sqtKey(o, "filter"); sqtExpr(c, p->y.pWin->pFilter);
        }else{
          sqtKey(o, "window"); c->depth--; sqtWindowFn(c, p->y.pWin); c->depth++;
        }
      }
#endif
      break;
    }
    case TK_SELECT: {         /* scalar subquery */
      sqtKKw(o, "node", "scalar-subquery");
      if( ExprUseXSelect(p) ){
        sqtKey(o, "select"); c->depth--; sqtSelect(c, p->x.pSelect); c->depth++;
        if( ExprHasProperty(p, EP_VarSelect) ) sqtKBool(o, "correlated", 1);
      }
      break;
    }
    case TK_EXISTS: {
      sqtKKw(o, "node", "exists");
      if( ExprUseXSelect(p) ){
        sqtKey(o, "select"); c->depth--; sqtSelect(c, p->x.pSelect); c->depth++;
        if( ExprHasProperty(p, EP_VarSelect) ) sqtKBool(o, "correlated", 1);
      }
      break;
    }
    case TK_IN: {
      sqtKKw(o, "node", "in");
      if( !ExprHasProperty(p, EP_TokenOnly) ){
        sqtKey(o, "lhs"); sqtExpr(c, p->pLeft);
      }
      if( ExprUseXSelect(p) ){
        sqtKKw(o, "rhs", "select");
        sqtKey(o, "select"); c->depth--; sqtSelect(c, p->x.pSelect); c->depth++;
      }else if( ExprUseXList(p) && p->x.pList ){
        sqtKKw(o, "rhs", "list");
        sqtKey(o, "values"); sqtVec(o);
        sqtExprList(c, p->x.pList);
        sqtVecEnd(o);
      }else{
        sqtKKw(o, "rhs", "empty");
      }
      break;
    }
    case TK_BETWEEN: {
      ExprList *pl = ExprUseXList(p) ? p->x.pList : 0;
      sqtKKw(o, "node", "between");
      if( !ExprHasProperty(p, EP_TokenOnly) ){
        sqtKey(o, "operand"); sqtExpr(c, p->pLeft);
      }
      if( pl && pl->nExpr==2 ){
        sqtKey(o, "low");  sqtExpr(c, pl->a[0].pExpr);
        sqtKey(o, "high"); sqtExpr(c, pl->a[1].pExpr);
      }
      break;
    }
    case TK_CASE: {
      ExprList *pl = ExprUseXList(p) ? p->x.pList : 0;
      sqtKKw(o, "node", "case");
      if( !ExprHasProperty(p, EP_TokenOnly) && p->pLeft ){
        sqtKey(o, "operand"); sqtExpr(c, p->pLeft);
      }
      if( pl && pl->nExpr>=2 ){
        int i;
        int n = pl->nExpr;
        int hasElse = (n % 2)==1;
        sqtKey(o, "whens"); sqtVec(o);
        for(i=0; i+1<n; i+=2){
          if( i ) sqtSep(o);
          sqtMap(o); sqtKKw(o,"node","when");
          sqtKey(o,"when"); sqtExpr(c, pl->a[i].pExpr);
          sqtKey(o,"then"); sqtExpr(c, pl->a[i+1].pExpr);
          sqtMapEnd(o);
        }
        sqtVecEnd(o);
        if( hasElse ){
          sqtKey(o, "else"); sqtExpr(c, pl->a[n-1].pExpr);
        }
      }
      break;
    }
    case TK_CAST: {
      sqtKKw(o, "node", "cast");
      if( ExprUseUToken(p) ) sqtKStr(o, "type", p->u.zToken);
      if( !ExprHasProperty(p, EP_TokenOnly) && p->pLeft ){
        sqtKey(o, "operand"); sqtExpr(c, p->pLeft);
      }
      break;
    }
    case TK_COLLATE: {
      sqtKKw(o, "node", "collate");
      if( ExprUseUToken(p) ) sqtKStr(o, "collation", p->u.zToken);
      if( !ExprHasProperty(p, EP_TokenOnly) && p->pLeft ){
        sqtKey(o, "operand"); sqtExpr(c, p->pLeft);
      }
      break;
    }
    case TK_VECTOR: {
      sqtKKw(o, "node", "vector");
      if( ExprUseXList(p) && p->x.pList ){
        sqtKey(o, "items"); sqtVec(o);
        sqtExprList(c, p->x.pList);
        sqtVecEnd(o);
      }
      break;
    }
    case TK_SELECT_COLUMN: {
      /* element of a row vector projected from a scalar subquery */
      sqtKKw(o, "node", "select-column");
      sqtKInt(o, "column", p->iColumn);
      sqtKInt(o, "vector-size", p->iTable);
      break;
    }
    case TK_TRUTH: {
      sqtKKw(o, "node", "truth");
      sqtKKw(o, "op", p->op2==TK_IS ? "is" : "is-not");
      if( !ExprHasProperty(p, EP_TokenOnly) ){
        if( p->pLeft ){ sqtKey(o, "lhs"); sqtExpr(c, p->pLeft); }
        if( p->pRight){ sqtKey(o, "rhs"); sqtExpr(c, p->pRight); }
      }
      break;
    }
    case TK_RAISE: {
      sqtKKw(o, "node", "raise");
      if( ExprUseUToken(p) ) sqtKStr(o, "action", p->u.zToken);
      if( !ExprHasProperty(p, EP_TokenOnly) && p->pLeft ){
        sqtKey(o, "message"); sqtExpr(c, p->pLeft);
      }
      break;
    }
    case TK_TRIGGER: {   /* NEW.x / OLD.x pseudo-row reference */
      sqtKKw(o, "node", "trigger-column");
      sqtKKw(o, "row", p->iTable!=0 ? "new" : "old");
      sqtKInt(o, "idx", p->iColumn);
      /* u.zToken is not populated for this op; recover the column name
      ** from the trigger table when the parse context carries it */
      if( p->iColumn>=0 && c->pParse->pTriggerTab
       && p->iColumn<c->pParse->pTriggerTab->nCol ){
        sqtKStr(o, "name", c->pParse->pTriggerTab->aCol[p->iColumn].zCnName);
      }else if( p->iColumn<0 ){
        sqtKKw(o, "name", "rowid");
      }
      break;
    }
    case TK_ROW: {
      sqtKKw(o, "node", "row");
      break;
    }
    case TK_SPAN: {     /* DEFAULT/generated wrapper: source text + expr */
      sqtKKw(o, "node", "span");
      if( ExprUseUToken(p) && p->u.zToken ) sqtKStr(o, "text", p->u.zToken);
      if( !ExprHasProperty(p, EP_TokenOnly) && p->pLeft ){
        sqtKey(o, "expr"); sqtExpr(c, p->pLeft);
      }
      break;
    }
    case TK_ORDER: {    /* ORDER BY suffix inside aggregate args */
      sqtKKw(o, "node", "order-suffix");
      if( ExprUseXList(p) && p->x.pList ){
        sqtKey(o, "items"); sqtVec(o);
        sqtExprList(c, p->x.pList);
        sqtVecEnd(o);
      }
      break;
    }
    case TK_LIMIT: {    /* internal: pLeft=limit pRight=offset */
      sqtKKw(o, "node", "limit");
      if( !ExprHasProperty(p, EP_TokenOnly) ){
        if( p->pLeft ){ sqtKey(o, "limit");  sqtExpr(c, p->pLeft); }
        if( p->pRight){ sqtKey(o, "offset"); sqtExpr(c, p->pRight); }
      }
      break;
    }
    default: {
      const char *opKw = sqtOpKw(p->op);
      int unary = ( p->op==TK_NOT || p->op==TK_BITNOT || p->op==TK_UMINUS
                 || p->op==TK_UPLUS || p->op==TK_ISNULL || p->op==TK_NOTNULL );
      if( opKw && !unary ){
        /* binary operator */
        sqtKKw(o, "node", "binop");
        sqtKKw(o, "op", opKw);
        if( !ExprHasProperty(p, EP_TokenOnly) ){
          sqtKey(o, "lhs"); sqtExpr(c, p->pLeft);
          sqtKey(o, "rhs"); sqtExpr(c, p->pRight);
        }
      }else if( opKw ){
        sqtKKw(o, "node", "unop");
        sqtKKw(o, "op", opKw);
        if( !ExprHasProperty(p, EP_TokenOnly) && p->pLeft ){
          sqtKey(o, "operand"); sqtExpr(c, p->pLeft);
        }
      }else{
        /* total-coverage fallback: shape + children, never ambiguous */
        sqtKKw(o, "node", "expr");
        if( ExprUseUToken(p) && p->u.zToken ) sqtKStr(o, "token", p->u.zToken);
        if( !ExprHasProperty(p, EP_TokenOnly) ){
          if( p->pLeft ){ sqtKey(o, "left");  sqtExpr(c, p->pLeft); }
          if( p->pRight){ sqtKey(o, "right"); sqtExpr(c, p->pRight); }
        }
        if( ExprUseXSelect(p) && p->x.pSelect ){
          sqtKey(o, "select"); c->depth--; sqtSelect(c, p->x.pSelect); c->depth++;
        }else if( ExprUseXList(p) && p->x.pList ){
          sqtKey(o, "list"); sqtVec(o);
          sqtExprList(c, p->x.pList);
          sqtVecEnd(o);
        }
      }
    }
  }
  /* token span offset, when available (w.iOfst collides with join
  ** references and reduced nodes, both excluded here) */
  if( ExprHasProperty(p, EP_Reduced|EP_TokenOnly)==0
   && ExprHasProperty(p, EP_InnerON|EP_OuterON)==0
   && ExprUseWOfst(p) && p->w.iOfst>0 ){
    sqtKInt(o, "ofst", p->w.iOfst);
  }
  sqtMapEnd(o);
  c->depth--;
}

/* an ExprList as a sequence of {:expr ... :name ... :sort ...} items */
static void sqtExprListItem(SqtCtx *c, const struct ExprList_item *it){
  sqlite3_str *o = c->node;
  sqtMap(o);
  sqtKey(o, "expr"); sqtExpr(c, it->pExpr);
  if( it->zEName && it->fg.eEName==ENAME_NAME ) sqtKStr(o, "name", it->zEName);
  if( it->zEName && it->fg.eEName==ENAME_SPAN ) sqtKStr(o, "span", it->zEName);
  if( it->zEName && it->fg.eEName==ENAME_TAB  ) sqtKStr(o, "qualifier", it->zEName);
  if( it->fg.sortFlags & KEYINFO_ORDER_DESC ){
    sqtKKw(o, "dir", "desc");
  }else if( it->fg.sortFlags ){
    sqtKKw(o, "dir", "asc");
  }
  if( it->fg.bNulls ){
    sqtKKw(o, "nulls",
           (it->fg.sortFlags & KEYINFO_ORDER_BIGNULL) ? "first" : "last");
  }
  if( it->u.x.iOrderByCol ){
    sqtKInt(o, "order-by-col", it->u.x.iOrderByCol);
  }
  if( it->fg.bUsingTerm ) sqtKBool(o, "from-using", 1);
  if( it->fg.bNoExpand  ) sqtKBool(o, "no-expand", 1);
  sqtMapEnd(o);
}

static void sqtExprList(SqtCtx *c, const ExprList *p){
  sqlite3_str *o = c->node;
  int i;
  if( p==0 ) return;
  for(i=0;i<p->nExpr;i++){
    if(i) sqtSep(o);
    sqtExprListItem(c, &p->a[i]);
  }
}

/* ------------------------------------------------------------------ */
/* window definitions                                                 */
/* ------------------------------------------------------------------ */

#ifndef SQLITE_OMIT_WINDOWFUNC
static const char *sqtFrameBoundKw(int b){
  switch( b ){
    case TK_UNBOUNDED: return "unbounded";
    case TK_CURRENT:   return "current";
    default:           return "expr";   /* PRECEDING/FOLLOWING with expr */
  }
}
static void sqtWindowFn(SqtCtx *c, const Window *p){
  sqlite3_str *o = c->node;
  if( p==0 ){ sqlite3_str_appendall(o, "nil"); return; }
  sqtMap(o);
  sqtKKw(o, "node", "window");
  if( p->zName ) sqtKStr(o, "name", p->zName);
  if( p->zBase ) sqtKStr(o, "base", p->zBase);
  if( p->pPartition ){
    sqtKey(o, "partition-by"); sqtVec(o);
    sqtExprList(c, p->pPartition);
    sqtVecEnd(o);
  }
  if( p->pOrderBy ){
    sqtKey(o, "order-by"); sqtVec(o);
    sqtExprList(c, p->pOrderBy);
    sqtVecEnd(o);
  }
  if( p->eFrmType ){
    const char *frm =
      p->eFrmType==TK_ROWS   ? "rows"   :
      p->eFrmType==TK_RANGE  ? "range"  :
      p->eFrmType==TK_GROUPS ? "groups" : "filter";
    sqtKKw(o, "frame", frm);
    sqtKey(o, "bounds"); sqtMap(o);
    sqtKKw(o, "start", sqtFrameBoundKw(p->eStart));
    if( p->eStart==TK_PRECEDING || p->eStart==TK_FOLLOWING ){
      sqtKKw(o, "start-dir",
             p->eStart==TK_PRECEDING ? "preceding" : "following");
      if( p->pStart ){ sqtKey(o, "start-expr"); sqtExpr(c, p->pStart); }
    }
    sqtKKw(o, "end", sqtFrameBoundKw(p->eEnd));
    if( p->eEnd==TK_PRECEDING || p->eEnd==TK_FOLLOWING ){
      sqtKKw(o, "end-dir",
             p->eEnd==TK_PRECEDING ? "preceding" : "following");
      if( p->pEnd ){ sqtKey(o, "end-expr"); sqtExpr(c, p->pEnd); }
    }
    sqtMapEnd(o);
    if( p->eExclude ){
      const char *ex =
        p->eExclude==TK_NO      ? "no-others" :
        p->eExclude==TK_CURRENT ? "current"   :
        p->eExclude==TK_GROUP   ? "group"     : "ties";
      sqtKKw(o, "exclude", ex);
    }
    if( p->bImplicitFrame ) sqtKBool(o, "implicit-frame", 1);
  }
  if( p->pFilter ){ sqtKey(o, "filter"); sqtExpr(c, p->pFilter); }
  if( p->pWFunc && p->pWFunc->zName ) sqtKStr(o, "function", p->pWFunc->zName);
  sqtMapEnd(o);
}
#endif

/* ------------------------------------------------------------------ */
/* CTE (WITH)                                                         */
/* ------------------------------------------------------------------ */

static void sqtWith(SqtCtx *c, const With *p){
  sqlite3_str *o = c->node;
  int i;
  if( p==0 ) return;
  sqtKey(o, "ctes"); sqtVec(o);
  for(i=0;i<p->nCte;i++){
    const Cte *pCte = &p->a[i];
    if(i) sqtSep(o);
    sqtMap(o);
    sqtKKw(o, "node", "cte");
    sqtKStr(o, "name", pCte->zName);
    if( pCte->eM10d==M10d_Yes ) sqtKKw(o, "materialized", "yes");
    if( pCte->eM10d==M10d_No  ) sqtKKw(o, "materialized", "no");
    if( pCte->pCols ){
      int j;
      sqtKey(o, "columns"); sqtVec(o);
      for(j=0;j<pCte->pCols->nExpr;j++){
        if(j) sqtSep(o);
        sqtStr(o, pCte->pCols->a[j].zEName);
      }
      sqtVecEnd(o);
    }
    if( pCte->pSelect ){
      if( pCte->pSelect->selFlags & SF_Recursive ) sqtKBool(o, "recursive", 1);
      sqtKey(o, "query"); c->depth--; sqtSelect(c, pCte->pSelect); c->depth++;
    }
    sqtMapEnd(o);
  }
  sqtVecEnd(o);
}

/* ------------------------------------------------------------------ */
/* FROM clause (SrcList)                                              */
/* ------------------------------------------------------------------ */

/* resolved column list of a table; used for FROM items and targets */
static void sqtColumnsOf(SqtCtx *c, const Table *pTab, int skipNoExpand){
  sqlite3_str *o = c->node;
  int i;
  sqtKey(o, "columns"); sqtVec(o);
  for(i=0;i<pTab->nCol;i++){
    const Column *pCol = &pTab->aCol[i];
    if( skipNoExpand && (pCol->colFlags & COLFLAG_NOEXPAND)!=0 ) continue;
    if( i ) sqtSep(o);
    sqtMap(o);
    sqtKStr(o, "name", pCol->zCnName);
    sqtKInt(o, "idx", i);
    {
      const char *zType = sqlite3ColumnType((Column*)pCol, 0);
      if( zType && zType[0] ) sqtKStr(o, "type", zType);
      sqtKKw(o, "affinity", sqtAffKw(pCol->affinity));
    }
    if( pCol->notNull!=OE_None ) sqtKKw(o, "not-null", sqtOnConfKw(pCol->notNull));
    if( pCol->colFlags & COLFLAG_PRIMKEY ) sqtKBool(o, "pk", 1);
    if( pCol->colFlags & COLFLAG_HIDDEN )  sqtKBool(o, "hidden", 1);
    if( pCol->colFlags & COLFLAG_VIRTUAL ) sqtKKw(o, "generated", "virtual");
    if( pCol->colFlags & COLFLAG_STORED  ) sqtKKw(o, "generated", "stored");
    {
      const char *zColl = sqlite3ColumnColl((Column*)pCol);
      if( zColl && sqlite3StrICmp(zColl,"BINARY")!=0 ){
        sqtKStr(o, "collation", zColl);
      }
    }
    {
      Expr *pDflt = sqlite3ColumnExpr((Table*)pTab, (Column*)pCol);
      if( pDflt ){
        if( pCol->colFlags & COLFLAG_GENERATED ){
          sqtKey(o, "as-expr"); sqtExpr(c, pDflt);
        }else{
          sqtKey(o, "default"); sqtExpr(c, pDflt);
        }
      }
    }
    sqtMapEnd(o);
  }
  sqtVecEnd(o);
}

static void sqtSrcItem(SqtCtx *c, const SrcItem *pItem, int iItem){
  sqlite3_str *o = c->node;
  sqtMap(o);
  if( pItem->fg.isSubquery ){
    sqtKKw(o, "node", "subquery");
    if( pItem->zName ) sqtKStr(o, "name", pItem->zName);
    if( pItem->zAlias ) sqtKStr(o, "alias", pItem->zAlias);
    sqtKInt(o, "cursor", pItem->iCursor);
    if( pItem->u4.pSubq && pItem->u4.pSubq->pSelect ){
      const Select *ps = pItem->u4.pSubq->pSelect;
      if( ps->selFlags & SF_View ) sqtKBool(o, "view", 1);
      if( pItem->fg.isCte ) sqtKBool(o, "cte", 1);
      if( pItem->fg.isMaterialized ) sqtKBool(o, "materialized", 1);
      if( pItem->fg.isCorrelated ) sqtKBool(o, "correlated", 1);
      if( pItem->pSTab && pItem->pSTab->zName ){
        sqtKStr(o, "table", pItem->pSTab->zName);
      }
      if( pItem->pSTab ) sqtColumnsOf(c, pItem->pSTab, 1);
      sqtKey(o, "select"); c->depth--; sqtSelect(c, ps); c->depth++;
    }
  }else if( pItem->fg.isTabFunc ){
    sqtKKw(o, "node", "table-function");
    if( pItem->zName ) sqtKStr(o, "name", pItem->zName);
    if( pItem->zAlias ) sqtKStr(o, "alias", pItem->zAlias);
    sqtKInt(o, "cursor", pItem->iCursor);
    if( pItem->u1.pFuncArg ){
      sqtKey(o, "args"); sqtVec(o);
      sqtExprList(c, pItem->u1.pFuncArg);
      sqtVecEnd(o);
    }
  }else{
    sqtKKw(o, "node", "table");
    if( pItem->u4.zDatabase && pItem->u4.zDatabase[0] ){
      sqtKStr(o, "schema", pItem->u4.zDatabase);
    }
    if( pItem->zName ) sqtKStr(o, "name", pItem->zName);
    if( pItem->zAlias ) sqtKStr(o, "alias", pItem->zAlias);
    sqtKInt(o, "cursor", pItem->iCursor);
    if( pItem->fg.isCte ) sqtKBool(o, "cte", 1);
    if( pItem->pSTab ){
      if( IsVirtual(pItem->pSTab) ) sqtKBool(o, "virtual-table", 1);
      if( pItem->pSTab->pSchema && c->pParse->db ){
        const char *zDb = sqtSchemaDbName(c->pParse->db, pItem->pSTab->pSchema);
        if( zDb ) sqtKStr(o, "schema", zDb);
      }
      sqtColumnsOf(c, pItem->pSTab, 1);
    }
    if( pItem->fg.notIndexed ) sqtKBool(o, "not-indexed", 1);
    if( pItem->fg.isIndexedBy && pItem->u1.zIndexedBy ){
      sqtKStr(o, "indexed-by", pItem->u1.zIndexedBy);
    }
  }
  /* join of this item to the previous one (post-shift convention) */
  if( iItem>0 && pItem->fg.jointype ){
    sqtKey(o, "join"); sqtMap(o);
    sqtKey(o, "type"); sqtJoinType(o, pItem->fg.jointype);
    if( pItem->fg.isUsing && pItem->u3.pUsing ){
      int j;
      sqtKey(o, "using"); sqtVec(o);
      for(j=0;j<pItem->u3.pUsing->nId;j++){
        if(j) sqtSep(o);
        sqtStr(o, pItem->u3.pUsing->a[j].zName);
      }
      sqtVecEnd(o);
    }else if( pItem->u3.pOn ){
      sqtKey(o, "on"); sqtExpr(c, pItem->u3.pOn);
    }
    if( pItem->fg.isSynthUsing ) sqtKBool(o, "natural-expanded", 1);
    sqtMapEnd(o);
  }
  sqtMapEnd(o);
}

static void sqtSrcList(SqtCtx *c, const SrcList *p){
  sqlite3_str *o = c->node;
  int i;
  if( p==0 ) return;
  for(i=0;i<p->nSrc;i++){
    if(i) sqtSep(o);
    sqtSrcItem(c, &p->a[i], i);
  }
}

/* ------------------------------------------------------------------ */
/* SELECT                                                             */
/* ------------------------------------------------------------------ */

static const char *sqtSetOpKw(u8 op){
  switch( op ){
    case TK_UNION:    return "union";
    case TK_EXCEPT:   return "except";
    case TK_INTERSECT:return "intersect";
    default:          return "union";
  }
}

/* dump one SELECT term; pPrior linkage handled by caller */
static void sqtSelectTerm(SqtCtx *c, const Select *p){
  sqlite3_str *o = c->node;
  sqtMap(o);
  sqtKKw(o, "node", "select");
  if( p->selFlags & SF_Distinct  ) sqtKBool(o, "distinct", 1);
  if( p->selFlags & SF_All       ) sqtKBool(o, "all", 1);
  if( p->selFlags & SF_NestedFrom) sqtKBool(o, "nested-from", 1);

  sqtWith(c, p->pWith);

  if( p->pEList ){
    sqtKey(o, "columns"); sqtVec(o);
    sqtExprList(c, p->pEList);
    sqtVecEnd(o);
  }
  if( p->pSrc ){
    sqtKey(o, "from"); sqtVec(o);
    sqtSrcList(c, p->pSrc);
    sqtVecEnd(o);
  }
  if( p->pWhere ){ sqtKey(o, "where"); sqtExpr(c, p->pWhere); }
  if( p->pGroupBy ){
    sqtKey(o, "group-by"); sqtVec(o);
    sqtExprList(c, p->pGroupBy);
    sqtVecEnd(o);
  }
  if( p->pHaving ){ sqtKey(o, "having"); sqtExpr(c, p->pHaving); }
#ifndef SQLITE_OMIT_WINDOWFUNC
  if( p->pWinDefn ){
    const Window *pw;
    int n = 0;
    sqtKey(o, "windows"); sqtVec(o);
    for(pw=p->pWinDefn; pw; pw=pw->pNextWin){
      if( n++ ) sqtSep(o);
      sqtWindowFn(c, pw);
    }
    sqtVecEnd(o);
  }
#endif
  if( p->pOrderBy ){
    sqtKey(o, "order-by"); sqtVec(o);
    sqtExprList(c, p->pOrderBy);
    sqtVecEnd(o);
  }
  if( p->pLimit ){ sqtKey(o, "limit"); sqtExpr(c, p->pLimit); }
  sqtMapEnd(o);
}

static void sqtSelect(SqtCtx *c, const Select *p){
  sqlite3_str *o = c->node;
  if( p==0 ){ sqlite3_str_appendall(o, "nil"); return; }
  if( c->depth>200 ){ sqlite3_str_appendall(o, "\"<too-deep>\""); return; }
  c->depth++;

  /* VALUES (...), (...) are linked through pPrior with SF_MultiValue;
  ** surface them as an explicit :values node with all rows. */
  if( (p->selFlags & SF_Values)!=0 ){
    const Select *pt;
    int n = 0;
    for(pt=p; pt; pt=pt->pPrior) n++;
    sqtMap(o);
    sqtKKw(o, "node", "values");
    sqtWith(c, p->pWith);
    sqtKey(o, "rows"); sqtVec(o);
    {
      /* chain is right-to-left; emit rows in written order */
      int i;
      const Select **ap = (const Select**)sqlite3StackAllocRaw(
                            c->pParse->db, n*sizeof(Select*));
      if( ap ){
        for(i=n-1, pt=p; i>=0; i--, pt=pt->pPrior) ap[i] = pt;
        for(i=0;i<n;i++){
          if(i) sqtSep(o);
          if( ap[i]->pEList ){
            sqtVec(o);
            sqtExprList(c, ap[i]->pEList);
            sqtVecEnd(o);
          }else{
            sqlite3_str_appendall(o, "[]");
          }
        }
        sqlite3StackFree(c->pParse->db, (void*)ap);
      }
    }
    sqtVecEnd(o);
    sqtMapEnd(o);
    c->depth--;
    return;
  }

  if( p->pPrior ){
    /* compound: op on the right term relates it to the left chain */
    sqtMap(o);
    sqtKKw(o, "node", "compound");
    if( p->op==TK_ALL ){
      sqtKKw(o, "op", "union-all");
    }else{
      sqtKKw(o, "op", sqtSetOpKw(p->op));
    }
    if( p->selFlags & SF_Recursive ) sqtKBool(o, "recursive", 1);
    sqtKey(o, "left");  c->depth--; sqtSelect(c, p->pPrior); c->depth++;
    sqtKey(o, "right"); c->depth--; sqtSelectTerm(c, p);     c->depth++;
    /* LIMIT/OFFSET/ORDER BY of the last term apply to the whole compound */
    if( p->pOrderBy ){
      sqtKey(o, "order-by"); sqtVec(o);
      sqtExprList(c, p->pOrderBy);
      sqtVecEnd(o);
    }
    if( p->pLimit ){ sqtKey(o, "limit"); sqtExpr(c, p->pLimit); }
    sqtMapEnd(o);
    c->depth--;
    return;
  }

  sqtSelectTerm(c, p);
  c->depth--;
}

/* ------------------------------------------------------------------ */
/* table/index/trigger definitions (DDL)                              */
/* ------------------------------------------------------------------ */

static void sqtIndexDef(SqtCtx *c, const Index *pIdx){
  sqlite3_str *o = c->node;
  int i;
  sqtMap(o);
  sqtKKw(o, "node", "index");
  if( pIdx->zName ) sqtKStr(o, "name", pIdx->zName);
  switch( pIdx->idxType ){
    case SQLITE_IDXTYPE_UNIQUE:     sqtKKw(o, "kind", "unique");       break;
    case SQLITE_IDXTYPE_PRIMARYKEY: sqtKKw(o, "kind", "primary-key");  break;
    case SQLITE_IDXTYPE_IPK:        sqtKKw(o, "kind", "rowid");        break;
    default:                        sqtKKw(o, "kind", "index");        break;
  }
  if( pIdx->onError!=OE_None ) sqtKKw(o, "on-conflict", sqtOnConfKw(pIdx->onError));
  if( pIdx->nColumn>0 ){
    sqtKey(o, "columns"); sqtVec(o);
    for(i=0;i<pIdx->nColumn;i++){
      if(i) sqtSep(o);
      sqtMap(o);
      if( pIdx->aiColumn[i]==XN_ROWID ){
        sqtKKw(o, "name", "rowid");
        sqtKInt(o, "idx", -1);
      }else if( pIdx->aiColumn[i]==XN_EXPR && pIdx->aColExpr
                && i<pIdx->aColExpr->nExpr ){
        sqtKKw(o, "name", "expr");
        sqtKey(o, "expr"); sqtExpr(c, pIdx->aColExpr->a[i].pExpr);
      }else if( pIdx->pTable && pIdx->aiColumn[i]>=0
                && pIdx->aiColumn[i]<pIdx->pTable->nCol ){
        const Column *pCol = &pIdx->pTable->aCol[pIdx->aiColumn[i]];
        sqtKStr(o, "name", pCol->zCnName);
        sqtKInt(o, "idx", pIdx->aiColumn[i]);
      }
      if( pIdx->aSortOrder && pIdx->aSortOrder[i] ){
        sqtKKw(o, "dir", "desc");
      }
      if( pIdx->azColl && pIdx->azColl[i] ){
        sqtKStr(o, "collation", pIdx->azColl[i]);
      }
      sqtMapEnd(o);
    }
    sqtVecEnd(o);
  }
  if( pIdx->pPartIdxWhere ){
    sqtKey(o, "where"); sqtExpr(c, pIdx->pPartIdxWhere);
  }
  sqtMapEnd(o);
}

static void sqtForeignKey(SqtCtx *c, const FKey *pFk){
  sqlite3_str *o = c->node;
  int i;
  Table *pParent = 0;
  sqtMap(o);
  sqtKKw(o, "node", "foreign-key");
  if( pFk->pFrom && pFk->pFrom->pSchema && c->pParse->db ){
    pParent = sqlite3FindTable(c->pParse->db, pFk->zTo,
              sqtSchemaDbName(c->pParse->db, pFk->pFrom->pSchema));
  }
  sqtKStr(o, "parent-table", pFk->zTo);
  sqtKBool(o, "parent-found", pParent!=0);
  sqtKey(o, "columns"); sqtVec(o);
  for(i=0;i<pFk->nCol;i++){
    int iFrom = pFk->aCol[i].iFrom;
    if(i) sqtSep(o);
    sqtMap(o);
    if( pFk->pFrom && iFrom>=0 && iFrom<pFk->pFrom->nCol ){
      sqtKStr(o, "column", pFk->pFrom->aCol[iFrom].zCnName);
      sqtKInt(o, "idx", iFrom);
    }
    if( pFk->aCol[i].zCol ){
      sqtKStr(o, "parent-column", pFk->aCol[i].zCol);
      if( pParent ){
        int j = sqlite3ColumnIndex(pParent, pFk->aCol[i].zCol);
        if( j>=0 ) sqtKInt(o, "parent-idx", j);
      }
    }else if( pParent && pParent->iPKey>=0 ){
      sqtKStr(o, "parent-column", pParent->aCol[pParent->iPKey].zCnName);
    }else{
      sqtKKw(o, "parent-column", "primary-key");
    }
    sqtMapEnd(o);
  }
  sqtVecEnd(o);
  sqtKKw(o, "on-delete", sqtOnConfKw(pFk->aAction[0]));
  sqtKKw(o, "on-update", sqtOnConfKw(pFk->aAction[1]));
  if( pFk->isDeferred ) sqtKBool(o, "deferred", 1);
  sqtMapEnd(o);
}

static void sqtTableDef(SqtCtx *c, const Table *p){
  sqlite3_str *o = c->node;
  sqtMap(o);
  sqtKKw(o, "node", "table");
  if( p->zName ) sqtKStr(o, "name", p->zName);
  if( p->pSchema && c->pParse->db ){
    const char *zDb = sqtSchemaDbName(c->pParse->db, p->pSchema);
    if( zDb ) sqtKStr(o, "schema", zDb);
  }
  if( p->tabFlags & TF_WithoutRowid  ) sqtKBool(o, "without-rowid", 1);
  if( p->tabFlags & TF_Strict        ) sqtKBool(o, "strict", 1);
  if( p->tabFlags & TF_Autoincrement ) sqtKBool(o, "autoincrement", 1);
  if( p->iPKey>=0 && p->iPKey<p->nCol ){
    sqtKStr(o, "rowid-alias", p->aCol[p->iPKey].zCnName);
  }
  sqtColumnsOf(c, p, 0);
  if( IsOrdinaryTable(p) && p->pCheck && p->pCheck->nExpr ){
    int i;
    sqtKey(o, "checks"); sqtVec(o);
    for(i=0;i<p->pCheck->nExpr;i++){
      if(i) sqtSep(o);
      sqtMap(o);
      sqtKKw(o, "node", "check");
      {
        const Expr *pe = p->pCheck->a[i].pExpr;
        /* CHECKs are stored in TK_SPAN wrappers carrying original text */
        if( pe && pe->op==TK_SPAN && ExprUseUToken(pe) && pe->u.zToken ){
          sqtKStr(o, "span", pe->u.zToken);
        }
        sqtKey(o, "expr"); sqtExpr(c, pe);
      }
      sqtMapEnd(o);
    }
    sqtVecEnd(o);
  }
  if( IsOrdinaryTable(p) && p->u.tab.pFKey ){
    const FKey *pFk;
    int n = 0;
    sqtKey(o, "foreign-keys"); sqtVec(o);
    for(pFk=p->u.tab.pFKey; pFk; pFk=pFk->pNextFrom){
      if( n++ ) sqtSep(o);
      sqtForeignKey(c, pFk);
    }
    sqtVecEnd(o);
  }
  if( p->pIndex ){
    const Index *pi;
    int n = 0;
    sqtKey(o, "indexes"); sqtVec(o);
    for(pi=p->pIndex; pi; pi=pi->pNext){
      if( n++ ) sqtSep(o);
      sqtIndexDef(c, pi);
    }
    sqtVecEnd(o);
  }
  sqtMapEnd(o);
}

/* ------------------------------------------------------------------ */
/* resolve helpers (mirror what codegen would do, minus codegen)      */
/* ------------------------------------------------------------------ */

/* Resolve a whole SELECT the same way sqlite3Select() does. */
static int sqtResolveSelect(SqtCtx *c, Select *p){
  if( p==0 ) return 0;
  sqlite3SelectPrep(c->pParse, p, 0);
  return c->pParse->nErr!=0;
}

/* Build a one-item SrcList over pTab for expression resolution. */
static SrcList *sqtFakeSrc(SqtCtx *c, Table *pTab){
  sqlite3 *db = c->pParse->db;
  SrcList *pSrc = sqlite3DbMallocRawNN(db, SZ_SRCLIST_1);
  if( pSrc==0 ) return 0;
  memset(pSrc, 0, SZ_SRCLIST_1);
  pSrc->nSrc = 1;
  pSrc->a[0].zName = sqlite3DbStrDup(db, pTab->zName);
  pSrc->a[0].pSTab = pTab;
  pSrc->a[0].iCursor = c->pParse->nTab++;
  return pSrc;
}
static void sqtFreeSrc(SqtCtx *c, SrcList *p){
  if( p ){
    sqlite3DbFree(c->pParse->db, p->a[0].zName);
    sqlite3DbFree(c->pParse->db, p);
  }
}

/* Resolve *ppExpr against pSrc with the given ncFlags. Returns 0=ok. */
static int sqtResolveExpr(SqtCtx *c, Expr **ppExpr, SrcList *pSrc,
                          int ncFlags){
  NameContext sNC;
  int rc;
  if( ppExpr==0 || *ppExpr==0 ) return 0;
  memset(&sNC, 0, sizeof(sNC));
  sNC.pParse = c->pParse;
  sNC.pSrcList = pSrc;
  sNC.ncFlags = ncFlags;
  rc = sqlite3ResolveExprNames(&sNC, *ppExpr);
  return rc!=0 || c->pParse->nErr!=0;
}

static int sqtResolveExprList(SqtCtx *c, ExprList *pList, SrcList *pSrc,
                              int ncFlags){
  int i;
  if( pList==0 ) return 0;
  for(i=0;i<pList->nExpr;i++){
    if( sqtResolveExpr(c, &pList->a[i].pExpr, pSrc, ncFlags) ) return 1;
  }
  return 0;
}

/* Resolve exprs attached to a table's DEFAULT/AS/CHECK parts so the
** dump can show resolved column references inside them. */
static void sqtResolveTableExprs(SqtCtx *c, Table *pTab){
  int i;
  SrcList *pSrc;
  if( !IsOrdinaryTable(pTab) ) return;
  pSrc = sqtFakeSrc(c, pTab);
  if( pSrc==0 ) return;
  for(i=0;i<pTab->nCol;i++){
    Expr *pE = sqlite3ColumnExpr(pTab, &pTab->aCol[i]);
    while( pE && pE->op==TK_SPAN && pE->pLeft ) pE = pE->pLeft;
    if( pE ) sqtResolveExpr(c, &pE, pSrc, NC_GenCol);
  }
  if( pTab->pCheck ){
    int j;
    for(j=0;j<pTab->pCheck->nExpr;j++){
      Expr *pE = pTab->pCheck->a[j].pExpr;
      while( pE && pE->op==TK_SPAN && pE->pLeft ) pE = pE->pLeft;
      if( pE ) sqtResolveExpr(c, &pE, pSrc, NC_IsCheck);
    }
  }
  sqtFreeSrc(c, pSrc);
}

/* ------------------------------------------------------------------ */
/* statement node builders (called by hooks)                          */
/* ------------------------------------------------------------------ */

/* Begin a fresh node buffer for the current statement. */
static void sqtBeginStmtMap(SqtCtx *c, const char *kind){
  sqlite3_str_reset(c->node);
  c->hasNode = 1;
  sqtMap(c->node);
  sqtKKw(c->node, "node", kind);
}

static void sqtReturning(SqtCtx *c){
  Parse *pParse = c->pParse;
  Returning *pRet;
  pRet = pParse->u1.d.pReturning;
  if( pRet && pRet->pReturnEL && pRet->pReturnEL->nExpr ){
    int i;
    sqtKey(c->node, "returning"); sqtVec(c->node);
    for(i=0;i<pRet->pReturnEL->nExpr;i++){
      if(i) sqtSep(c->node);
      sqtExprListItem(c, &pRet->pReturnEL->a[i]);
    }
    sqtVecEnd(c->node);
  }
}

/* resolve RETURNING exprs against pSrc (best effort, mirrors INSERT
** codegen which resolves them at fire time) */
static void sqtResolveReturning(SqtCtx *c, SrcList *pSrc){
  Parse *pParse = c->pParse;
  Returning *pRet;
  pRet = pParse->u1.d.pReturning;
  if( pRet && pRet->pReturnEL ){
    int i;
    for(i=0;i<pRet->pReturnEL->nExpr;i++){
      sqtResolveExpr(c, &pRet->pReturnEL->a[i].pExpr, pSrc, 0);
    }
  }
}

/* full statement envelope: {:node ... :sql ... :explain n} */
static void sqtEmitStatement(SqtCtx *c, const char *zEnd){
  sqlite3_str *o = c->out;
  const Parse *pParse = c->pParse;
  if( o==0 ) return;
  sqtMap(o);
  if( c->hasNode && sqlite3_str_value(c->node) ){
    sqlite3_str_appendall(o, ":stmt ");
    sqlite3_str_appendall(o, sqlite3_str_value(c->node));
  }else{
    sqlite3_str_appendall(o, ":stmt :other");
  }
  {
    const char *zS = c->zStmtStart;
    int n = (int)(zEnd - zS);
    if( zS && n>0 ){
      /* trim trailing semicolon and whitespace from the span */
      while( n>0 && (zS[n-1]==';' || sqlite3Isspace(zS[n-1])) ) n--;
      sqtKey(o, "sql"); sqtStrN(o, zS, n);
    }
  }
  if( pParse->explain ) sqtKInt(o, "explain", pParse->explain);
  sqtMapEnd(o);
  sqlite3_str_appendchar(o, 1, '\n');
  c->hasNode = 0;
}

/* advance statement start past ; whitespace and comments */
static const char *sqtNextStmtStart(const char *p, const char *zEnd){
  while( p<zEnd ){
    if( *p==';' || sqlite3Isspace((unsigned char)*p) ){
      p++;
    }else if( p+1<zEnd && p[0]=='-' && p[1]=='-' ){
      p += 2;
      while( p<zEnd && *p!='\n' ) p++;
    }else if( p+1<zEnd && p[0]=='/' && p[1]=='*' ){
      p += 2;
      while( p+1<zEnd && !(p[0]=='*' && p[1]=='/') ) p++;
      p = (p+1<zEnd) ? p+2 : zEnd;
    }else{
      break;
    }
  }
  return p;
}

/* ------------------------------------------------------------------ */
/* hook implementations                                               */
/* ------------------------------------------------------------------ */

int sqltreeStmtFinish(Parse *pParse){
  SqtCtx *c = sqtCtx(pParse);
  const char *zEnd;
  if( c==0 ) return 0;
  zEnd = pParse->sLastToken.z + pParse->sLastToken.n;
  if( zEnd < c->zStmtStart ) zEnd = c->zStmtStart;
  if( pParse->nErr==0 ){
    sqtEmitStatement(c, zEnd);
  }
  c->zStmtStart = sqtNextStmtStart(zEnd, c->zSql + strlen(c->zSql));
  return 1;   /* skip all of sqlite3FinishCoding */
}

int sqltreeSelect(Parse *pParse, Select *p){
  SqtCtx *c = sqtCtx(pParse);
  if( c==0 ) return 0;
  if( pParse->nErr ) return 0;
  sqlite3_str_reset(c->node);
  c->hasNode = 1;
  sqtSelect(c, p);
  return 1;
}

int sqltreeInsert(Parse *pParse, SrcList *pTabList, Select *pSelect,
                  IdList *pColumn, int onError, Upsert *pUpsert){
  SqtCtx *c = sqtCtx(pParse);
  sqlite3_str *o;
  Table *pTab;
  int i;
  if( c==0 ) return 0;
  o = c->node;
  pTab = sqlite3SrcListLookup(pParse, pTabList);
  if( pTab ) pTabList->a[0].iCursor = pParse->nTab++;
  sqtBeginStmtMap(c, "insert");
  sqtKey(o, "into"); sqtSrcItem(c, &pTabList->a[0], 0);
  if( onError!=OE_Default && onError!=OE_None ){
    sqtKKw(o, "or", sqtOnConfKw(onError));
  }
  if( pColumn && pColumn->nId ){
    sqtKey(o, "columns"); sqtVec(o);
    for(i=0;i<pColumn->nId;i++){
      if( i ) sqtSep(o);
      sqtMap(o);
      sqtKStr(o, "name", pColumn->a[i].zName);
      if( pTab ){
        int ix = sqlite3ColumnIndex(pTab, pColumn->a[i].zName);
        if( ix>=0 ) sqtKInt(o, "idx", ix);
      }
      sqtMapEnd(o);
    }
    sqtVecEnd(o);
  }
  if( pSelect==0 ){
    sqtKKw(o, "source", "default-values");
  }else{
    int bad = sqtResolveSelect(c, pSelect);
    sqtKey(o, "source");
    c->depth--; sqtSelect(c, pSelect); c->depth++;
    if( bad ) sqtKBool(o, "resolve-error", 1);
  }
  if( pUpsert ){
    const Upsert *pu;
    int n = 0;
    sqtKey(o, "on-conflict"); sqtVec(o);
    for(pu=pUpsert; pu; pu=pu->pNextUpsert){
      if( n++ ) sqtSep(o);
      sqtMap(o);
      sqtKKw(o, "node", "upsert");
      if( pu->isDoUpdate ) sqtKKw(o, "action", "do-update");
      else                 sqtKKw(o, "action", "do-nothing");
      if( pu->pUpsertTarget ){
        /* conflict target resolves against the target table */
        SrcList *pSrc = pTab ? sqtFakeSrc(c, pTab) : 0;
        sqtResolveExprList(c, (ExprList*)pu->pUpsertTarget, pSrc, 0);
        if( pu->pUpsertTargetWhere ){
          sqtResolveExpr(c, (Expr**)&pu->pUpsertTargetWhere, pSrc, 0);
        }
        sqtKey(o, "target"); sqtVec(o);
        sqtExprList(c, pu->pUpsertTarget);
        sqtVecEnd(o);
        if( pu->pUpsertTargetWhere ){
          sqtKey(o, "target-where"); sqtExpr(c, pu->pUpsertTargetWhere);
        }
        if( pSrc ) sqtFreeSrc(c, pSrc);
      }
      if( pu->pUpsertSet ){
        sqtResolveExprList(c, (ExprList*)pu->pUpsertSet, pTabList, 0);
        if( pu->pUpsertWhere ){
          sqtResolveExpr(c, (Expr**)&pu->pUpsertWhere, pTabList, 0);
        }
        sqtKey(o, "set"); sqtVec(o);
        sqtExprList(c, pu->pUpsertSet);
        sqtVecEnd(o);
        if( pu->pUpsertWhere ){
          sqtKey(o, "where"); sqtExpr(c, pu->pUpsertWhere);
        }
      }
      sqtMapEnd(o);
    }
    sqtVecEnd(o);
  }
  sqtResolveReturning(c, pTabList);
  sqtReturning(c);
  sqtMapEnd(o);
  return 1;
}

int sqltreeUpdate(Parse *pParse, SrcList *pTabList, ExprList *pChanges,
                  Expr *pWhere, int onError, ExprList *pOrderBy,
                  Expr *pLimit, Upsert *pUpsert){
  SqtCtx *c = sqtCtx(pParse);
  sqlite3_str *o;
  Table *pTab;
  int i;
  if( c==0 ) return 0;
  o = c->node;
  (void)pUpsert;
  pTab = sqlite3SrcListLookup(pParse, pTabList);
  sqlite3SrcListAssignCursors(pParse, pTabList);
  sqtBeginStmtMap(c, "update");
  sqtKey(o, "target"); sqtSrcItem(c, &pTabList->a[0], 0);
  if( onError!=OE_Default && onError!=OE_None ){
    sqtKKw(o, "or", sqtOnConfKw(onError));
  }
  if( pTabList->nSrc>1 ){
    int j;
    sqtKey(o, "from"); sqtVec(o);
    for(j=1;j<pTabList->nSrc;j++){
      if( j>1 ) sqtSep(o);
      sqtSrcItem(c, &pTabList->a[j], j);
    }
    sqtVecEnd(o);
  }
  if( pChanges ){
    sqtKey(o, "set"); sqtVec(o);
    for(i=0;i<pChanges->nExpr;i++){
      if( i ) sqtSep(o);
      sqtMap(o);
      sqtKStr(o, "column", pChanges->a[i].zEName);
      if( pTab ){
        int ix = sqlite3ColumnIndex(pTab, pChanges->a[i].zEName);
        if( ix>=0 ) sqtKInt(o, "idx", ix);
        if( ix>=0 && ix==pTab->iPKey ) sqtKBool(o, "rowid", 1);
      }
      sqtResolveExpr(c, &pChanges->a[i].pExpr, pTabList, 0);
      sqtKey(o, "expr"); sqtExpr(c, pChanges->a[i].pExpr);
      sqtMapEnd(o);
    }
    sqtVecEnd(o);
  }
  if( pWhere ){
    sqtResolveExpr(c, &pWhere, pTabList, 0);
    sqtKey(o, "where"); sqtExpr(c, pWhere);
  }
  if( pOrderBy ){
    sqtResolveExprList(c, pOrderBy, pTabList, 0);
    sqtKey(o, "order-by"); sqtVec(o);
    sqtExprList(c, pOrderBy);
    sqtVecEnd(o);
  }
  if( pLimit ){
    Expr *pLim = pLimit;
    sqtResolveExpr(c, &pLim, 0, 0);
    sqtKey(o, "limit"); sqtExpr(c, pLim);
  }
  sqtResolveReturning(c, pTabList);
  sqtReturning(c);
  sqtMapEnd(o);
  return 1;
}

int sqltreeDelete(Parse *pParse, SrcList *pTabList, Expr *pWhere,
                  ExprList *pOrderBy, Expr *pLimit){
  SqtCtx *c = sqtCtx(pParse);
  sqlite3_str *o;
  if( c==0 ) return 0;
  o = c->node;
  sqlite3SrcListLookup(pParse, pTabList);
  pTabList->a[0].iCursor = pParse->nTab++;
  sqtBeginStmtMap(c, "delete");
  sqtKey(o, "from"); sqtSrcItem(c, &pTabList->a[0], 0);
  if( pWhere ){
    sqtResolveExpr(c, &pWhere, pTabList, 0);
    sqtKey(o, "where"); sqtExpr(c, pWhere);
  }
  if( pOrderBy ){
    sqtResolveExprList(c, pOrderBy, pTabList, 0);
    sqtKey(o, "order-by"); sqtVec(o);
    sqtExprList(c, pOrderBy);
    sqtVecEnd(o);
  }
  if( pLimit ){
    Expr *pLim = pLimit;
    sqtResolveExpr(c, &pLim, 0, 0);
    sqtKey(o, "limit"); sqtExpr(c, pLim);
  }
  sqtResolveReturning(c, pTabList);
  sqtReturning(c);
  sqtMapEnd(o);
  return 1;
}

int sqltreeEndTable(Parse *pParse, u32 tabOpts, Select *pSelect){
  SqtCtx *c = sqtCtx(pParse);
  sqlite3_str *o;
  Table *pTab;
  if( c==0 ) return 0;
  o = c->node;
  pTab = pParse->pNewTable;
  if( pTab==0 ) return 0;
  if( tabOpts & TF_WithoutRowid ) pTab->tabFlags |= TF_WithoutRowid;
  if( tabOpts & TF_Strict       ) pTab->tabFlags |= TF_Strict;
  if( pSelect ){
    int bad = sqtResolveSelect(c, pSelect);
    sqtBeginStmtMap(c, "create-table-as-select");
    sqtTableDef(c, pTab);
    /* columns for CTAS come from the select */
    sqtKey(o, "query"); c->depth--; sqtSelect(c, pSelect); c->depth++;
    if( bad ) sqtKBool(o, "resolve-error", 1);
  }else{
    sqtResolveTableExprs(c, pTab);
    sqtBeginStmtMap(c, "create-table");
    sqtTableDef(c, pTab);
  }
  sqtMapEnd(o);
  return 1;
}

int sqltreeCreateIndex(Parse *pParse, Token *pName1, Token *pName2,
                       SrcList *pTblName, ExprList *pList, int onError,
                       Token *pStart, Expr *pPIWhere, int sortOrder,
                       int ifNotExist, u8 idxType){
  SqtCtx *c = sqtCtx(pParse);
  sqlite3_str *o;
  Table *pTab;
  SrcList *pSrc;
  int i;
  if( c==0 ) return 0;
  (void)pStart; (void)sortOrder;
  o = c->node;
  pTab = sqlite3SrcListLookup(pParse, pTblName);
  pSrc = pTab ? sqtFakeSrc(c, pTab) : 0;
  sqtBeginStmtMap(c, "create-index");
  if( pName1 ){
    if( pName2 && pName2->n ){
      /* schema-qualified: name2 is the index name */
      sqtKTok(o, "name", pName2);
      sqtKTok(o, "schema", pName1);
    }else{
      sqtKTok(o, "name", pName1);
    }
  }
  if( idxType==SQLITE_IDXTYPE_UNIQUE ) sqtKBool(o, "unique", 1);
  if( onError!=OE_None && onError!=OE_Default ){
    sqtKKw(o, "on-conflict", sqtOnConfKw(onError));
  }
  if( ifNotExist ) sqtKBool(o, "if-not-exists", 1);
  sqtKey(o, "table"); sqtSrcItem(c, &pTblName->a[0], 0);
  if( pList ){
    sqtKey(o, "columns"); sqtVec(o);
    for(i=0;i<pList->nExpr;i++){
      struct ExprList_item *it = &pList->a[i];
      if( i ) sqtSep(o);
      sqtMap(o);
      if( pSrc ) sqtResolveExpr(c, &it->pExpr, pSrc, NC_IdxExpr);
      sqtKey(o, "expr"); sqtExpr(c, it->pExpr);
      if( it->zEName && it->fg.eEName==ENAME_NAME ){
        sqtKStr(o, "collation", it->zEName);
      }
      if( it->fg.sortFlags & KEYINFO_ORDER_DESC ){
        sqtKKw(o, "dir", "desc");
      }else{
        sqtKKw(o, "dir", "asc");
      }
      if( it->fg.bNulls ){
        sqtKKw(o, "nulls",
               (it->fg.sortFlags & KEYINFO_ORDER_BIGNULL) ? "first" : "last");
      }
      /* map bare column references to column idx */
      if( it->pExpr && it->pExpr->op==TK_COLUMN && pTab ){
        if( it->pExpr->iColumn<0 ){
          sqtKKw(o, "column", "rowid");
        }else if( it->pExpr->iColumn<pTab->nCol ){
          sqtKStr(o, "column", pTab->aCol[it->pExpr->iColumn].zCnName);
          sqtKInt(o, "idx", it->pExpr->iColumn);
        }
      }
      sqtMapEnd(o);
    }
    sqtVecEnd(o);
  }
  if( pPIWhere ){
    if( pSrc ) sqtResolveExpr(c, &pPIWhere, pSrc, NC_IdxExpr|NC_PartIdx);
    sqtKey(o, "where"); sqtExpr(c, pPIWhere);
  }
  if( pSrc ) sqtFreeSrc(c, pSrc);
  sqtMapEnd(o);
  return 1;
}

int sqltreeCreateView(Parse *pParse, Token *pName1, Token *pName2,
                      ExprList *pCNames, Select *pSel, int isTemp, int noErr){
  SqtCtx *c = sqtCtx(pParse);
  sqlite3_str *o;
  int bad = 0;
  if( c==0 ) return 0;
  o = c->node;
  if( pParse->nVar>0 ){
    sqlite3ErrorMsg(pParse, "parameters are not allowed in views");
    return 1;
  }
  bad = sqtResolveSelect(c, pSel);
  sqtBeginStmtMap(c, "create-view");
  if( pName2 && pName2->n ){
    sqtKTok(o, "name", pName2);
    sqtKTok(o, "schema", pName1);
  }else{
    sqtKTok(o, "name", pName1);
  }
  if( isTemp ) sqtKBool(o, "temp", 1);
  if( noErr ) sqtKBool(o, "if-not-exists", 1);
  if( pCNames && pCNames->nExpr ){
    int i;
    sqtKey(o, "columns"); sqtVec(o);
    for(i=0;i<pCNames->nExpr;i++){
      if( i ) sqtSep(o);
      sqtStr(o, pCNames->a[i].zEName);
    }
    sqtVecEnd(o);
  }
  sqtKey(o, "query"); c->depth--; sqtSelect(c, pSel); c->depth++;
  if( bad ) sqtKBool(o, "resolve-error", 1);
  sqtMapEnd(o);
  return 1;
}

/* Trigger steps store their expression trees as EXPRDUP_REDUCE
** packed duplicates: nodes are truncated at Expr.iTable and cannot be
** resolved in place (SQLite itself never does that; fire-time code
** works on full-size copies). Build a full-size, resolved copy of each
** component, dump from the copy, and leave the stored originals
** untouched. */
typedef struct SqtStepView {
  ExprList *pSet;      /* resolved full-size copy of pExprList */
  Expr *pWhere;        /* resolved full-size copy of pWhere */
  Select *pSelect;     /* resolved full-size copy of pSelect */
} SqtStepView;

static void sqtFreeStepView(sqlite3 *db, SqtStepView *v){
  sqlite3ExprListDelete(db, v->pSet);
  sqlite3ExprDelete(db, v->pWhere);
  sqlite3SelectDelete(db, v->pSelect);
}

static void sqtResolveTriggerSteps(SqtCtx *c, TriggerStep *pStepList,
                                   Table *pTab, int trigOp,
                                   SqtStepView *aView, int nStep){
  sqlite3 *db = c->pParse->db;
  SrcList *pSrc = pTab ? sqtFakeSrc(c, pTab) : 0;
  Parse *pParse = c->pParse;
  Table *pSaveTab = pParse->pTriggerTab;
  u8 eSaveOp = pParse->eTriggerOp;
  int i;
  if( aView==0 ) return;
  if( pSrc==0 ){ memset(aView, 0, nStep*sizeof(SqtStepView)); return; }
  pParse->pTriggerTab = pTab;
  pParse->eTriggerOp = trigOp;
  for(i=0; i<nStep; i++, pStepList = pStepList ? pStepList->pNext : 0){
    SqtStepView *v = &aView[i];
    if( pStepList==0 ) break;
    memset(v, 0, sizeof(*v));
    if( pStepList->pSrc && pStepList->pSrc->nSrc>0 ){
      pStepList->pSrc->a[0].iCursor = pSrc->a[0].iCursor;
    }
    if( pStepList->pExprList ){
      v->pSet = sqlite3ExprListDup(db, pStepList->pExprList, 0);
      sqtResolveExprList(c, v->pSet, pSrc, 0);
    }
    if( pStepList->pWhere ){
      v->pWhere = sqlite3ExprDup(db, pStepList->pWhere, 0);
      sqtResolveExpr(c, &v->pWhere, pSrc, 0);
    }
    if( pStepList->pSelect ){
      NameContext sNC;
      v->pSelect = sqlite3SelectDup(db, pStepList->pSelect, 0);
      memset(&sNC, 0, sizeof(sNC));
      sNC.pParse = pParse;
      sNC.pSrcList = pSrc;
      sqlite3SelectPrep(pParse, v->pSelect, &sNC);
    }
  }
  pParse->pTriggerTab = pSaveTab;
  pParse->eTriggerOp = eSaveOp;
  sqtFreeSrc(c, pSrc);
}

int sqltreeFinishTrigger(Parse *pParse, TriggerStep *pSteps, Token *pAll){
  SqtCtx *c = sqtCtx(pParse);
  sqlite3_str *o;
  Trigger *pTrig;
  Table *pTab = 0;
  const TriggerStep *ps;
  Expr *pWhenCopy = 0;
  SqtStepView *aView = 0;
  int nStep = 0;
  int i;
  if( c==0 ) return 0;
  (void)pAll;
  o = c->node;
  pTrig = pParse->pNewTrigger;
  if( pTrig==0 ) return 0;
  if( pTrig->pTabSchema && c->pParse->db ){
    pTab = sqlite3FindTable(pParse->db, pTrig->table,
              sqtSchemaDbName(pParse->db, pTrig->pTabSchema));
  }
  /* WHEN is stored reduced like the steps; resolve a full copy */
  pWhenCopy = pTrig->pWhen ? sqlite3ExprDup(pParse->db, pTrig->pWhen, 0) : 0;
  if( pWhenCopy && pTab ){
    SrcList *pSrc = sqtFakeSrc(c, pTab);
    if( pSrc ){
      Table *pSaveTab = pParse->pTriggerTab;
      u8 eSaveOp = pParse->eTriggerOp;
      pParse->pTriggerTab = pTab;
      pParse->eTriggerOp = pTrig->op;
      sqtResolveExpr(c, &pWhenCopy, pSrc, 0);
      pParse->pTriggerTab = pSaveTab;
      pParse->eTriggerOp = eSaveOp;
      sqtFreeSrc(c, pSrc);
    }
  }
  for(nStep=0, ps=pSteps; ps; ps=ps->pNext) nStep++;
  aView = (SqtStepView*)sqlite3DbMallocRaw(pParse->db, nStep*sizeof(SqtStepView));
  if( aView ) memset(aView, 0, nStep*sizeof(SqtStepView));
  sqtResolveTriggerSteps(c, pSteps, pTab, pTrig->op, aView, nStep);
  {
    Table *pSaveTab = pParse->pTriggerTab;
    u8 eSaveOp = pParse->eTriggerOp;
    pParse->pTriggerTab = pTab;
    pParse->eTriggerOp = pTrig->op;
    sqtDumpTriggerBody(c, pTrig, pTab, pSteps, aView, nStep, pWhenCopy);
    pParse->pTriggerTab = pSaveTab;
    pParse->eTriggerOp = eSaveOp;
  }
  if( aView ){
    for(i=0;i<nStep;i++) sqtFreeStepView(pParse->db, &aView[i]);
    sqlite3DbFree(pParse->db, aView);
  }
  sqlite3ExprDelete(pParse->db, pWhenCopy);
  return 1;
}

/* body of the create-trigger statement, dumped with the trigger table
** context active so NEW/OLD columns resolve to names */
static void sqtDumpTriggerBody(SqtCtx *c, const Trigger *pTrig,
                               const Table *pTab, const TriggerStep *pSteps,
                               const SqtStepView *aView, int nStep,
                               const Expr *pWhenCopy){
  sqlite3_str *o = c->node;
  int i;
  const TriggerStep *ps;
  sqtBeginStmtMap(c, "create-trigger");
  sqtKStr(o, "name", pTrig->zName);
  sqtKStr(o, "table", pTrig->table);
  if( pTab ){
    sqtKBool(o, "table-found", 1);
  }else{
    sqlite3ErrorMsg(c->pParse, "no such table: %s", pTrig->table);
  }
  sqtKKw(o, "timing", pTrig->tr_tm==TRIGGER_BEFORE ? "before" : "after");
  switch( pTrig->op ){
    case TK_INSERT: sqtKKw(o, "event", "insert");  break;
    case TK_UPDATE: sqtKKw(o, "event", "update");  break;
    case TK_DELETE: sqtKKw(o, "event", "delete");  break;
    default:        sqtKKw(o, "event", "unknown"); break;
  }
  if( pTrig->pColumns && pTrig->pColumns->nId ){
    int i;
    sqtKey(o, "update-of"); sqtVec(o);
    for(i=0;i<pTrig->pColumns->nId;i++){
      if( i ) sqtSep(o);
      sqtStr(o, pTrig->pColumns->a[i].zName);
    }
    sqtVecEnd(o);
  }
  if( pWhenCopy ){ sqtKey(o, "when"); sqtExpr(c, pWhenCopy); }
  sqtKey(o, "steps"); sqtVec(o);
  for(i=0, ps=pSteps; ps; ps=ps->pNext, i++){
    const SqtStepView *v = (aView && i<nStep) ? &aView[i] : 0;
    sqtSep(o);
    sqtMap(o);
    switch( ps->op ){
      case TK_INSERT:  sqtKKw(o, "node", "insert");  break;
      case TK_UPDATE:  sqtKKw(o, "node", "update");  break;
      case TK_DELETE:  sqtKKw(o, "node", "delete");  break;
      case TK_SELECT:  sqtKKw(o, "node", "select");  break;
      default:         sqtKKw(o, "node", "step");    break;
    }
    if( ps->zSpan ) sqtKStr(o, "sql", ps->zSpan);
    if( ps->orconf!=OE_Default ) sqtKKw(o, "or", sqtOnConfKw(ps->orconf));
    if( ps->pSrc && ps->pSrc->nSrc ){
      sqtKey(o, "target"); sqtSrcItem(c, &ps->pSrc->a[0], 0);
    }
    if( ps->pIdList && ps->pIdList->nId ){
      int i;
      sqtKey(o, "columns"); sqtVec(o);
      for(i=0;i<ps->pIdList->nId;i++){
        if( i ) sqtSep(o);
        sqtStr(o, ps->pIdList->a[i].zName);
      }
      sqtVecEnd(o);
    }
    if( v && v->pSet ){
      sqtKey(o, "set"); sqtVec(o);
      sqtExprList(c, v->pSet);
      sqtVecEnd(o);
    }else if( ps->pExprList ){
      sqtKey(o, "set"); sqtVec(o);
      sqtExprList(c, ps->pExprList);
      sqtVecEnd(o);
    }
    if( v && v->pWhere ){ sqtKey(o, "where"); sqtExpr(c, v->pWhere); }
    else if( ps->pWhere ){ sqtKey(o, "where"); sqtExpr(c, ps->pWhere); }
    if( v && v->pSelect ){
      sqtKey(o, "source"); c->depth--; sqtSelect(c, v->pSelect); c->depth++;
    }else if( ps->pSelect ){
      sqtKey(o, "source"); c->depth--; sqtSelect(c, ps->pSelect); c->depth++;
    }
    sqtMapEnd(o);
  }
  sqtVecEnd(o);
  sqtMapEnd(o);
}

int sqltreePragma(Parse *pParse, Token *pId1, Token *pId2, Token *pValue,
                  int minusFlag){
  SqtCtx *c = sqtCtx(pParse);
  sqlite3_str *o;
  if( c==0 ) return 0;
  o = c->node;
  sqtBeginStmtMap(c, "pragma");
  if( pId2 && pId2->n ){
    sqtKTok(o, "name", pId2);
    sqtKTok(o, "schema", pId1);
  }else{
    sqtKTok(o, "name", pId1);
  }
  if( pValue ){
    if( minusFlag ) sqtKBool(o, "negated", 1);
    sqtKTok(o, "value", pValue);
  }
  sqtMapEnd(o);
  return 1;
}

int sqltreeDropTable(Parse *pParse, SrcList *pName, int isView, int noErr){
  SqtCtx *c = sqtCtx(pParse);
  sqlite3_str *o;
  Table *pTab;
  if( c==0 ) return 0;
  o = c->node;
  pTab = sqlite3LocateTableItem(pParse, noErr ? LOCATE_NOERR : 0, &pName->a[0]);
  if( pTab==0 && !noErr ){
    sqlite3ErrorMsg(pParse, "no such table: %s", pName->a[0].zName);
  }
  sqtBeginStmtMap(c, "drop-table");
  if( isView ) sqtKBool(o, "view", 1);
  if( noErr ) sqtKBool(o, "if-exists", 1);
  sqtKey(o, "target"); sqtSrcItem(c, &pName->a[0], 0);
  if( pTab ) sqtKBool(o, "found", 1);
  sqtMapEnd(o);
  return 1;
}

int sqltreeDropIndex(Parse *pParse, SrcList *pName, int ifExists){
  SqtCtx *c = sqtCtx(pParse);
  sqlite3_str *o;
  if( c==0 ) return 0;
  o = c->node;
  sqtBeginStmtMap(c, "drop-index");
  if( ifExists ) sqtKBool(o, "if-exists", 1);
  sqtKey(o, "target"); sqtSrcItem(c, &pName->a[0], 0);
  sqtMapEnd(o);
  return 1;
}

int sqltreeDropTrigger(Parse *pParse, SrcList *pName, int noErr){
  SqtCtx *c = sqtCtx(pParse);
  sqlite3_str *o;
  if( c==0 ) return 0;
  o = c->node;
  sqtBeginStmtMap(c, "drop-trigger");
  if( noErr ) sqtKBool(o, "if-exists", 1);
  sqtKey(o, "target"); sqtSrcItem(c, &pName->a[0], 0);
  sqtMapEnd(o);
  return 1;
}

int sqltreeBeginTxn(Parse *pParse, int type){
  SqtCtx *c = sqtCtx(pParse);
  if( c==0 ) return 0;
  sqtBeginStmtMap(c, "begin");
  sqtKKw(c->node, "type",
         type==TK_IMMEDIATE ? "immediate" :
         type==TK_EXCLUSIVE ? "exclusive" : "deferred");
  sqtMapEnd(c->node);
  return 1;
}

int sqltreeEndTxn(Parse *pParse, int type){
  SqtCtx *c = sqtCtx(pParse);
  if( c==0 ) return 0;
  sqtBeginStmtMap(c, type==TK_ROLLBACK ? "rollback" : "commit");
  sqtMapEnd(c->node);
  return 1;
}

int sqltreeSavepoint(Parse *pParse, int op, Token *pName){
  SqtCtx *c = sqtCtx(pParse);
  if( c==0 ) return 0;
  sqtBeginStmtMap(c,
    op==0 ? "savepoint" : op==1 ? "release" : "rollback-to");
  sqtKTok(c->node, "name", pName);
  sqtMapEnd(c->node);
  return 1;
}

int sqltreeAttach(Parse *pParse, Expr *pFile, Expr *pDb, Expr *pKey){
  SqtCtx *c = sqtCtx(pParse);
  if( c==0 ) return 0;
  sqtBeginStmtMap(c, "attach");
  sqtKey(c->node, "file");  sqtExpr(c, pFile);
  sqtKey(c->node, "as");    sqtExpr(c, pDb);
  if( pKey ){ sqtKey(c->node, "key"); sqtExpr(c, pKey); }
  sqtMapEnd(c->node);
  return 1;
}

int sqltreeDetach(Parse *pParse, Expr *pDb){
  SqtCtx *c = sqtCtx(pParse);
  if( c==0 ) return 0;
  sqtBeginStmtMap(c, "detach");
  sqtKey(c->node, "database"); sqtExpr(c, pDb);
  sqtMapEnd(c->node);
  return 1;
}

int sqltreeAnalyze(Parse *pParse, Token *pName1, Token *pName2){
  SqtCtx *c = sqtCtx(pParse);
  if( c==0 ) return 0;
  sqtBeginStmtMap(c, "analyze");
  if( pName2 && pName2->n ){
    sqtKTok(c->node, "schema", pName1);
    sqtKTok(c->node, "name", pName2);
  }else if( pName1 && pName1->n ){
    sqtKTok(c->node, "name", pName1);
  }
  sqtMapEnd(c->node);
  return 1;
}

int sqltreeVacuum(Parse *pParse, Token *pNm, Expr *pInto){
  SqtCtx *c = sqtCtx(pParse);
  if( c==0 ) return 0;
  sqtBeginStmtMap(c, "vacuum");
  if( pNm && pNm->n ) sqtKTok(c->node, "schema", pNm);
  if( pInto ){ sqtKey(c->node, "into"); sqtExpr(c, pInto); }
  sqtMapEnd(c->node);
  return 1;
}

int sqltreeReindex(Parse *pParse, Token *pName1, Token *pName2){
  SqtCtx *c = sqtCtx(pParse);
  if( c==0 ) return 0;
  sqtBeginStmtMap(c, "reindex");
  if( pName2 && pName2->n ){
    sqtKTok(c->node, "schema", pName1);
    sqtKTok(c->node, "name", pName2);
  }else if( pName1 && pName1->n ){
    sqtKTok(c->node, "collation-or-name", pName1);
  }
  sqtMapEnd(c->node);
  return 1;
}

int sqltreeRenameTable(Parse *pParse, SrcList *pSrc, Token *pName){
  SqtCtx *c = sqtCtx(pParse);
  if( c==0 ) return 0;
  sqtBeginStmtMap(c, "rename-table");
  sqtKey(c->node, "table"); sqtSrcItem(c, &pSrc->a[0], 0);
  sqtKTok(c->node, "to", pName);
  sqtMapEnd(c->node);
  return 1;
}

int sqltreeRenameColumn(Parse *pParse, SrcList *pSrc, Token *pOld, Token *pNew){
  SqtCtx *c = sqtCtx(pParse);
  if( c==0 ) return 0;
  sqtBeginStmtMap(c, "rename-column");
  sqtKey(c->node, "table"); sqtSrcItem(c, &pSrc->a[0], 0);
  sqtKTok(c->node, "from", pOld);
  sqtKTok(c->node, "to", pNew);
  sqtMapEnd(c->node);
  return 1;
}

int sqltreeAlterDropConstraint(Parse *pParse, SrcList *pSrc, Token *pCons,
                               Token *pCol){
  SqtCtx *c = sqtCtx(pParse);
  if( c==0 ) return 0;
  sqtBeginStmtMap(c, "alter-drop-constraint");
  sqtKey(c->node, "table"); sqtSrcItem(c, &pSrc->a[0], 0);
  if( pCons && pCons->n ) sqtKTok(c->node, "constraint", pCons);
  if( pCol && pCol->n ) sqtKTok(c->node, "column", pCol);
  sqtMapEnd(c->node);
  return 1;
}

int sqltreeAlterSetNotNull(Parse *pParse, SrcList *pSrc, Token *pCol,
                           Token *pFirst){
  SqtCtx *c = sqtCtx(pParse);
  if( c==0 ) return 0;
  (void)pFirst;
  sqtBeginStmtMap(c, "alter-set-not-null");
  sqtKey(c->node, "table"); sqtSrcItem(c, &pSrc->a[0], 0);
  sqtKTok(c->node, "column", pCol);
  sqtMapEnd(c->node);
  return 1;
}

int sqltreeAlterAddConstraint(Parse *pParse, SrcList *pSrc, Token *pFirst,
                              Token *pName, const char *zExpr, int nExpr,
                              Expr *pExpr){
  SqtCtx *c = sqtCtx(pParse);
  if( c==0 ) return 0;
  (void)pFirst;
  sqtBeginStmtMap(c, "alter-add-constraint");
  sqtKey(c->node, "table"); sqtSrcItem(c, &pSrc->a[0], 0);
  if( pName && pName->n ) sqtKTok(c->node, "name", pName);
  if( zExpr && nExpr>0 ){
    sqtKey(c->node, "text");
    sqtStrN(c->node, zExpr, nExpr);
  }
  if( pExpr ){ sqtKey(c->node, "expr"); sqtExpr(c, pExpr); }
  sqtMapEnd(c->node);
  return 1;
}

int sqltreeAlterDropColumn(Parse *pParse, SrcList *pSrc, const Token *pName){
  SqtCtx *c = sqtCtx(pParse);
  if( c==0 ) return 0;
  sqtBeginStmtMap(c, "drop-column");
  sqtKey(c->node, "table"); sqtSrcItem(c, &pSrc->a[0], 0);
  sqtKTok(c->node, "column", pName);
  sqtMapEnd(c->node);
  return 1;
}

int sqltreeAlterFinishAddColumn(Parse *pParse, Token *pColDef){
  SqtCtx *c = sqtCtx(pParse);
  Table *pNew = pParse->pNewTable;
  if( c==0 ) return 0;
  if( pNew==0 || pNew->nCol==0 ) return 0;
  sqtBeginStmtMap(c, "add-column");
  /* pNewTable is the working copy of the table including the new column */
  sqtKey(c->node, "table");
  sqtTableDef(c, pNew);
  sqtKTok(c->node, "definition", pColDef);
  sqtMapEnd(c->node);
  return 1;
}

/* ------------------------------------------------------------------ */
/* C API                                                              */
/* ------------------------------------------------------------------ */

SQT_API const char *sqt_version(void){
  return SQLITE_VERSION;
}

SQT_API void *sqt_open(const char *zPath){
  SqtSession *pS;
  sqlite3 *db = 0;
  int rc;
  if( zPath==0 || zPath[0]==0 ) zPath = ":memory:";
  rc = sqlite3_open(zPath, &db);
  if( rc!=SQLITE_OK ){
    if( db ) sqlite3_close(db);
    return 0;
  }
  /* Force schema load the way the first prepare on a normal
  ** connection would. */
  sqlite3_exec(db, "SELECT 1;", 0, 0, 0);
  pS = (SqtSession*)sqlite3_malloc(sizeof(SqtSession));
  if( pS==0 ){
    sqlite3_close(db);
    return 0;
  }
  pS->db = db;
  return (void*)pS;
}

SQT_API void sqt_close(void *h){
  SqtSession *pS = (SqtSession*)h;
  if( pS==0 ) return;
  sqlite3_close(pS->db);
  sqlite3_free(pS);
}

SQT_API char *sqt_load_schema(void *h, const char *zDdl){
  SqtSession *pS = (SqtSession*)h;
  char *zErr = 0;
  int rc;
  if( pS==0 || zDdl==0 ) return 0;
  rc = sqlite3_exec(pS->db, zDdl, 0, 0, &zErr);
  if( rc!=SQLITE_OK ){
    sqlite3_str *pOut = sqlite3_str_new(0);
    char *z;
    sqlite3_str_appendall(pOut, "{:error ");
    sqtStr(pOut, zErr ? zErr : sqlite3ErrStr(rc));
    sqlite3_str_appendall(pOut, "}");
    sqlite3_free(zErr);
    z = sqlite3_str_finish(pOut);
    return z;   /* caller frees with sqt_free */
  }
  return 0;
}

SQT_API char *sqt_parse(void *h, const char *zSql){
  SqtSession *pS = (SqtSession*)h;
  sqlite3 *db;
  Parse sParse;
  SqtCtx ctx;
  sqlite3_str *pTop;
  char *zRes;

  if( pS==0 || zSql==0 ) return 0;
  db = pS->db;
  pTop = sqlite3_str_new(0);

  sqlite3_mutex_enter(db->mutex);
  sqlite3BtreeEnterAll(db);

  memset(PARSE_HDR(&sParse), 0, PARSE_HDR_SZ);
  memset(PARSE_TAIL(&sParse), 0, PARSE_TAIL_SZ);
  sParse.pOuterParse = db->pParse;
  db->pParse = &sParse;
  sParse.db = db;

  ctx.pParse = &sParse;
  ctx.out = sqlite3_str_new(0);
  ctx.node = sqlite3_str_new(0);
  ctx.zSql = zSql;
  ctx.zStmtStart = zSql;
  ctx.hasNode = 0;
  ctx.depth = 0;
  g_ctx = &ctx;

  sqlite3ReadSchema(&sParse);
  sqlite3RunParser(&sParse, zSql);

  g_ctx = 0;

  if( sParse.nErr || sParse.zErrMsg || sParse.rc!=SQLITE_OK ){
    /* the failing statement never reaches FinishCoding; emit it as an
    ** error envelope from the current statement start to where parsing
    ** stopped */
    sqlite3_str *o = ctx.out;
    const char *zErrPos = sParse.zTail ? sParse.zTail : zSql;
    sqtMap(o);
    sqtKKw(o, "node", "error");
    sqtKStr(o, "message", sParse.zErrMsg ? sParse.zErrMsg
                                         : sqlite3ErrStr(sParse.rc));
    sqtKInt(o, "offset", (int)(zErrPos - zSql));
    sqtMapEnd(o);
    sqlite3_str_appendchar(o, 1, '\n');
  }

  db->pParse = sParse.pOuterParse;
  sqlite3ParseObjectReset(&sParse);
  sqlite3BtreeLeaveAll(db);
  sqlite3_mutex_leave(db->mutex);

  sqlite3_str_appendall(pTop, "{:sqlite ");
  sqtStr(pTop, SQLITE_VERSION);
  sqlite3_str_appendall(pTop, " :statements [");
  {
    const char *zBody = sqlite3_str_value(ctx.out);
    if( zBody ) sqlite3_str_appendall(pTop, zBody);
  }
  sqlite3_str_appendall(pTop, "]}");
  sqlite3_free(sqlite3_str_finish(ctx.out));
  sqlite3_free(sqlite3_str_finish(ctx.node));

  zRes = sqlite3_str_finish(pTop);
  if( zRes==0 ){
    zRes = (char*)sqlite3_malloc(1);
    if( zRes ) zRes[0] = 0;
  }
  return zRes;   /* caller frees with sqt_free */
}

SQT_API void sqt_free(char *z){
  sqlite3_free(z);
}

/* ------------------------------------------------------------------ */
/* JNI bindings                                                       */
/* ------------------------------------------------------------------ */

#ifdef SQT_ENABLE_JNI
#include <jni.h>

static jstring sqtJniStr(JNIEnv *env, const char *z){
  return (*env)->NewStringUTF(env, z ? z : "");
}

static char *sqtJniCStr(JNIEnv *env, jstring s){
  const char *z = (*env)->GetStringUTFChars(env, s, 0);
  jsize n = (*env)->GetStringUTFLength(env, s);
  char *c;
  if( z==0 ) return 0;
  c = (char*)sqlite3_malloc(n + 1);
  if( c ){
    memcpy(c, z, n);
    c[n] = 0;
  }
  (*env)->ReleaseStringUTFChars(env, s, z);
  return c;
}

JNIEXPORT jstring JNICALL
Java_parseq_native_Sqltree_version(JNIEnv *env, jclass cls){
  (void)cls;
  return sqtJniStr(env, sqt_version());
}

JNIEXPORT jlong JNICALL
Java_parseq_native_Sqltree_open(JNIEnv *env, jclass cls, jstring path){
  char *zPath;
  void *h;
  (void)cls;
  zPath = sqtJniCStr(env, path);
  if( zPath==0 ) return 0;
  h = sqt_open(zPath);
  sqlite3_free(zPath);
  return (jlong)(intptr_t)h;
}

JNIEXPORT void JNICALL
Java_parseq_native_Sqltree_close(JNIEnv *env, jclass cls, jlong h){
  (void)env; (void)cls;
  sqt_close((void*)(intptr_t)h);
}

JNIEXPORT jstring JNICALL
Java_parseq_native_Sqltree_parse(JNIEnv *env, jclass cls, jlong h,
                                 jstring sql){
  char *zSql;
  char *zRes;
  jstring r;
  (void)cls;
  zSql = sqtJniCStr(env, sql);
  if( zSql==0 ) return 0;
  zRes = sqt_parse((void*)(intptr_t)h, zSql);
  sqlite3_free(zSql);
  if( zRes==0 ) return 0;
  r = sqtJniStr(env, zRes);
  sqt_free(zRes);
  return r;
}

JNIEXPORT jstring JNICALL
Java_parseq_native_Sqltree_loadSchema(JNIEnv *env, jclass cls, jlong h,
                                      jstring ddl){
  char *zDdl;
  char *zRes;
  jstring r = 0;
  (void)cls;
  zDdl = sqtJniCStr(env, ddl);
  if( zDdl==0 ) return 0;
  zRes = sqt_load_schema((void*)(intptr_t)h, zDdl);
  sqlite3_free(zDdl);
  if( zRes ){
    r = sqtJniStr(env, zRes);
    sqt_free(zRes);
  }
  return r;
}
#endif /* SQT_ENABLE_JNI */
