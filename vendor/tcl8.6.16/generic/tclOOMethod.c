/*
 * tclOOMethod.c --
 *
 *	This file contains code to create and manage methods.
 *
 * Copyright (c) 2005-2011 Donal K. Fellows
 *
 * See the file "license.terms" for information on usage and redistribution of
 * this file, and for a DISCLAIMER OF ALL WARRANTIES.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif
#include "tclInt.h"
#include "tclOOInt.h"
#include "tclCompile.h"

/*
 * Structure used to contain all the information needed about a call frame
 * used in a procedure-like method.
 */

typedef struct {
    CallFrame *framePtr;	/* Reference to the call frame itself (it's
				 * actually allocated on the Tcl stack). */
    ProcErrorProc *errProc;	/* The error handler for the body. */
    Tcl_Obj *nameObj;		/* The "name" of the command. Only used for a
				 * few moments, so not reference. */
} PMFrameData;

/*
 * Structure used to pass information about variable resolution to the
 * on-the-ground resolvers used when working with resolved compiled variables.
 */

typedef struct {
    Tcl_ResolvedVarInfo info;	/* "Type" information so that the compiled
				 * variable can be linked to the namespace
				 * variable at the right time. */
    Tcl_Obj *variableObj;	/* The name of the variable. */
    Tcl_Var cachedObjectVar;	/* TODO: When to flush this cache? Can class
				 * variables be cached? */
} OOResVarInfo;

/*
 * Function declarations for things defined in this file.
 */

static Tcl_Obj **	InitEnsembleRewrite(Tcl_Interp *interp, int objc,
			    Tcl_Obj *const *objv, int toRewrite,
			    int rewriteLength, Tcl_Obj *const *rewriteObjs,
			    int *lengthPtr);
static int		InvokeProcedureMethod(void *clientData,
			    Tcl_Interp *interp, Tcl_ObjectContext context,
			    int objc, Tcl_Obj *const *objv);
static Tcl_NRPostProc	FinalizeForwardCall;
static Tcl_NRPostProc	FinalizePMCall;
static int		PushMethodCallFrame(Tcl_Interp *interp,
			    CallContext *contextPtr, ProcedureMethod *pmPtr,
			    int objc, Tcl_Obj *const *objv,
			    PMFrameData *fdPtr);
static void		DeleteProcedureMethodRecord(ProcedureMethod *pmPtr);
static void		DeleteProcedureMethod(void *clientData);
static int		CloneProcedureMethod(Tcl_Interp *interp,
			    void *clientData, void **newClientData);
static void		MethodErrorHandler(Tcl_Interp *interp,
			    Tcl_Obj *procNameObj);
static void		ConstructorErrorHandler(Tcl_Interp *interp,
			    Tcl_Obj *procNameObj);
static void		DestructorErrorHandler(Tcl_Interp *interp,
			    Tcl_Obj *procNameObj);
static Tcl_Obj *	RenderMethodName(void *clientData);
static Tcl_Obj *	RenderDeclarerName(void *clientData);
static int		InvokeForwardMethod(void *clientData,
			    Tcl_Interp *interp, Tcl_ObjectContext context,
			    int objc, Tcl_Obj *const *objv);
static void		DeleteForwardMethod(void *clientData);
static int		CloneForwardMethod(Tcl_Interp *interp,
			    void *clientData, void **newClientData);
static int		ProcedureMethodVarResolver(Tcl_Interp *interp,
			    const char *varName, Tcl_Namespace *contextNs,
			    int flags, Tcl_Var *varPtr);
static int		ProcedureMethodCompiledVarResolver(Tcl_Interp *interp,
			    const char *varName, int length,
			    Tcl_Namespace *contextNs,
			    Tcl_ResolvedVarInfo **rPtrPtr);

/*
 * The types of methods defined by the core OO system.
 */

static const Tcl_MethodType procMethodType = {
    TCL_OO_METHOD_VERSION_CURRENT, "method",
    InvokeProcedureMethod, DeleteProcedureMethod, CloneProcedureMethod
};
static const Tcl_MethodType fwdMethodType = {
    TCL_OO_METHOD_VERSION_CURRENT, "forward",
    InvokeForwardMethod, DeleteForwardMethod, CloneForwardMethod
};

/*
 * Helper macros (derived from things private to tclVar.c)
 */

#define TclVarTable(contextNs) \
    ((Tcl_HashTable *) (&((Namespace *) (contextNs))->varTable))
#define TclVarHashGetValue(hPtr) \
    ((Tcl_Var) ((char *)hPtr - TclOffset(VarInHash, entry)))

static inline ProcedureMethod *
AllocProcedureMethodRecord(
    int flags)
{
    ProcedureMethod *pmPtr = (ProcedureMethod *)
	    ckalloc(sizeof(ProcedureMethod));
    memset(pmPtr, 0, sizeof(ProcedureMethod));
    pmPtr->version = TCLOO_PROCEDURE_METHOD_VERSION;
    pmPtr->flags = flags & USE_DECLARER_NS;
    pmPtr->refCount = 1;
    pmPtr->cmd.clientData = &pmPtr->efi;
    return pmPtr;
}

/*
 * ----------------------------------------------------------------------
 *
 * Tcl_NewInstanceMethod --
 *
 *	Attach a method to an object instance.
 *
 * ----------------------------------------------------------------------
 */

Tcl_Method
Tcl_NewInstanceMethod(
    Tcl_Interp *interp,		/* Unused? */
    Tcl_Object object,		/* The object that has the method attached to
				 * it. */
    Tcl_Obj *nameObj,		/* The name of the method. May be NULL; if so,
				 * up to caller to manage storage (e.g., when
				 * it is a constructor or destructor). */
    int flags,			/* Whether this is a public method. */
    const Tcl_MethodType *typePtr,
				/* The type of method this is, which defines
				 * how to invoke, delete and clone the
				 * method. */
    void *clientData)		/* Some data associated with the particular
				 * method to be created. */
{
    Object *oPtr = (Object *) object;
    Method *mPtr;
    Tcl_HashEntry *hPtr;
    int isNew;

    if (nameObj == NULL) {
	mPtr = (Method *)ckalloc(sizeof(Method));
	mPtr->namePtr = NULL;
	mPtr->refCount = 1;
	goto populate;
    }
    if (!oPtr->methodsPtr) {
	oPtr->methodsPtr = (Tcl_HashTable *)ckalloc(sizeof(Tcl_HashTable));
	Tcl_InitObjHashTable(oPtr->methodsPtr);
	oPtr->flags &= ~USE_CLASS_CACHE;
    }
    hPtr = Tcl_CreateHashEntry(oPtr->methodsPtr, (char *) nameObj, &isNew);
    if (isNew) {
	mPtr = (Method *)ckalloc(sizeof(Method));
	mPtr->namePtr = nameObj;
	mPtr->refCount = 1;
	Tcl_IncrRefCount(nameObj);
	Tcl_SetHashValue(hPtr, mPtr);
    } else {
	mPtr = (Method *)Tcl_GetHashValue(hPtr);
	if (mPtr->typePtr != NULL && mPtr->typePtr->deleteProc != NULL) {
	    mPtr->typePtr->deleteProc(mPtr->clientData);
	}
    }

  populate:
    mPtr->typePtr = typePtr;
    mPtr->clientData = clientData;
    mPtr->flags = 0;
    mPtr->declaringObjectPtr = oPtr;
    mPtr->declaringClassPtr = NULL;
    if (flags) {
	mPtr->flags |= flags & (PUBLIC_METHOD | PRIVATE_METHOD);
    }
    oPtr->epoch++;
    return (Tcl_Method) mPtr;
}

