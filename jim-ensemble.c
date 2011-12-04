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
    long magic;
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

typedef struct JimEnsemble {
    long magic;
    Jim_Obj *unknownSel, *itselfSel;
} JimEnsemble;

static Jim_Obj *JimResolveSelector(Jim_Interp *interp, const char *baseName,
    JimEnsemble *base, Jim_Obj *selObj, int flags);
static JimEnsemble *JimGetEnsemble(Jim_Interp *interp, Jim_Obj *cmdObj);
int JimEnsembleCmdProc(Jim_Interp *interp, int argc, Jim_Obj *const *argv);

static Jim_Obj *JimResolveSelector(Jim_Interp *interp, const char *baseName,
    JimEnsemble *base, Jim_Obj *selObj, int flags)
{
    const char *selName;
    char *name;
    int selLen, baseLen;
    Jim_Obj *cmdObj;
    JimSelector *sel;

    if (selObj->typePtr == &selectorObjType && JimSelectorValue(selObj)->magic == base->magic) {
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
    memcpy(&name[baseLen + 1], selName, selLen);
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
    sel->magic = base->magic;
    sel->cmdObj = cmdObj; /* transfer ownership */

    return cmdObj;
}

static JimEnsemble *JimGetEnsemble(Jim_Interp *interp, Jim_Obj *cmdObj)
{
    Jim_Cmd *cmd;

    cmd = Jim_GetCommand(interp, cmdObj, 0);
    if (cmd->isproc || cmd->u.native.cmdProc != JimEnsembleCmdProc)
        return NULL;

    return cmd->u.native.privData;
}

int JimEnsembleCmdProc(Jim_Interp *interp, int argc, Jim_Obj *const *argv)
{
    int i;
    JimEnsemble *ens;
    Jim_Obj *cmdObj, *newCmdObj;

    cmdObj = argv[0];
    i = 1;

    while ((ens = JimGetEnsemble(interp, cmdObj)) != NULL) {
        if (i < argc) {
            newCmdObj = JimResolveSelector(interp, Jim_String(cmdObj), ens, argv[i], JIM_ERR);
            if (newCmdObj) {
                cmdObj = newCmdObj;
                i++;
            }
            else {
                if (ens->unknownSel == NULL) {
                    ens->unknownSel = Jim_NewStringObj(interp, "unknown", -1);
                    Jim_IncrRefCount(ens->unknownSel);
                }

                cmdObj = JimResolveSelector(interp, Jim_String(cmdObj), ens, ens->unknownSel, 0);
                if (!cmdObj)
                    return JIM_ERR;
            }
        }
        else {
            if (ens->itselfSel == NULL) {
                ens->itselfSel = Jim_NewStringObj(interp, "itself", -1);
                Jim_IncrRefCount(ens->itselfSel);
            }

            newCmdObj = JimResolveSelector(interp, Jim_String(cmdObj), ens, ens->itselfSel, 0);
            if (!newCmdObj) {
                Jim_WrongNumArgs(interp, 1, &cmdObj, "selector ?args ...?");
                return JIM_ERR;
            }
            else {
                cmdObj = newCmdObj;
                break;
            }
        }
    }

    return Jim_EvalObjPrefix(interp, cmdObj, argc - i, argv + i);
}

void JimEnsembleDelProc(Jim_Interp *interp, void *privData)
{
    JimEnsemble *ens = privData;

    if (ens->unknownSel)
        Jim_DecrRefCount(interp, ens->unknownSel);
    if (ens->itselfSel)
        Jim_DecrRefCount(interp, ens->itselfSel);

    Jim_Free(ens);
}

int Jim_CreateEnsemble(Jim_Interp *interp, const char *name)
{
    JimEnsemble *ens = Jim_Alloc(sizeof(JimEnsemble));
    ens->magic = Jim_GetId(interp);
    ens->unknownSel = ens->itselfSel = NULL;

    Jim_CreateCommand(interp, name, JimEnsembleCmdProc, ens, JimEnsembleDelProc);
    return JIM_OK;
}

/* [ensemble] */
int JimEnsembleCommand(Jim_Interp *interp, int argc, Jim_Obj *const *argv)
{
    if (argc != 2) {
        Jim_WrongNumArgs(interp, 1, argv, "name");
        return JIM_ERR;
    }

    Jim_SetResult(interp, argv[1]);
    return Jim_CreateEnsemble(interp, Jim_String(argv[1]));
}

/* [delegate] */
int JimDelegateCommand(Jim_Interp *interp, int argc, Jim_Obj *const *argv)
{
    Jim_SetResultString(interp, "not implemented", -1);
    return JIM_ERR;
}

int Jim_ensembleInit(Jim_Interp *interp)
{
    if (Jim_PackageProvide(interp, "ensemble", "0.1", JIM_ERRMSG) != JIM_OK)
        return JIM_ERR;

    Jim_CreateCommand(interp, "ensemble", JimEnsembleCommand, NULL, NULL);
    Jim_CreateCommand(interp, "delegate", JimDelegateCommand, NULL, NULL);

    return JIM_OK;
}
