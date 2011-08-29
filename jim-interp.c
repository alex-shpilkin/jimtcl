#include <assert.h>

#include "jim.h"
#include "jimautoconf.h"
#include "jim-subcmd.h"

struct InterpInfo
{
    Jim_Interp *interp;
};

static void JimInterpDelProc(Jim_Interp *interp, void *privData)
{
    struct InterpInfo *iis = privData;

    Jim_FreeInterp(iis->interp);
    Jim_Free(iis);
}

static int JimInterpCrossInterpEval(Jim_Interp *target, Jim_Interp *source, Jim_Obj *scriptObj)
{
    int ret;
    int len;
    Jim_Obj *targetScriptObj;
    const char *script;

    /* Create a string copy of the script in the target interp */
    script = Jim_GetString(scriptObj, &len);
    targetScriptObj = Jim_NewStringObj(target, script, len);

    /* Evaluate it */
    Jim_IncrRefCount(targetScriptObj);
    ret = Jim_EvalObj(target, targetScriptObj);
    Jim_DecrRefCount(target, targetScriptObj);

    /* And extract the result */
    Jim_SetResultString(source, Jim_String(Jim_GetResult(target)), -1);

    return ret;
}

static int interp_cmd_eval(Jim_Interp *interp, int argc, Jim_Obj *const *argv)
{
    int ret;
    Jim_Interp *subinterp = ((struct InterpInfo *)Jim_CmdPrivData(interp))->interp;

    /* Evaluate the command in the given interpreter.
     * Everything passing between the two interpreters must be converted to
     * a string first.
     */
    Jim_Obj *scriptStringObj = Jim_ConcatObj(interp, argc, argv);

    ret = JimInterpCrossInterpEval(subinterp, interp, scriptStringObj);

    Jim_FreeNewObj(interp, scriptStringObj);

    return ret;
}

static int interp_cmd_delete(Jim_Interp *interp, int argc, Jim_Obj *const *argv)
{
    return Jim_RenameCommand(interp, Jim_String(argv[0]), "");
}

static void JimInterpDelObj(Jim_Interp *interp, void *privData)
{
    Jim_DecrRefCount(interp, (Jim_Obj *)privData);
}

static int JimInterpSubCmdAlias(Jim_Interp *interp, int argc, Jim_Obj *const *argv)
{
    int ret;
    int i;
    Jim_Interp *parent = Jim_GetAssocData(interp, "interp.parent");
    Jim_Obj *aliasPrefixList = Jim_CmdPrivData(interp);
    Jim_Obj *cmdList;

    assert(parent);

    /* Build the complete command */
    cmdList = Jim_DuplicateObj(interp, aliasPrefixList);

    for (i = 1; i < argc; i++) {
        Jim_ListAppendElement(interp, cmdList, argv[i]);
    }

    ret = JimInterpCrossInterpEval(parent, interp, cmdList);

    Jim_FreeNewObj(interp, cmdList);

    return ret;
}

static int interp_cmd_alias(Jim_Interp *interp, int argc, Jim_Obj *const *argv)
{
    int i;
    Jim_Interp *subinterp = ((struct InterpInfo *)Jim_CmdPrivData(interp))->interp;
    Jim_Obj *aliasPrefixList;

    /* Create a prefix list in subinterp from argv[1...] */
    aliasPrefixList = Jim_NewListObj(subinterp, NULL, 0);
    for (i = 1; i < argc; i++) {
        Jim_ListAppendElement(subinterp, aliasPrefixList,
            Jim_NewStringObj(subinterp, Jim_String(argv[i]), -1));
    }

    Jim_IncrRefCount(aliasPrefixList);

    /* Create the command in the sub interpreter with the prefix list as the privdata */
    Jim_CreateCommand(subinterp, Jim_String(argv[0]), JimInterpSubCmdAlias, aliasPrefixList, JimInterpDelObj);
    return JIM_OK;
}

static const jim_subcmd_type interp_command_table[] = {
    {   .cmd = "eval",
        .args = "script ...",
        .function = interp_cmd_eval,
        .minargs = 1,
        .maxargs = -1,
        .description = "Concat the args and evaluate the script in the interpreter"
    },
    {   .cmd = "delete",
        .function = interp_cmd_delete,
        .flags = JIM_MODFLAG_FULLARGV,
        .description = "Delete this interpreter"
    },
    {   .cmd = "alias",
        .args = "slavecmd mastercmd ...",
        .function = interp_cmd_alias,
        .minargs = 2,
        .maxargs = -1,
        .description = "Create an alias which refers to a command in the parent interpreter"
    },
    { 0 }
};

static int JimInterpSubCmdProc(Jim_Interp *interp, int argc, Jim_Obj *const *argv)
{
    return Jim_CallSubCmd(interp, Jim_ParseSubCmd(interp, interp_command_table, argc, argv), argc, argv);
}

static void JimInterpCopyVariable(Jim_Interp *target, Jim_Interp *source, const char *var, const char *default_value)
{
    Jim_Obj *value = Jim_GetGlobalVariableStr(source, var, JIM_NONE);
    const char *str;

    if (value) {
        str = Jim_String(value);
    }
    else {
        str = default_value;
    }
    if (str) {
        Jim_SetGlobalVariableStr(target, var, Jim_NewStringObj(target, str, -1));
    }
}

/**
 * [interp] creates a new interpreter.
 */
static int JimInterpCommand(Jim_Interp *interp, int argc, Jim_Obj *const *argv)
{
    struct InterpInfo *iis;
    char buf[32];

    if (argc != 1) {
        Jim_WrongNumArgs(interp, 1, argv, "");
        return JIM_ERR;
    }

    /* Create the interpreter command */
    iis = Jim_Alloc(sizeof(*iis));
    iis->interp = Jim_CreateInterp();
    Jim_RegisterCoreCommands(iis->interp);
    Jim_InitStaticExtensions(iis->interp);

    /* Copy some core variables to the new interpreter */
    JimInterpCopyVariable(iis->interp, interp, "argv", "");
    JimInterpCopyVariable(iis->interp, interp, "argc", "0");
    JimInterpCopyVariable(iis->interp, interp, "argv0", NULL);
    JimInterpCopyVariable(iis->interp, interp, "jim_argv0", NULL);

    /* Allow the slave interpreter to find the parent */
    Jim_SetAssocData(iis->interp, "interp.parent", NULL, interp);

    snprintf(buf, sizeof(buf), "interp.handle%ld", Jim_GetId(interp));
    Jim_CreateCommand(interp, buf, JimInterpSubCmdProc, iis, JimInterpDelProc);
    Jim_SetResultString(interp, buf, -1);
    return JIM_OK;
}

int Jim_interpInit(Jim_Interp *interp)
{
    if (Jim_PackageProvide(interp, "interp", "1.0", JIM_ERRMSG))
        return JIM_ERR;

    Jim_CreateCommand(interp, "interp", JimInterpCommand, NULL, NULL);

    return JIM_OK;
}