/*
 * ----------------------------------------------------------------------
 *
 * Tcl_NewMethod --
 *
 *	Attach a method to a class.
 *
 * ----------------------------------------------------------------------
 */

Tcl_Method
Tcl_NewMethod(
    Tcl_Interp *interp,		/* The interpreter containing the class. */
    Tcl_Class cls,		/* The class to attach the method to. */
    Tcl_Obj *nameObj,		/* The name of the object. May be NULL (e.g.,
				 * for constructors or destructors); if so, up
				 * to caller to manage storage. */
    int flags,			/* Whether this is a public method. */
    const Tcl_MethodType *typePtr,
				/* The type of method this is, which defines
				 * how to invoke, delete and clone the
				 * method. */
    void *clientData)		/* Some data associated with the particular
				 * method to be created. */
{
    Class *clsPtr = (Class *) cls;
    Method *mPtr;
    Tcl_HashEntry *hPtr;
    int isNew;

    if (nameObj == NULL) {
	mPtr = (Method *)ckalloc(sizeof(Method));
	mPtr->namePtr = NULL;
	mPtr->refCount = 1;
	goto populate;
    }
    hPtr = Tcl_CreateHashEntry(&clsPtr->classMethods, (char *)nameObj,&isNew);
    if (isNew) {
	mPtr = (Method *)ckalloc(sizeof(Method));
	mPtr->refCount = 1;
	mPtr->namePtr = nameObj;
	Tcl_IncrRefCount(nameObj);
	Tcl_SetHashValue(hPtr, mPtr);
    } else {
	mPtr = (Method *)Tcl_GetHashValue(hPtr);
	if (mPtr->typePtr != NULL && mPtr->typePtr->deleteProc != NULL) {
	    mPtr->typePtr->deleteProc(mPtr->clientData);
	}
    }

  populate:
    clsPtr->thisPtr->fPtr->epoch++;
    mPtr->typePtr = typePtr;
    mPtr->clientData = clientData;
    mPtr->flags = 0;
    mPtr->declaringObjectPtr = NULL;
    mPtr->declaringClassPtr = clsPtr;
    if (flags) {
	mPtr->flags |= flags & (PUBLIC_METHOD | PRIVATE_METHOD);
    }

    return (Tcl_Method) mPtr;
}

/*
 * ----------------------------------------------------------------------
 *
 * TclOODelMethodRef --
 *
 *	How to delete a method.
 *
 * ----------------------------------------------------------------------
 */

void
TclOODelMethodRef(
    Method *mPtr)
{
    if ((mPtr != NULL) && (mPtr->refCount-- <= 1)) {
	if (mPtr->typePtr != NULL && mPtr->typePtr->deleteProc != NULL) {
	    mPtr->typePtr->deleteProc(mPtr->clientData);
	}
	if (mPtr->namePtr != NULL) {
	    Tcl_DecrRefCount(mPtr->namePtr);
	}

	ckfree(mPtr);
    }
}

/*
 * ----------------------------------------------------------------------
 *
 * TclOONewBasicMethod --
 *
 *	Helper that makes it cleaner to create very simple methods during
 *	basic system initialization. Not suitable for general use.
 *
 * ----------------------------------------------------------------------
 */

void
TclOONewBasicMethod(
    Tcl_Interp *interp,
    Class *clsPtr,		/* Class to attach the method to. */
    const DeclaredClassMethod *dcm)
				/* Name of the method, whether it is public,
				 * and the function to implement it. */
{
    Tcl_Obj *namePtr = Tcl_NewStringObj(dcm->name, -1);

    Tcl_IncrRefCount(namePtr);
    Tcl_NewMethod(interp, (Tcl_Class) clsPtr, namePtr,
	    (dcm->isPublic ? PUBLIC_METHOD : 0), &dcm->definition, NULL);
    Tcl_DecrRefCount(namePtr);
}

/*
 * ----------------------------------------------------------------------
 *
 * TclOONewProcInstanceMethod --
 *
 *	Create a new procedure-like method for an object.
 *
 * ----------------------------------------------------------------------
 */

Method *
TclOONewProcInstanceMethod(
    Tcl_Interp *interp,		/* The interpreter containing the object. */
    Object *oPtr,		/* The object to modify. */
    int flags,			/* Whether this is a public method. */
    Tcl_Obj *nameObj,		/* The name of the method, which must not be
				 * NULL. */
    Tcl_Obj *argsObj,		/* The formal argument list for the method,
				 * which must not be NULL. */
    Tcl_Obj *bodyObj,		/* The body of the method, which must not be
				 * NULL. */
    ProcedureMethod **pmPtrPtr)	/* Place to write pointer to procedure method
				 * structure to allow for deeper tuning of the
				 * structure's contents. NULL if caller is not
				 * interested. */
{
    int argsLen;
    ProcedureMethod *pmPtr;
    Tcl_Method method;

    if (TclListObjLength(interp, argsObj, &argsLen) != TCL_OK) {
	return NULL;
    }
    pmPtr = AllocProcedureMethodRecord(flags);
    method = TclOOMakeProcInstanceMethod(interp, oPtr, flags, nameObj,
	    argsObj, bodyObj, &procMethodType, pmPtr, &pmPtr->procPtr);
    if (method == NULL) {
	ckfree(pmPtr);
    } else if (pmPtrPtr != NULL) {
	*pmPtrPtr = pmPtr;
    }
    return (Method *) method;
}

/*
 * ----------------------------------------------------------------------
 *
 * TclOONewProcMethod --
 *
 *	Create a new procedure-like method for a class.
 *
 * ----------------------------------------------------------------------
 */

Method *
TclOONewProcMethod(
    Tcl_Interp *interp,		/* The interpreter containing the class. */
    Class *clsPtr,		/* The class to modify. */
    int flags,			/* Whether this is a public method. */
    Tcl_Obj *nameObj,		/* The name of the method, which may be NULL;
				 * if so, up to caller to manage storage
				 * (e.g., because it is a constructor or
				 * destructor). */
    Tcl_Obj *argsObj,		/* The formal argument list for the method,
				 * which may be NULL; if so, it is equivalent
				 * to an empty list. */
    Tcl_Obj *bodyObj,		/* The body of the method, which must not be
				 * NULL. */
    ProcedureMethod **pmPtrPtr)	/* Place to write pointer to procedure method
				 * structure to allow for deeper tuning of the
				 * structure's contents. NULL if caller is not
				 * interested. */
{
    int argsLen;		/* -1 => delete argsObj before exit */
    ProcedureMethod *pmPtr;
    const char *procName;
    Tcl_Method method;

    if (argsObj == NULL) {
	argsLen = -1;
	TclNewObj(argsObj);
	Tcl_IncrRefCount(argsObj);
	procName = "<destructor>";
    } else if (TclListObjLength(interp, argsObj, &argsLen) != TCL_OK) {
	return NULL;
    } else {
	procName = (nameObj==NULL ? "<constructor>" : TclGetString(nameObj));
    }

    pmPtr = AllocProcedureMethodRecord(flags);
    method = TclOOMakeProcMethod(interp, clsPtr, flags, nameObj, procName,
	    argsObj, bodyObj, &procMethodType, pmPtr, &pmPtr->procPtr);

    if (argsLen == -1) {
	Tcl_DecrRefCount(argsObj);
    }
    if (method == NULL) {
	ckfree(pmPtr);
    } else if (pmPtrPtr != NULL) {
	*pmPtrPtr = pmPtr;
    }

    return (Method *) method;
}

