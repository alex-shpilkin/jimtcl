#include <string.h>

#include "jimautoconf.h"
#include "jim.h"
#include "jim-ensemble.h"

/* ------------------------------------------------------------------------
 * Ensemble command object
 * ------------------------------------------------------------------------ */

#define JimSelectorValue(o) ((JimSelector *)((o)->internalRep.ptr))

typedef struct JimSelector {
    int refCount;
    unsigned long procEpoch;
    Jim_Ensemble *base;
    Jim_Obj *cmdObj;
} JimSelector;

static void FreeSelectorInternalRep(Jim_Interp *interp, Jim_Obj *obj)
{
    JimSelector *sel = JimSelectorValue(obj);
    if (--(sel->refCount) == 0) {
        Jim_DecrRefCount(interp, sel->cmdObj);
        Jim_Free(sel);
    }
}

static void DupSelectorInternalRep(Jim_Interp *interp, Jim_Obj *oldObj, Jim_Obj *newObj)
{
    newObj->typePtr = oldObj->typePtr;
    newObj->internalRep.ptr = oldObj->internalRep.ptr;
    JimSelectorValue(oldObj)->refCount++;
}

static Jim_ObjType selectorObjType = {
    "selector",
    FreeSelectorInternalRep,
    DupSelectorInternalRep,
    NULL,
    JIM_TYPE_REFERENCES
};

/* ------------------------------------------------------------------------
 * Ensemble implementation
 * ------------------------------------------------------------------------ */

static Jim_Obj *JimResolveSelector(Jim_Interp *interp, const char *baseName,
    Jim_Ensemble *base, Jim_Obj *selObj, int flags);
static int JimRewriteAlias(Jim_Interp *interp, int *objcPtr,
    Jim_Obj ***objvPtr);
static Jim_Obj *JimUnknownSelector(Jim_Interp *interp, const char *baseName,
    Jim_Ensemble *base);
static void JimInsufficientArgs(Jim_Interp *interp, Jim_Obj *ensemble);

static int JimEnsembleCmdProc(Jim_Interp *interp, int argc, Jim_Obj *const *argv);
static void JimEnsembleDelProc(Jim_Interp *interp, void *privData);

Jim_Ensemble *Jim_GetEnsemble(Jim_Interp *interp, Jim_Obj *cmdObj)
{
    Jim_Cmd *cmd;

    cmd = Jim_GetCommand(interp, cmdObj, JIM_ERR);
    if (!cmd || cmd->isproc || cmd->u.native.cmdProc != JimEnsembleCmdProc)
        return NULL;

    return cmd->u.native.privData;
}

static Jim_Obj *JimResolveSelector(Jim_Interp *interp, const char *baseName,
    Jim_Ensemble *base, Jim_Obj *selObj, int flags)
{
    const char *selName;
    char *name;
    int selLen, baseLen;
    Jim_Obj *cmdObj;
    JimSelector *sel;

    if (selObj->typePtr == &selectorObjType &&
        JimSelectorValue(selObj)->procEpoch == interp->procEpoch &&
        JimSelectorValue(selObj)->base == base)
    {
        cmdObj = JimSelectorValue(selObj)->cmdObj;
        if (Jim_GetCommand(interp, cmdObj, flags) != NULL) {
            return cmdObj;
        } else {
            Jim_FreeIntRep(interp, selObj);
            return NULL;
        }
    }

    selName = Jim_GetString(selObj, &selLen);
    baseLen = strlen(baseName);

    name = Jim_Alloc(baseLen + selLen + 2);
    memcpy(name, baseName, baseLen);
    name[baseLen] = ' ';
    memcpy(name + baseLen + 1, selName, selLen);
    name[baseLen + selLen + 1] = '\0';

    cmdObj = Jim_NewStringObjNoAlloc(interp, name, baseLen + selLen + 1);
    Jim_IncrRefCount(cmdObj);

    if (Jim_GetCommand(interp, cmdObj, flags) == NULL) {
        Jim_DecrRefCount(interp, cmdObj);
        return NULL;
    }

    Jim_FreeIntRep(interp, selObj);
    selObj->typePtr = &selectorObjType;
    selObj->internalRep.ptr = sel = Jim_Alloc(sizeof(JimSelector));

    sel->refCount = 1;
    sel->procEpoch = interp->procEpoch;
    sel->base = base;
    sel->cmdObj = cmdObj; /* transfer ownership */

    return cmdObj;
}

static int JimRewriteAlias(Jim_Interp *interp, int *objcPtr,
    Jim_Obj ***objvPtr)
{
    int prefc;
    Jim_Obj *const *prefv;
    int objc = *objcPtr;
    Jim_Obj **objv = *objvPtr;

    if (Jim_ResolveAlias(interp, objv[0], &prefc, &prefv) != JIM_OK)
        return JIM_ERR;

    if (prefc == 1) {
        objv[0] = prefv[0];
    }
    else if (prefc > 0) {
        objv = Jim_Realloc(objv, sizeof(*objv) * (prefc + objc - 1));
        memmove(objv + prefc, objv + 1, sizeof(*objv) * (objc - 1));
        memcpy(objv, prefv, sizeof(*objv) * prefc);
        objc += prefc - 1;
    }

    *objcPtr = objc;
    *objvPtr = objv;

    return JIM_OK;
}

