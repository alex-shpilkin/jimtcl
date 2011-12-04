#include <string.h>
#include <stdio.h>

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
} JimEnsemble;

static int SetSelectorFromAny(Jim_Interp *interp, Jim_Obj *obj, const char *baseName, JimEnsemble *base)
{
    const char *selName;
    char *name;
    int selLen, baseLen;
    Jim_Obj *cmdObj;
    JimSelector *sel;

    selName = Jim_GetString(obj, &selLen);
    baseLen = strlen(baseName);

    name = Jim_Alloc(baseLen + selLen + 2);
    memcpy(name, baseName, baseLen);
    name[baseLen] = ' ';
    memcpy(&name[baseLen + 1], selName, selLen);
    name[baseLen + selLen + 1] = '\0';

    cmdObj = Jim_NewStringObjNoAlloc(interp, name, baseLen + selLen + 1);
    Jim_IncrRefCount(cmdObj);

    if (Jim_GetCommand(interp, cmdObj, JIM_ERRMSG) == NULL) {
        Jim_DecrRefCount(interp, cmdObj);
        return JIM_ERR;
    }

    Jim_FreeIntRep(interp, obj);
    obj->typePtr = &selectorObjType;
    obj->internalRep.ptr = sel = Jim_Alloc(sizeof(JimSelector));

    sel->refCount = 1;
    sel->magic = base->magic;
    sel->cmdObj = cmdObj; /* transfer ownership */

    return JIM_OK;
}

int JimEnsembleCmdProc(Jim_Interp *interp, int argc, Jim_Obj *const *argv)
{
    int i;
    JimEnsemble *ens;
    Jim_Obj *cmdObj;
    Jim_Cmd *cmd;

    cmdObj = argv[0]; i = 1;
    cmd = Jim_GetCommand(interp, cmdObj, 0);
    /* assert(cmd); */

    while (!cmd->isproc && cmd->u.native.cmdProc == JimEnsembleCmdProc) {
        if (argc < i) {
            Jim_WrongNumArgs(interp, 1, &cmdObj, "selector ?args ..?");
            return JIM_ERR;
        }

        ens = cmd->u.native.privData;
        if (argv[i]->typePtr != &selectorObjType || JimSelectorValue(argv[i])->magic != ens->magic) {
            if (SetSelectorFromAny(interp, argv[i], Jim_String(cmdObj), ens) != JIM_OK)
                return JIM_ERR;
        }

        cmdObj = JimSelectorValue(argv[i])->cmdObj;

        cmd = Jim_GetCommand(interp, cmdObj, JIM_ERRMSG);
        if (cmd == NULL) {
            /* This can happen if the proc epoch has been bumped since
             * the cached lookup. Invalidate the cached result. */

            Jim_FreeIntRep(interp, argv[i]);
            argv[i]->typePtr = NULL;
            return JIM_ERR;
        }

        i++;
    }

    return Jim_EvalObjPrefix(interp, cmdObj, argc - i, argv + i);
}

void JimEnsembleDelProc(Jim_Interp *interp, void *privData)
{
    JimEnsemble *ens = privData;
    Jim_Free(ens);
}

int Jim_CreateEnsemble(Jim_Interp *interp, const char *name)
{
    JimEnsemble *ens = Jim_Alloc(sizeof(JimEnsemble));
    ens->magic = Jim_GetId(interp);
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