/*
 * ----------------------------------------------------------------------
 *
 * TclOOMakeProcInstanceMethod --
 *
 *	The guts of the code to make a procedure-like method for an object.
 *	Split apart so that it is easier for other extensions to reuse (in
 *	particular, it frees them from having to pry so deeply into Tcl's
 *	guts).
 *
 * ----------------------------------------------------------------------
 */

Tcl_Method
TclOOMakeProcInstanceMethod(
    Tcl_Interp *interp,		/* The interpreter containing the object. */
    Object *oPtr,		/* The object to modify. */
    int flags,			/* Whether this is a public method. */
    Tcl_Obj *nameObj,		/* The name of the method, which _must not_ be
				 * NULL. */
    Tcl_Obj *argsObj,		/* The formal argument list for the method,
				 * which _must not_ be NULL. */
    Tcl_Obj *bodyObj,		/* The body of the method, which _must not_ be
				 * NULL. */
    const Tcl_MethodType *typePtr,
				/* The type of the method to create. */
    void *clientData,	/* The per-method type-specific data. */
    Proc **procPtrPtr)		/* A pointer to the variable in which to write
				 * the procedure record reference. Presumably
				 * inside the structure indicated by the
				 * pointer in clientData. */
{
    Interp *iPtr = (Interp *) interp;
    Proc *procPtr;

    if (TclCreateProc(interp, NULL, TclGetString(nameObj), argsObj, bodyObj,
	    procPtrPtr) != TCL_OK) {
	return NULL;
    }
    procPtr = *procPtrPtr;
    procPtr->cmdPtr = NULL;

    if (iPtr->cmdFramePtr) {
	CmdFrame context = *iPtr->cmdFramePtr;

	if (context.type == TCL_LOCATION_BC) {
	    /*
	     * Retrieve source information from the bytecode, if possible. If
	     * the information is retrieved successfully, context.type will be
	     * TCL_LOCATION_SOURCE and the reference held by
	     * context.data.eval.path will be counted.
	     */

	    TclGetSrcInfoForPc(&context);
	} else if (context.type == TCL_LOCATION_SOURCE) {
	    /*
	     * The copy into 'context' up above has created another reference
	     * to 'context.data.eval.path'; account for it.
	     */

	    Tcl_IncrRefCount(context.data.eval.path);
	}

	if (context.type == TCL_LOCATION_SOURCE) {
	    /*
	     * We can account for source location within a proc only if the
	     * proc body was not created by substitution.
	     * (FIXME: check that this is sane and correct!)
	     */

	    if (context.line
		    && (context.nline >= 4) && (context.line[3] >= 0)) {
		int isNew;
		CmdFrame *cfPtr = (CmdFrame *)ckalloc(sizeof(CmdFrame));
		Tcl_HashEntry *hPtr;

		cfPtr->level = -1;
		cfPtr->type = context.type;
		cfPtr->line = (int *)ckalloc(sizeof(int));
		cfPtr->line[0] = context.line[3];
		cfPtr->nline = 1;
		cfPtr->framePtr = NULL;
		cfPtr->nextPtr = NULL;

		cfPtr->data.eval.path = context.data.eval.path;
		Tcl_IncrRefCount(cfPtr->data.eval.path);

		cfPtr->cmd = NULL;
		cfPtr->len = 0;

		hPtr = Tcl_CreateHashEntry(iPtr->linePBodyPtr,
			(char *) procPtr, &isNew);
		Tcl_SetHashValue(hPtr, cfPtr);
	    }

	    /*
	     * 'context' is going out of scope; account for the reference that
	     * it's holding to the path name.
	     */

	    Tcl_DecrRefCount(context.data.eval.path);
	    context.data.eval.path = NULL;
	}
    }

    return Tcl_NewInstanceMethod(interp, (Tcl_Object) oPtr, nameObj, flags,
	    typePtr, clientData);
}

/*
 * ----------------------------------------------------------------------
 *
 * TclOOMakeProcMethod --
 *
 *	The guts of the code to make a procedure-like method for a class.
 *	Split apart so that it is easier for other extensions to reuse (in
 *	particular, it frees them from having to pry so deeply into Tcl's
 *	guts).
 *
 * ----------------------------------------------------------------------
 */

Tcl_Method
TclOOMakeProcMethod(
    Tcl_Interp *interp,		/* The interpreter containing the class. */
    Class *clsPtr,		/* The class to modify. */
    int flags,			/* Whether this is a public method. */
    Tcl_Obj *nameObj,		/* The name of the method, which may be NULL;
				 * if so, up to caller to manage storage
				 * (e.g., because it is a constructor or
				 * destructor). */
    const char *namePtr,	/* The name of the method as a string, which
				 * _must not_ be NULL. */
    Tcl_Obj *argsObj,		/* The formal argument list for the method,
				 * which _must not_ be NULL. */
    Tcl_Obj *bodyObj,		/* The body of the method, which _must not_ be
				 * NULL. */
    const Tcl_MethodType *typePtr,
				/* The type of the method to create. */
    void *clientData,	/* The per-method type-specific data. */
    Proc **procPtrPtr)		/* A pointer to the variable in which to write
				 * the procedure record reference. Presumably
				 * inside the structure indicated by the
				 * pointer in clientData. */
{
    Interp *iPtr = (Interp *) interp;
    Proc *procPtr;

    if (TclCreateProc(interp, NULL, namePtr, argsObj, bodyObj,
	    procPtrPtr) != TCL_OK) {
	return NULL;
    }
    procPtr = *procPtrPtr;
    procPtr->cmdPtr = NULL;

    if (iPtr->cmdFramePtr) {
	CmdFrame context = *iPtr->cmdFramePtr;

	if (context.type == TCL_LOCATION_BC) {
	    /*
	     * Retrieve source information from the bytecode, if possible. If
	     * the information is retrieved successfully, context.type will be
	     * TCL_LOCATION_SOURCE and the reference held by
	     * context.data.eval.path will be counted.
	     */

	    TclGetSrcInfoForPc(&context);
	} else if (context.type == TCL_LOCATION_SOURCE) {
	    /*
	     * The copy into 'context' up above has created another reference
	     * to 'context.data.eval.path'; account for it.
	     */

	    Tcl_IncrRefCount(context.data.eval.path);
	}

	if (context.type == TCL_LOCATION_SOURCE) {
	    /*
	     * We can account for source location within a proc only if the
	     * proc body was not created by substitution.
	     * (FIXME: check that this is sane and correct!)
	     */

	    if (context.line
		    && (context.nline >= 4) && (context.line[3] >= 0)) {
		int isNew;
		CmdFrame *cfPtr = (CmdFrame *)ckalloc(sizeof(CmdFrame));
		Tcl_HashEntry *hPtr;

		cfPtr->level = -1;
		cfPtr->type = context.type;
		cfPtr->line = (int *)ckalloc(sizeof(int));
		cfPtr->line[0] = context.line[3];
		cfPtr->nline = 1;
		cfPtr->framePtr = NULL;
		cfPtr->nextPtr = NULL;

		cfPtr->data.eval.path = context.data.eval.path;
		Tcl_IncrRefCount(cfPtr->data.eval.path);

		cfPtr->cmd = NULL;
		cfPtr->len = 0;

		hPtr = Tcl_CreateHashEntry(iPtr->linePBodyPtr,
			(char *) procPtr, &isNew);
		Tcl_SetHashValue(hPtr, cfPtr);
	    }

	    /*
	     * 'context' is going out of scope; account for the reference that
	     * it's holding to the path name.
	     */

	    Tcl_DecrRefCount(context.data.eval.path);
	    context.data.eval.path = NULL;
	}
    }

    return Tcl_NewMethod(interp, (Tcl_Class) clsPtr, nameObj, flags, typePtr,
	    clientData);
}

