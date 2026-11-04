/*
** sqltree_hooks.h -- parse-coverage hooks between the vendored SQLite
** amalgamation and the parseq serializer (sqltree.c).
**
** The amalgamation is compiled together with sqltree.c into a single
** translation unit (see sqltree_all.c), and parsecov.patch inserts
** guarded calls to these functions at the boundary in SQLite where a
** statement's parse tree is fully name-resolved but code generation
** has not started yet. Each hook returns 1 when it has taken over the
** statement (parse-capture mode), telling the caller to skip SQLite's
** own code-generation path.
**
** Every hook is a no-op unless parse-capture is active for the given
** Parse object; outside of sqt_parse() the library behaves exactly
** like stock SQLite.
*/
#ifndef PARSEQ_SQLTREE_HOOKS_H
#define PARSEQ_SQLTREE_HOOKS_H

/* Statement lifecycle */
int sqltreeStmtFinish(Parse *pParse);                    /* sqlite3FinishCoding */
int sqltreeValuesRow(Parse *pParse, Select *pBody, ExprList *pRow); /* sqlite3MultiValues */
int sqltreeSelect(Parse *pParse, Select *p);             /* sqlite3Select */
int sqltreeInsert(Parse *pParse, SrcList *pTab, Select *pSel, IdList *pCols,
                  int onError, Upsert *pUpsert);
int sqltreeUpdate(Parse *pParse, SrcList *pTab, ExprList *pChanges, Expr *pWhere,
                  int onError, ExprList *pOrderBy, Expr *pLimit, Upsert *pUpsert);
int sqltreeDelete(Parse *pParse, SrcList *pTab, Expr *pWhere, ExprList *pOrderBy,
                  Expr *pLimit);
int sqltreeEndTable(Parse *pParse, u32 tabOpts, Select *pSelect);
int sqltreeCreateIndex(Parse *pParse, Token *pName1, Token *pName2,
                       SrcList *pTblName, ExprList *pList, int onError,
                       Token *pStart, Expr *pPIWhere, int sortOrder,
                       int ifNotExist, u8 idxType);
int sqltreeCreateView(Parse *pParse, Token *pName1, Token *pName2,
                      ExprList *pCNames, Select *pSel, int isTemp, int noErr);
int sqltreeFinishTrigger(Parse *pParse, TriggerStep *pSteps, Token *pAll);
int sqltreePragma(Parse *pParse, Token *pId1, Token *pId2, Token *pValue,
                  int minusFlag);
int sqltreeDropTable(Parse *pParse, SrcList *pName, int isView, int noErr);
int sqltreeDropIndex(Parse *pParse, SrcList *pName, int ifExists);
int sqltreeDropTrigger(Parse *pParse, SrcList *pName, int noErr);
int sqltreeBeginTxn(Parse *pParse, int type);
int sqltreeEndTxn(Parse *pParse, int type);
int sqltreeSavepoint(Parse *pParse, int op, Token *pName);
int sqltreeAttach(Parse *pParse, Expr *pFile, Expr *pDb, Expr *pKey);
int sqltreeDetach(Parse *pParse, Expr *pDb);
int sqltreeAnalyze(Parse *pParse, Token *pName1, Token *pName2);
int sqltreeVacuum(Parse *pParse, Token *pNm, Expr *pInto);
int sqltreeReindex(Parse *pParse, Token *pName1, Token *pName2);
int sqltreeRenameTable(Parse *pParse, SrcList *pSrc, Token *pName);
int sqltreeRenameColumn(Parse *pParse, SrcList *pSrc, Token *pOld, Token *pNew);
int sqltreeAlterDropConstraint(Parse *pParse, SrcList *pSrc, Token *pCons,
                               Token *pCol);
int sqltreeAlterSetNotNull(Parse *pParse, SrcList *pSrc, Token *pCol,
                           Token *pFirst);
int sqltreeAlterAddConstraint(Parse *pParse, SrcList *pSrc, Token *pFirst,
                              Token *pName, const char *zExpr, int nExpr,
                              Expr *pExpr);
int sqltreeAlterDropColumn(Parse *pParse, SrcList *pSrc, const Token *pName);
int sqltreeAlterFinishAddColumn(Parse *pParse, Token *pColDef);

#endif /* PARSEQ_SQLTREE_HOOKS_H */