int Jim_ResolvePrefix(Jim_Interp *interp, int objc, Jim_Obj *const *objv,
    int *rescPtr, Jim_Obj ***resvPtr)
{
    int i, ridx, widx;
    Jim_Obj **resv;
    Jim_Obj *cmdObj;
    Jim_Ensemble *base;
    int ret;

    ret = JIM_OK;

    resv = Jim_Alloc(sizeof(*resv));
    ridx = 0; widx = 1;
    cmdObj = objv[0];

    do {
        resv[0] = cmdObj;
        if ((ret = JimRewriteAlias(interp, &widx, &resv)) != JIM_OK)
            break;
        /* Consider it consumed now that we know it's an existing command */
        ridx++;

        base = Jim_GetEnsemble(interp, resv[0]);
        if (base == NULL || widx > base->arity + 1)
            break;

        if (widx < base->arity + 1)
            resv = Jim_Realloc(resv, sizeof(*resv) * (base->arity + 1));
        for (i = widx - 1; ridx < objc && i < base->arity; i++) {
            if (Jim_CompareStringImmediate(interp, objv[ridx], ".."))
                ridx++;
            else
                resv[widx++] = objv[ridx++];
        }

        if (ridx == objc) {
            /* Not enough arguments to get the selector */
            break;
        }

        cmdObj = JimResolveSelector(interp, Jim_String(resv[0]), base, objv[ridx], JIM_ERRMSG);
    } while (cmdObj);

    objc -= ridx;
    resv = Jim_Realloc(resv, sizeof(*resv) * (widx + objc));
    memcpy(resv + widx, objv + ridx, sizeof(*resv) * objc);

    *rescPtr = widx + objc;
    *resvPtr = resv;

    return ret;
}

static Jim_Obj *JimUnknownSelector(Jim_Interp *interp, const char *baseName,
    Jim_Ensemble *base)
{
    if (base->unknown == NULL) {
        base->unknown = Jim_NewStringObj(interp, "unknown", -1);
        Jim_IncrRefCount(base->unknown);
    }

    return JimResolveSelector(interp, baseName, base, base->unknown, JIM_NONE);
}

static void JimInsufficientArgs(Jim_Interp *interp, Jim_Obj *ensemble)
{
    Jim_Ensemble *base;
    Jim_Obj *obj, *listObj;

    base = Jim_GetEnsemble(interp, ensemble);
    listObj = Jim_NewListObj(interp, &ensemble, 1);

    Jim_ListAppendList(interp, listObj, base->argList);
    Jim_IncrRefCount(listObj);
    obj = Jim_ListJoin(interp, listObj, " ", 1);
    Jim_DecrRefCount(interp, listObj);

    Jim_IncrRefCount(obj);
    Jim_SetResultFormatted(interp, "wrong # args: should be \"%#s subcommand ?args ...?\"", 
        obj, base->argList);
    Jim_DecrRefCount(interp, obj);
}

static int JimEnsembleCmdProc(Jim_Interp *interp, int argc, Jim_Obj *const *argv)
{
    int resc;
    Jim_Obj **resv;

    int ret;
    Jim_Ensemble *base;

    /* NB. The computation down here is uncacheable, unlike the one in
     * Jim_ResolvePrefix, because we wouldn't know when the [unknown] lookup
     * becomes invalid.
     */

    Jim_ResolvePrefix(interp, argc, argv, &resc, &resv);

    if ((base = Jim_GetEnsemble(interp, resv[0])) != NULL) {
        if (resc - 1 < base->arity + 1) {
            JimInsufficientArgs(interp, resv[0]);
            ret = JIM_ERR;
            goto cleanup;
        }
        else {
            resv[0] = JimUnknownSelector(interp, Jim_String(resv[0]), base);
            if (resv[0] == NULL) {
                ret = JIM_ERR;
                goto cleanup;
            }
        }
    }

    ret = Jim_EvalObjVector(interp, resc, resv);

  cleanup:
    Jim_Free(resv);
    return ret;
}

static void JimEnsembleDelProc(Jim_Interp *interp, void *privData)
{
    Jim_Ensemble *ens = privData;

    Jim_DecrRefCount(interp, ens->argList);
    if (ens->delProc != NULL)
        ens->delProc(interp, ens->privData);
    if (ens->unknown != NULL)
        Jim_DecrRefCount(interp, ens->unknown);

    Jim_Free(ens);
}

int Jim_CreateEnsemble(Jim_Interp *interp, const char *name, Jim_Obj *argList, void *privData, Jim_DelCmdProc delProc)
{
    Jim_Ensemble *ens = Jim_Alloc(sizeof(Jim_Ensemble));

    if (argList != NULL)
        ens->argList = argList;
    else
        ens->argList = interp->emptyObj;
    Jim_IncrRefCount(ens->argList);
    ens->arity = Jim_ListLength(interp, ens->argList);

    ens->privData = privData;
    ens->delProc = delProc;
    ens->unknown = NULL;

    Jim_CreateCommand(interp, name, JimEnsembleCmdProc, ens, JimEnsembleDelProc);
    return JIM_OK;
}

/* [ensemble] */
static int JimEnsembleCommand(Jim_Interp *interp, int argc, Jim_Obj *const *argv)
{
    if (argc < 2 || argc > 3) {
        Jim_WrongNumArgs(interp, 1, argv, "name");
        return JIM_ERR;
    }

    Jim_SetResult(interp, argv[1]);
    return Jim_CreateEnsemble(interp, Jim_String(argv[1]),
        argc > 2 ? argv[2] : NULL, NULL, NULL);
}

int Jim_ensembleInit(Jim_Interp *interp)
{
    if (Jim_PackageProvide(interp, "ensemble", "0.1", JIM_ERRMSG) != JIM_OK)
        return JIM_ERR;

    Jim_CreateCommand(interp, "ensemble", JimEnsembleCommand, NULL, NULL);
    return JIM_OK;
}