/*
 * ----------------------------------------------------------------------
 *
 * InvokeProcedureMethod, PushMethodCallFrame --
 *
 *	How to invoke a procedure-like method.
 *
 * ----------------------------------------------------------------------
 */

static int
InvokeProcedureMethod(
    void *clientData,		/* Pointer to some per-method context. */
    Tcl_Interp *interp,
    Tcl_ObjectContext context,	/* The method calling context. */
    int objc,			/* Number of arguments. */
    Tcl_Obj *const *objv)	/* Arguments as actually seen. */
{
    ProcedureMethod *pmPtr = (ProcedureMethod *)clientData;
    int result;
    PMFrameData *fdPtr;		/* Important data that has to have a lifetime
				 * matched by this function (or rather, by the
				 * call frame's lifetime). */

    /*
     * If the object namespace (or interpreter) were deleted, we just skip to
     * the next thing in the chain.
     */

    if (TclOOObjectDestroyed(((CallContext *)context)->oPtr) ||
	Tcl_InterpDeleted(interp)
    ) {
	return TclNRObjectContextInvokeNext(interp, context, objc, objv,
		Tcl_ObjectContextSkippedArgs(context));
    }

    /*
     * Finishes filling out the extra frame info so that [info frame] works if
     * that is not already set up.
     */

    if (pmPtr->efi.length == 0) {
	Tcl_Method method = Tcl_ObjectContextMethod(context);

	pmPtr->efi.length = 2;
	pmPtr->efi.fields[0].name = "method";
	pmPtr->efi.fields[0].proc = RenderMethodName;
	pmPtr->efi.fields[0].clientData = pmPtr;
	pmPtr->callSiteFlags = ((CallContext *)
		context)->callPtr->flags & (CONSTRUCTOR | DESTRUCTOR);
	pmPtr->interp = interp;
	pmPtr->method = method;
	if (pmPtr->gfivProc != NULL) {
	    pmPtr->efi.fields[1].name = "";
	    pmPtr->efi.fields[1].proc = pmPtr->gfivProc;
	    pmPtr->efi.fields[1].clientData = pmPtr;
	} else {
	    if (Tcl_MethodDeclarerObject(method) != NULL) {
		pmPtr->efi.fields[1].name = "object";
	    } else {
		pmPtr->efi.fields[1].name = "class";
	    }
	    pmPtr->efi.fields[1].proc = RenderDeclarerName;
	    pmPtr->efi.fields[1].clientData = pmPtr;
	}
    }

    /*
     * Allocate the special frame data.
     */

    fdPtr = (PMFrameData *)TclStackAlloc(interp, sizeof(PMFrameData));

    /*
     * Create a call frame for this method.
     */

    result = PushMethodCallFrame(interp, (CallContext *) context, pmPtr,
	    objc, objv, fdPtr);
    if (result != TCL_OK) {
	TclStackFree(interp, fdPtr);
	return result;
    }
    pmPtr->refCount++;

    /*
     * Give the pre-call callback a chance to do some setup and, possibly,
     * veto the call.
     */

    if (pmPtr->preCallProc != NULL) {
	int isFinished;

	result = pmPtr->preCallProc(pmPtr->clientData, interp, context,
		(Tcl_CallFrame *) fdPtr->framePtr, &isFinished);
	if (isFinished || result != TCL_OK) {
	    Tcl_PopCallFrame(interp);
	    TclStackFree(interp, fdPtr->framePtr);
	    if (pmPtr->refCount-- <= 1) {
		DeleteProcedureMethodRecord(pmPtr);
	    }
	    TclStackFree(interp, fdPtr);
	    return result;
	}
    }

    /*
     * Now invoke the body of the method.
     */

    TclNRAddCallback(interp, FinalizePMCall, pmPtr, context, fdPtr, NULL);
    return TclNRInterpProcCore(interp, fdPtr->nameObj,
	    Tcl_ObjectContextSkippedArgs(context), fdPtr->errProc);
}

static int
FinalizePMCall(
    void *data[],
    Tcl_Interp *interp,
    int result)
{
    ProcedureMethod *pmPtr = (ProcedureMethod *)data[0];
    Tcl_ObjectContext context = (Tcl_ObjectContext)data[1];
    PMFrameData *fdPtr = (PMFrameData *)data[2];

    /*
     * Give the post-call callback a chance to do some cleanup. Note that at
     * this point the call frame itself is invalid; it's already been popped.
     */

    if (pmPtr->postCallProc) {
	result = pmPtr->postCallProc(pmPtr->clientData, interp, context,
		Tcl_GetObjectNamespace(Tcl_ObjectContextObject(context)),
		result);
    }

    /*
     * Scrap the special frame data now that we're done with it. Note that we
     * are inlining DeleteProcedureMethod() here; this location is highly
     * sensitive when it comes to performance!
     */

    if (pmPtr->refCount-- <= 1) {
	DeleteProcedureMethodRecord(pmPtr);
    }
    TclStackFree(interp, fdPtr);
    return result;
}

static int
PushMethodCallFrame(
    Tcl_Interp *interp,		/* Current interpreter. */
    CallContext *contextPtr,	/* Current method call context. */
    ProcedureMethod *pmPtr,	/* Information about this procedure-like
				 * method. */
    int objc,			/* Number of arguments. */
    Tcl_Obj *const *objv,	/* Array of arguments. */
    PMFrameData *fdPtr)		/* Place to store information about the call
				 * frame. */
{
    Namespace *nsPtr = (Namespace *) contextPtr->oPtr->namespacePtr;
    int result;
    CallFrame **framePtrPtr = &fdPtr->framePtr;

    /*
     * Compute basic information on the basis of the type of method it is.
     */

    if (contextPtr->callPtr->flags & CONSTRUCTOR) {
	fdPtr->nameObj = contextPtr->oPtr->fPtr->constructorName;
	fdPtr->errProc = ConstructorErrorHandler;
    } else if (contextPtr->callPtr->flags & DESTRUCTOR) {
	fdPtr->nameObj = contextPtr->oPtr->fPtr->destructorName;
	fdPtr->errProc = DestructorErrorHandler;
    } else {
	fdPtr->nameObj = Tcl_MethodName(
		Tcl_ObjectContextMethod((Tcl_ObjectContext) contextPtr));
	fdPtr->errProc = MethodErrorHandler;
    }
    if (pmPtr->errProc != NULL) {
	fdPtr->errProc = pmPtr->errProc;
    }

    /*
     * Magic to enable things like [incr Tcl], which wants methods to run in
     * their class's namespace.
     */

    if (pmPtr->flags & USE_DECLARER_NS) {
	Method *mPtr = contextPtr->callPtr->chain[contextPtr->index].mPtr;

	if (mPtr->declaringClassPtr != NULL) {
	    nsPtr = (Namespace *)
		    mPtr->declaringClassPtr->thisPtr->namespacePtr;
	} else {
	    nsPtr = (Namespace *) mPtr->declaringObjectPtr->namespacePtr;
	}
    }

    /*
     * Compile the body.
     *
     * [Bug 2037727] Always call TclProcCompileProc so that we check not only
     * that we have bytecode, but also that it remains valid. Note that we set
     * the namespace of the code here directly; this is a hack, but the
     * alternative is *so* slow...
     */

    pmPtr->procPtr->cmdPtr = &pmPtr->cmd;
    if (pmPtr->procPtr->bodyPtr->typePtr == &tclByteCodeType) {
	ByteCode *codePtr = (ByteCode *)
		pmPtr->procPtr->bodyPtr->internalRep.twoPtrValue.ptr1;

	codePtr->nsPtr = nsPtr;
    }
    result = TclProcCompileProc(interp, pmPtr->procPtr,
	    pmPtr->procPtr->bodyPtr, nsPtr, "body of method",
	    TclGetString(fdPtr->nameObj));
    if (result != TCL_OK) {
	return result;
    }

    /*
     * Make the stack frame and fill it out with information about this call.
     * This operation doesn't ever actually fail.
     */

    (void) TclPushStackFrame(interp, (Tcl_CallFrame **) framePtrPtr,
	    (Tcl_Namespace *) nsPtr, FRAME_IS_PROC|FRAME_IS_METHOD);

    fdPtr->framePtr->clientData = contextPtr;
    fdPtr->framePtr->objc = objc;
    fdPtr->framePtr->objv = objv;
    fdPtr->framePtr->procPtr = pmPtr->procPtr;

    return TCL_OK;
}

