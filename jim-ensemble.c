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

/* Performs selector lookups as long as they succeed. This is exactly the
 * action that can be performed easily without re-invoking the interpreter.
 * Assumes objc > 0. NB. It is correct to cache the result of this function
 * alongside a proc epoch value.
 *
 * Returns JIM_OK if something other than an ensemble command is encountered
 * or the whole chain is consumed; JIM_ERR if a selector for a valid ensemble
 * couldn't be resolved.
 *
 * On exit, *resolvedPtr contains the number of successful lookups, and
 * *cmdObjPtr the result of the last one.
 */

Jim_Obj *Jim_ResolvePrefix(Jim_Interp *interp, int objc, Jim_Obj *const *objv,
    int *lengthPtr)
{
    int i;
    Jim_Obj *cmdObj, *subObj;
    Jim_Ensemble *base;

    i = 0;
    cmdObj = subObj = objv[0];

    while (1) {
        if (subObj != NULL)
            subObj = Jim_ResolveAlias(interp, subObj);
        if (subObj == NULL)
            break;

        cmdObj = subObj;
        base = Jim_GetEnsemble(interp, cmdObj);

        i++;
        if (i >= objc || base == NULL)
            break;

        subObj = JimResolveSelector(interp, Jim_String(cmdObj), base, objv[i], JIM_ERRMSG);
    }

    *lengthPtr = i;
    return cmdObj;
}

static int JimEnsembleCmdProc(Jim_Interp *interp, int argc, Jim_Obj *const *argv)
{
    int resolved;
    Jim_Obj *cmdObj;
    Jim_Ensemble *base;

    /* We're trying hard to lookup chained selectors without re-invoking the
     * interpreter. Jim_ResolvePrefix contains all the lookup logic except the
     * cases when [itself] or [unknown] need to be utilised, at which point we
     * give up and recurse. NB. The computation down here is uncacheable.
     */

    cmdObj = Jim_ResolvePrefix(interp, argc, argv, &resolved);
    if ((base = Jim_GetEnsemble(interp, cmdObj)) != NULL) {
        if (resolved == argc) {
            if (base->itselfSel == NULL) {
                base->itselfSel = Jim_NewStringObj(interp, "itself", -1);
                Jim_IncrRefCount(base->itselfSel);
            }

            /* The problem here is that [itself] may need to be handled by
             * [unknown], so we don't try to perform any more lookups */

            return Jim_EvalObjPrefix(interp, cmdObj, 1, &base->itselfSel);
        }
        else {
            if (base->unknownSel == NULL) {
                base->unknownSel = Jim_NewStringObj(interp, "unknown", -1);
                Jim_IncrRefCount(base->unknownSel);
            }

            cmdObj = JimResolveSelector(interp, Jim_String(cmdObj), base, base->unknownSel, 0);
            if (cmdObj != NULL)
                return Jim_EvalObjPrefix(interp, cmdObj, argc - resolved, argv + resolved);
            else
                return JIM_ERR;
        }
    }

    return Jim_EvalObjPrefix(interp, cmdObj, argc - resolved, argv + resolved);
}

static void JimEnsembleDelProc(Jim_Interp *interp, void *privData)
{
    Jim_Ensemble *ens = privData;

    if (ens->delProc)
        ens->delProc(interp, ens->privData);

    if (ens->unknownSel)
        Jim_DecrRefCount(interp, ens->unknownSel);
    if (ens->itselfSel)
        Jim_DecrRefCount(interp, ens->itselfSel);

    Jim_Free(ens);
}

int Jim_CreateEnsemble(Jim_Interp *interp, const char *name, void *privData, Jim_DelCmdProc delProc)
{
    Jim_Ensemble *ens = Jim_Alloc(sizeof(Jim_Ensemble));
    ens->privData = privData;
    ens->delProc = delProc;
    ens->unknownSel = ens->itselfSel = NULL;

    Jim_CreateCommand(interp, name, JimEnsembleCmdProc, ens, JimEnsembleDelProc);
    return JIM_OK;
}

/* [ensemble] */
static int JimEnsembleCommand(Jim_Interp *interp, int argc, Jim_Obj *const *argv)
{
    if (argc != 2) {
        Jim_WrongNumArgs(interp, 1, argv, "name");
        return JIM_ERR;
    }

    Jim_SetResult(interp, argv[1]);
    return Jim_CreateEnsemble(interp, Jim_String(argv[1]), NULL, NULL);
}

int Jim_ensembleInit(Jim_Interp *interp)
{
    if (Jim_PackageProvide(interp, "ensemble", "0.1", JIM_ERRMSG) != JIM_OK)
        return JIM_ERR;

    Jim_CreateCommand(interp, "ensemble", JimEnsembleCommand, NULL, NULL);
    return JIM_OK;
}
