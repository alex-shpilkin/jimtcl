#ifndef JIM_ENSEMBLE_H
#define JIM_ENSEMBLE_H

#include "jim.h"

typedef struct Jim_Ensemble {
    int arity;
    Jim_Obj *argList;
    void *privData;
    Jim_DelCmdProc delProc;

    Jim_Obj *unknown; /* for caching */
} Jim_Ensemble;

JIM_EXPORT int Jim_CreateEnsemble (Jim_Interp *interp, const char *name,
    Jim_Obj *argList, void *privData, Jim_DelCmdProc delProc);
JIM_EXPORT Jim_Ensemble *Jim_GetEnsemble (Jim_Interp *interp, Jim_Obj *cmdObj);

#endif /* defined(JIM_ENSEMBLE_H) */