/*
 * ----------------------------------------------------------------------
 *
 * TclOOSetupVariableResolver, etc. --
 *
 *	Variable resolution engine used to connect declared variables to local
 *	variables used in methods. The compiled variable resolver is more
 *	important, but both are needed as it is possible to have a variable
 *	that is only referred to in ways that aren't compilable and we can't
 *	force LVT presence. [TIP #320]
 *
 * ----------------------------------------------------------------------
 */

void
TclOOSetupVariableResolver(
    Tcl_Namespace *nsPtr)
{
    Tcl_ResolverInfo info;

    Tcl_GetNamespaceResolvers(nsPtr, &info);
    if (info.compiledVarResProc == NULL) {
	Tcl_SetNamespaceResolvers(nsPtr, NULL, ProcedureMethodVarResolver,
		ProcedureMethodCompiledVarResolver);
    }
}

static int
ProcedureMethodVarResolver(
    Tcl_Interp *interp,
    const char *varName,
    Tcl_Namespace *contextNs,
    int flags,
    Tcl_Var *varPtr)
{
    int result;
    Tcl_ResolvedVarInfo *rPtr = NULL;

    result = ProcedureMethodCompiledVarResolver(interp, varName,
	    strlen(varName), contextNs, &rPtr);

    if (result != TCL_OK) {
	return result;
    }

    *varPtr = rPtr->fetchProc(interp, rPtr);

    /*
     * Must not retain reference to resolved information. [Bug 3105999]
     */

    rPtr->deleteProc(rPtr);
    return (*varPtr ? TCL_OK : TCL_CONTINUE);
}

static Tcl_Var
ProcedureMethodCompiledVarConnect(
    Tcl_Interp *interp,
    Tcl_ResolvedVarInfo *rPtr)
{
    OOResVarInfo *infoPtr = (OOResVarInfo *) rPtr;
    Interp *iPtr = (Interp *) interp;
    CallFrame *framePtr = iPtr->varFramePtr;
    CallContext *contextPtr;
    Tcl_Obj *variableObj;
    Tcl_HashEntry *hPtr;
    int i, isNew, cacheIt, varLen, len;
    const char *match, *varName;

    /*
     * Check that the variable is being requested in a context that is also a
     * method call; if not (i.e. we're evaluating in the object's namespace or
     * in a procedure of that namespace) then we do nothing.
     */

    if (framePtr == NULL || !(framePtr->isProcCallFrame & FRAME_IS_METHOD)) {
	return NULL;
    }
    contextPtr = (CallContext *)framePtr->clientData;

    /*
     * If we've done the work before (in a comparable context) then reuse that
     * rather than performing resolution ourselves.
     */

    if (infoPtr->cachedObjectVar) {
	return infoPtr->cachedObjectVar;
    }

    /*
     * Check if the variable is one we want to resolve at all (i.e. whether it
     * is in the list provided by the user). If not, we mustn't do anything
     * either.
     */

    varName = TclGetStringFromObj(infoPtr->variableObj, &varLen);
    if (contextPtr->callPtr->chain[contextPtr->index]
	    .mPtr->declaringClassPtr != NULL) {
	FOREACH(variableObj, contextPtr->callPtr->chain[contextPtr->index]
		.mPtr->declaringClassPtr->variables) {
	    match = TclGetStringFromObj(variableObj, &len);
	    if ((len == varLen) && !memcmp(match, varName, len)) {
		cacheIt = 0;
		goto gotMatch;
	    }
	}
    } else {
	FOREACH(variableObj, contextPtr->oPtr->variables) {
	    match = TclGetStringFromObj(variableObj, &len);
	    if ((len == varLen) && !memcmp(match, varName, len)) {
		cacheIt = 1;
		goto gotMatch;
	    }
	}
    }
    return NULL;

    /*
     * It is a variable we want to resolve, so resolve it.
     */

  gotMatch:
    hPtr = Tcl_CreateHashEntry(TclVarTable(contextPtr->oPtr->namespacePtr),
	    (char *) variableObj, &isNew);
    if (isNew) {
	TclSetVarNamespaceVar((Var *) TclVarHashGetValue(hPtr));
    }
    if (cacheIt) {
	infoPtr->cachedObjectVar = TclVarHashGetValue(hPtr);

	/*
	 * We must keep a reference to the variable so everything will
	 * continue to work correctly even if it is unset; being unset does
	 * not end the life of the variable at this level. [Bug 3185009]
	 */

	VarHashRefCount(infoPtr->cachedObjectVar)++;
    }
    return TclVarHashGetValue(hPtr);
}

static void
ProcedureMethodCompiledVarDelete(
    Tcl_ResolvedVarInfo *rPtr)
{
    OOResVarInfo *infoPtr = (OOResVarInfo *) rPtr;

    /*
     * Release the reference to the variable if we were holding it.
     */

    if (infoPtr->cachedObjectVar) {
	VarHashRefCount(infoPtr->cachedObjectVar)--;
	TclCleanupVar((Var *) infoPtr->cachedObjectVar, NULL);
    }
    Tcl_DecrRefCount(infoPtr->variableObj);
    ckfree(infoPtr);
}

static int
ProcedureMethodCompiledVarResolver(
    Tcl_Interp *interp,
    const char *varName,
    int length,
    Tcl_Namespace *contextNs,
    Tcl_ResolvedVarInfo **rPtrPtr)
{
    OOResVarInfo *infoPtr;
    Tcl_Obj *variableObj = Tcl_NewStringObj(varName, length);

    /*
     * Do not create resolvers for cases that contain namespace separators or
     * which look like array accesses. Both will lead us astray.
     */

    if (strstr(Tcl_GetString(variableObj), "::") != NULL ||
	    Tcl_StringMatch(Tcl_GetString(variableObj), "*(*)")) {
	Tcl_DecrRefCount(variableObj);
	return TCL_CONTINUE;
    }

    infoPtr = (OOResVarInfo *)ckalloc(sizeof(OOResVarInfo));
    infoPtr->info.fetchProc = ProcedureMethodCompiledVarConnect;
    infoPtr->info.deleteProc = ProcedureMethodCompiledVarDelete;
    infoPtr->cachedObjectVar = NULL;
    infoPtr->variableObj = variableObj;
    Tcl_IncrRefCount(variableObj);
    *rPtrPtr = &infoPtr->info;
    return TCL_OK;
}

/*
 * ----------------------------------------------------------------------
 *
 * RenderMethodName --
 *
 *	Returns the name of the declared method. Used for producing information
 *	for [info frame].
 *
 * ----------------------------------------------------------------------
 */

static Tcl_Obj *
RenderMethodName(
    void *clientData)
{
    ProcedureMethod *pmPtr = (ProcedureMethod *) clientData;

    if (pmPtr->callSiteFlags & CONSTRUCTOR) {
	return TclOOGetFoundation(pmPtr->interp)->constructorName;
    } else if (pmPtr->callSiteFlags & DESTRUCTOR) {
	return TclOOGetFoundation(pmPtr->interp)->destructorName;
    } else {
	return Tcl_MethodName(pmPtr->method);
    }
}

/*
 * ----------------------------------------------------------------------
 *
 * RenderDeclarerName --
 *
 *	Returns the name of the entity (object or class) which declared a
 *	method. Used for producing information for [info frame] in such a way
 *	that the expensive part of this (generating the object or class name
 *	itself) isn't done until it is needed.
 *
 * ----------------------------------------------------------------------
 */

static Tcl_Obj *
RenderDeclarerName(
    void *clientData)
{
    ProcedureMethod *pmPtr = (ProcedureMethod *) clientData;
    Tcl_Object object = Tcl_MethodDeclarerObject(pmPtr->method);

    if (object == NULL) {
	object = Tcl_GetClassAsObject(Tcl_MethodDeclarerClass(pmPtr->method));
    }
    return TclOOObjectName(pmPtr->interp, (Object *) object);
}

/*
 * ----------------------------------------------------------------------
 *
 * MethodErrorHandler, ConstructorErrorHandler, DestructorErrorHandler --
 *
 *	How to fill in the stack trace correctly upon error in various forms
 *	of procedure-like methods. LIMIT is how long the inserted strings in
 *	the error traces should get before being converted to have ellipses,
 *	and ELLIPSIFY is a macro to do the conversion (with the help of a
 *	%.*s%s format field). Note that ELLIPSIFY is only safe for use in
 *	suitable formatting contexts.
 *
 * ----------------------------------------------------------------------
 */

#define LIMIT 60
#define ELLIPSIFY(str,len) \
	((len) > LIMIT ? LIMIT : (len)), (str), ((len) > LIMIT ? "..." : "")

static void
CommonMethErrorHandler(
    Tcl_Interp *interp,
    const char *special)
{
    int objectNameLen;
    CallContext *contextPtr = (CallContext *)((Interp *) interp)->varFramePtr->clientData;
    Method *mPtr = contextPtr->callPtr->chain[contextPtr->index].mPtr;
    const char *objectName, *kindName = "instance";
    Object *declarerPtr = NULL;

    if (mPtr->declaringObjectPtr != NULL) {
	declarerPtr = mPtr->declaringObjectPtr;
	kindName = "object";
    } else if (mPtr->declaringClassPtr != NULL) {
	declarerPtr = mPtr->declaringClassPtr->thisPtr;
	kindName = "class";
    }

    if (declarerPtr) {
	objectName = Tcl_GetStringFromObj(TclOOObjectName(interp, declarerPtr),
	    &objectNameLen);
    } else {
	objectName = "unknown or deleted";
	objectNameLen = 18;
    }
    if (!special) {
	int nameLen;
	const char *methodName = Tcl_GetStringFromObj(mPtr->namePtr, &nameLen);
	Tcl_AppendObjToErrorInfo(interp, Tcl_ObjPrintf(
	    "\n    (%s \"%.*s%s\" method \"%.*s%s\" line %d)",
	    kindName, ELLIPSIFY(objectName, objectNameLen),
	    ELLIPSIFY(methodName, nameLen), Tcl_GetErrorLine(interp)));
    } else {
	Tcl_AppendObjToErrorInfo(interp, Tcl_ObjPrintf(
	    "\n    (%s \"%.*s%s\" %s line %d)", kindName,
	    ELLIPSIFY(objectName, objectNameLen), special, Tcl_GetErrorLine(interp)));
    }
}

static void
MethodErrorHandler(
    Tcl_Interp *interp,
    Tcl_Obj *methodNameObj)
{
    (void)methodNameObj;/* We pull the method name out of context instead of from argument */
    CommonMethErrorHandler(interp, NULL);
}

static void
ConstructorErrorHandler(
    Tcl_Interp *interp,
    Tcl_Obj *methodNameObj)
{
    (void)methodNameObj;/* Ignore. We know it is the constructor. */
    CommonMethErrorHandler(interp, "constructor");
}

static void
DestructorErrorHandler(
    Tcl_Interp *interp,
    Tcl_Obj *methodNameObj)
{
    (void)methodNameObj; /* Ignore. We know it is the destructor. */
    CommonMethErrorHandler(interp, "destructor");
}

/*
 * ----------------------------------------------------------------------
 *
 * DeleteProcedureMethod, CloneProcedureMethod --
 *
 *	How to delete and clone procedure-like methods.
 *
 * ----------------------------------------------------------------------
 */

static void
DeleteProcedureMethodRecord(
    ProcedureMethod *pmPtr)
{
    TclProcDeleteProc(pmPtr->procPtr);
    if (pmPtr->deleteClientdataProc) {
	pmPtr->deleteClientdataProc(pmPtr->clientData);
    }
    ckfree(pmPtr);
}

static void
DeleteProcedureMethod(
    void *clientData)
{
    ProcedureMethod *pmPtr = (ProcedureMethod *)clientData;

    if (pmPtr->refCount-- <= 1) {
	DeleteProcedureMethodRecord(pmPtr);
    }
}

static int
CloneProcedureMethod(
    Tcl_Interp *interp,
    void *clientData,
    void **newClientData)
{
    ProcedureMethod *pmPtr = (ProcedureMethod *)clientData;
    ProcedureMethod *pm2Ptr;
    Tcl_Obj *bodyObj, *argsObj;
    CompiledLocal *localPtr;

    /*
     * Copy the argument list.
     */

    TclNewObj(argsObj);
    for (localPtr=pmPtr->procPtr->firstLocalPtr; localPtr!=NULL;
	    localPtr=localPtr->nextPtr) {
	if (TclIsVarArgument(localPtr)) {
	    Tcl_Obj *argObj;

	    TclNewObj(argObj);
	    Tcl_ListObjAppendElement(NULL, argObj,
		    Tcl_NewStringObj(localPtr->name, -1));
	    if (localPtr->defValuePtr != NULL) {
		Tcl_ListObjAppendElement(NULL, argObj, localPtr->defValuePtr);
	    }
	    Tcl_ListObjAppendElement(NULL, argsObj, argObj);
	}
    }

    /*
     * Must strip the internal representation in order to ensure that any
     * bound references to instance variables are removed. [Bug 3609693]
     */

    bodyObj = Tcl_DuplicateObj(pmPtr->procPtr->bodyPtr);
    Tcl_GetString(bodyObj);
    TclFreeIntRep(bodyObj);

    /*
     * Create the actual copy of the method record, manufacturing a new proc
     * record.
     */

    pm2Ptr = (ProcedureMethod *)ckalloc(sizeof(ProcedureMethod));
    memcpy(pm2Ptr, pmPtr, sizeof(ProcedureMethod));
    pm2Ptr->refCount = 1;
    pm2Ptr->cmd.clientData = &pm2Ptr->efi;
    pm2Ptr->efi.length = 0;	/* Trigger a reinit of this. */
    Tcl_IncrRefCount(argsObj);
    Tcl_IncrRefCount(bodyObj);
    if (TclCreateProc(interp, NULL, "", argsObj, bodyObj,
	    &pm2Ptr->procPtr) != TCL_OK) {
	Tcl_DecrRefCount(argsObj);
	Tcl_DecrRefCount(bodyObj);
	ckfree(pm2Ptr);
	return TCL_ERROR;
    }
    Tcl_DecrRefCount(argsObj);
    Tcl_DecrRefCount(bodyObj);

    if (pmPtr->cloneClientdataProc) {
	pm2Ptr->clientData = pmPtr->cloneClientdataProc(pmPtr->clientData);
    }
    *newClientData = pm2Ptr;
    return TCL_OK;
}

/*
 * ----------------------------------------------------------------------
 *
 * TclOONewForwardInstanceMethod --
 *
 *	Create a forwarded method for an object.
 *
 * ----------------------------------------------------------------------
 */

Method *
TclOONewForwardInstanceMethod(
    Tcl_Interp *interp,		/* Interpreter for error reporting. */
    Object *oPtr,		/* The object to attach the method to. */
    int flags,			/* Whether the method is public or not. */
    Tcl_Obj *nameObj,		/* The name of the method. */
    Tcl_Obj *prefixObj)		/* List of arguments that form the command
				 * prefix to forward to. */
{
    int prefixLen;
    ForwardMethod *fmPtr;

    if (TclListObjLength(interp, prefixObj, &prefixLen) != TCL_OK) {
	return NULL;
    }
    if (prefixLen < 1) {
	Tcl_SetObjResult(interp, Tcl_NewStringObj(
		"method forward prefix must be non-empty", -1));
	Tcl_SetErrorCode(interp, "TCL", "OO", "BAD_FORWARD", (char *)NULL);
	return NULL;
    }

    fmPtr = (ForwardMethod *)ckalloc(sizeof(ForwardMethod));
    fmPtr->prefixObj = prefixObj;
    Tcl_IncrRefCount(prefixObj);
    return (Method *) Tcl_NewInstanceMethod(interp, (Tcl_Object) oPtr,
	    nameObj, flags, &fwdMethodType, fmPtr);
}

/*
 * ----------------------------------------------------------------------
 *
 * TclOONewForwardMethod --
 *
 *	Create a new forwarded method for a class.
 *
 * ----------------------------------------------------------------------
 */

Method *
TclOONewForwardMethod(
    Tcl_Interp *interp,		/* Interpreter for error reporting. */
    Class *clsPtr,		/* The class to attach the method to. */
    int flags,			/* Whether the method is public or not. */
    Tcl_Obj *nameObj,		/* The name of the method. */
    Tcl_Obj *prefixObj)		/* List of arguments that form the command
				 * prefix to forward to. */
{
    int prefixLen;
    ForwardMethod *fmPtr;

    if (TclListObjLength(interp, prefixObj, &prefixLen) != TCL_OK) {
	return NULL;
    }
    if (prefixLen < 1) {
	Tcl_SetObjResult(interp, Tcl_NewStringObj(
		"method forward prefix must be non-empty", -1));
	Tcl_SetErrorCode(interp, "TCL", "OO", "BAD_FORWARD", (char *)NULL);
	return NULL;
    }

    fmPtr = (ForwardMethod *)ckalloc(sizeof(ForwardMethod));
    fmPtr->prefixObj = prefixObj;
    Tcl_IncrRefCount(prefixObj);
    return (Method *) Tcl_NewMethod(interp, (Tcl_Class) clsPtr, nameObj,
	    flags, &fwdMethodType, fmPtr);
}

/*
 * ----------------------------------------------------------------------
 *
 * InvokeForwardMethod --
 *
 *	How to invoke a forwarded method. Works by doing some ensemble-like
 *	command rearranging and then invokes some other Tcl command.
 *
 * ----------------------------------------------------------------------
 */

static int
InvokeForwardMethod(
    void *clientData,	/* Pointer to some per-method context. */
    Tcl_Interp *interp,
    Tcl_ObjectContext context,	/* The method calling context. */
    int objc,			/* Number of arguments. */
    Tcl_Obj *const *objv)	/* Arguments as actually seen. */
{
    CallContext *contextPtr = (CallContext *) context;
    ForwardMethod *fmPtr = (ForwardMethod *)clientData;
    Tcl_Obj **argObjs, **prefixObjs;
    int numPrefixes, len, skip = contextPtr->skip;

    /*
     * Build the real list of arguments to use. Note that we know that the
     * prefixObj field of the ForwardMethod structure holds a reference to a
     * non-empty list, so there's a whole class of failures ("not a list") we
     * can ignore here.
     */

    TclListObjGetElements(NULL, fmPtr->prefixObj, &numPrefixes, &prefixObjs);
    argObjs = InitEnsembleRewrite(interp, objc, objv, skip,
	    numPrefixes, prefixObjs, &len);
    Tcl_NRAddCallback(interp, FinalizeForwardCall, argObjs, NULL, NULL, NULL);
    /*
     * NOTE: The combination of direct set of iPtr->lookupNsPtr and the use
     * of the TCL_EVAL_NOERR flag results in an evaluation configuration
     * very much like TCL_EVAL_INVOKE.
     */
    ((Interp *)interp)->lookupNsPtr
	    = (Namespace *) contextPtr->oPtr->namespacePtr;
    return TclNREvalObjv(interp, len, argObjs, TCL_EVAL_NOERR, NULL);
}

static int
FinalizeForwardCall(
    void *data[],
    Tcl_Interp *interp,
    int result)
{
    Tcl_Obj **argObjs = (Tcl_Obj **)data[0];

    TclStackFree(interp, argObjs);
    return result;
}

/*
 * ----------------------------------------------------------------------
 *
 * DeleteForwardMethod, CloneForwardMethod --
 *
 *	How to delete and clone forwarded methods.
 *
 * ----------------------------------------------------------------------
 */

static void
DeleteForwardMethod(
    void *clientData)
{
    ForwardMethod *fmPtr = (ForwardMethod *)clientData;

    Tcl_DecrRefCount(fmPtr->prefixObj);
    ckfree(fmPtr);
}

static int
CloneForwardMethod(
    Tcl_Interp *interp,
    void *clientData,
    void **newClientData)
{
    ForwardMethod *fmPtr = (ForwardMethod *)clientData;
    ForwardMethod *fm2Ptr = (ForwardMethod *)ckalloc(sizeof(ForwardMethod));

    fm2Ptr->prefixObj = fmPtr->prefixObj;
    Tcl_IncrRefCount(fm2Ptr->prefixObj);
    *newClientData = fm2Ptr;
    return TCL_OK;
}

/*
 * ----------------------------------------------------------------------
 *
 * TclOOGetProcFromMethod, TclOOGetFwdFromMethod --
 *
 *	Utility functions used for procedure-like and forwarding method
 *	introspection.
 *
 * ----------------------------------------------------------------------
 */

Proc *
TclOOGetProcFromMethod(
    Method *mPtr)
{
    if (mPtr->typePtr == &procMethodType) {
	ProcedureMethod *pmPtr = (ProcedureMethod *)mPtr->clientData;

	return pmPtr->procPtr;
    }
    return NULL;
}

Tcl_Obj *
TclOOGetMethodBody(
    Method *mPtr)
{
    if (mPtr->typePtr == &procMethodType) {
	ProcedureMethod *pmPtr = (ProcedureMethod *)mPtr->clientData;

	if (pmPtr->procPtr->bodyPtr->bytes == NULL) {
	    (void) Tcl_GetString(pmPtr->procPtr->bodyPtr);
	}
	return pmPtr->procPtr->bodyPtr;
    }
    return NULL;
}

Tcl_Obj *
TclOOGetFwdFromMethod(
    Method *mPtr)
{
    if (mPtr->typePtr == &fwdMethodType) {
	ForwardMethod *fwPtr = (ForwardMethod *)mPtr->clientData;

	return fwPtr->prefixObj;
    }
    return NULL;
}

/*
 * ----------------------------------------------------------------------
 *
 * InitEnsembleRewrite --
 *
 *	Utility function that wraps up a lot of the complexity involved in
 *	doing ensemble-like command forwarding. Here is a picture of memory
 *	management plan:
 *
 *                    <-----------------objc---------------------->
 *      objv:        |=============|===============================|
 *                    <-toRewrite->           |
 *                                             \
 *                    <-rewriteLength->         \
 *      rewriteObjs: |=================|         \
 *                           |                    |
 *                           V                    V
 *      argObjs:     |=================|===============================|
 *                    <------------------*lengthPtr------------------->
 *
 * ----------------------------------------------------------------------
 */

static Tcl_Obj **
InitEnsembleRewrite(
    Tcl_Interp *interp,		/* Place to log the rewrite info. */
    int objc,			/* Number of real arguments. */
    Tcl_Obj *const *objv,	/* The real arguments. */
    int toRewrite,		/* Number of real arguments to replace. */
    int rewriteLength,		/* Number of arguments to insert instead. */
    Tcl_Obj *const *rewriteObjs,/* Arguments to insert instead. */
    int *lengthPtr)		/* Where to write the resulting length of the
				 * array of rewritten arguments. */
{
    unsigned len = rewriteLength + objc - toRewrite;
    Tcl_Obj **argObjs = (Tcl_Obj **)TclStackAlloc(interp, sizeof(Tcl_Obj *) * len);

    memcpy(argObjs, rewriteObjs, rewriteLength * sizeof(Tcl_Obj *));
    memcpy(argObjs + rewriteLength, objv + toRewrite,
	    sizeof(Tcl_Obj *) * (objc - toRewrite));

    /*
     * Now plumb this into the core ensemble rewrite logging system so that
     * Tcl_WrongNumArgs() can rewrite its result appropriately. The rules for
     * how to store the rewrite rules get complex solely because of the case
     * where an ensemble rewrites itself out of the picture; when that
     * happens, the quality of the error message rewrite falls drastically
     * (and unavoidably).
     */

    if (TclInitRewriteEnsemble(interp, toRewrite, rewriteLength, objv)) {
	TclNRAddCallback(interp, TclClearRootEnsemble, NULL, NULL, NULL, NULL);
    }
    *lengthPtr = len;
    return argObjs;
}

/*
 * ----------------------------------------------------------------------
 *
 * assorted trivial 'getter' functions
 *
 * ----------------------------------------------------------------------
 */

Tcl_Object
Tcl_MethodDeclarerObject(
    Tcl_Method method)
{
    return (Tcl_Object) ((Method *) method)->declaringObjectPtr;
}

Tcl_Class
Tcl_MethodDeclarerClass(
    Tcl_Method method)
{
    return (Tcl_Class) ((Method *) method)->declaringClassPtr;
}

Tcl_Obj *
Tcl_MethodName(
    Tcl_Method method)
{
    return ((Method *) method)->namePtr;
}

int
Tcl_MethodIsType(
    Tcl_Method method,
    const Tcl_MethodType *typePtr,
    void **clientDataPtr)
{
    Method *mPtr = (Method *) method;

    if (mPtr->typePtr == typePtr) {
	if (clientDataPtr != NULL) {
	    *clientDataPtr = mPtr->clientData;
	}
	return 1;
    }
    return 0;
}

int
Tcl_MethodIsPublic(
    Tcl_Method method)
{
    return (((Method *)method)->flags & PUBLIC_METHOD) ? 1 : 0;
}

/*
 * Extended method construction for itcl-ng.
 */

Tcl_Method
TclOONewProcInstanceMethodEx(
    Tcl_Interp *interp,		/* The interpreter containing the object. */
    Tcl_Object oPtr,		/* The object to modify. */
    TclOO_PreCallProc *preCallPtr,
    TclOO_PostCallProc *postCallPtr,
    ProcErrorProc *errProc,
    void *clientData,
    Tcl_Obj *nameObj,		/* The name of the method, which must not be
				 * NULL. */
    Tcl_Obj *argsObj,		/* The formal argument list for the method,
				 * which must not be NULL. */
    Tcl_Obj *bodyObj,		/* The body of the method, which must not be
				 * NULL. */
    int flags,			/* Whether this is a public method. */
    void **internalTokenPtr)	/* If non-NULL, points to a variable that gets
				 * the reference to the ProcedureMethod
				 * structure. */
{
    ProcedureMethod *pmPtr;
    Tcl_Method method = (Tcl_Method) TclOONewProcInstanceMethod(interp,
	    (Object *) oPtr, flags, nameObj, argsObj, bodyObj, &pmPtr);

    if (method == NULL) {
	return NULL;
    }
    pmPtr->flags = flags & USE_DECLARER_NS;
    pmPtr->preCallProc = preCallPtr;
    pmPtr->postCallProc = postCallPtr;
    pmPtr->errProc = errProc;
    pmPtr->clientData = clientData;
    if (internalTokenPtr != NULL) {
	*internalTokenPtr = pmPtr;
    }
    return method;
}

Tcl_Method
TclOONewProcMethodEx(
    Tcl_Interp *interp,		/* The interpreter containing the class. */
    Tcl_Class clsPtr,		/* The class to modify. */
    TclOO_PreCallProc *preCallPtr,
    TclOO_PostCallProc *postCallPtr,
    ProcErrorProc *errProc,
    void *clientData,
    Tcl_Obj *nameObj,		/* The name of the method, which may be NULL;
				 * if so, up to caller to manage storage
				 * (e.g., because it is a constructor or
				 * destructor). */
    Tcl_Obj *argsObj,		/* The formal argument list for the method,
				 * which may be NULL; if so, it is equivalent
				 * to an empty list. */
    Tcl_Obj *bodyObj,		/* The body of the method, which must not be
				 * NULL. */
    int flags,			/* Whether this is a public method. */
    void **internalTokenPtr)	/* If non-NULL, points to a variable that gets
				 * the reference to the ProcedureMethod
				 * structure. */
{
    ProcedureMethod *pmPtr;
    Tcl_Method method = (Tcl_Method) TclOONewProcMethod(interp,
	    (Class *) clsPtr, flags, nameObj, argsObj, bodyObj, &pmPtr);

    if (method == NULL) {
	return NULL;
    }
    pmPtr->flags = flags & USE_DECLARER_NS;
    pmPtr->preCallProc = preCallPtr;
    pmPtr->postCallProc = postCallPtr;
    pmPtr->errProc = errProc;
    pmPtr->clientData = clientData;
    if (internalTokenPtr != NULL) {
	*internalTokenPtr = pmPtr;
    }
    return method;
}

/*
 * Local Variables:
 * mode: c
 * c-basic-offset: 4
 * fill-column: 78
 * End:
 */
