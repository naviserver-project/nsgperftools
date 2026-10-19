/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * Copyright (C) 2026 John Buckman
 */

/*
 * nsgperftools.c --
 *
 *      Heap profiling and heap sampling for NaviServer with gperftools.
 *      Defines the Tcl command "ns_heapprofile".
 *
 *      Nothing here is linked against gperftools. Its functions are looked
 *      up at run time with dlsym(RTLD_DEFAULT, ...) and are found when the
 *      full tcmalloc (libtcmalloc.so, not libtcmalloc_minimal.so) is in the
 *      process, normally through LD_PRELOAD. Without it the module still
 *      loads, and the subcommands return an error.
 *
 *      gperftools reads its settings from the environment once, when the
 *      library is loaded, so they belong in the server's launch environment:
 *
 *        HEAP_PROFILE_ALLOCATION_INTERVAL=0     no automatic profile dumps:
 *        HEAP_PROFILE_DEALLOCATION_INTERVAL=0   0 turns a trigger off, while
 *        HEAP_PROFILE_INUSE_INTERVAL=0          a large value only postpones
 *        HEAP_PROFILE_TIME_INTERVAL=0           it
 *        TCMALLOC_SAMPLE_PARAMETER=524288       optional: heap sampling
 *
 *      Leave HEAPPROFILE unset. It starts the profiler when the library is
 *      loaded, in nsd and in every program nsd executes, and each of those
 *      programs then prints "Starting tracking the heap" to stderr, which
 *      makes Tcl's exec fail. "ns_heapprofile start" starts the profiler
 *      inside nsd only.
 */

#include "ns.h"
#include <dlfcn.h>
#include <errno.h>

#define NSGPERFTOOLS_VERSION "0.1"

NS_EXTERN const int Ns_ModuleVersion;
NS_EXPORT const int Ns_ModuleVersion = 1;

NS_EXPORT Ns_ModuleInitProc Ns_ModuleInit;
#ifdef NS_MODULE_INFO_VERSION
NS_EXPORT Ns_ModuleInfoProc Ns_ModuleGetInfo;
#endif
NS_EXPORT int Nsgperftools_Init(Tcl_Interp *interp);

/*
 * The heap sample has no C entry point in gperftools; see heapsample.cpp.
 */
extern char *NsgpHeapSample(size_t *lengthPtr);

/*
 * gperftools C entry points (declared extern "C" by gperftools).
 */
typedef void  (HeapProfilerStartProc)(const char *prefix);
typedef void  (HeapProfilerStopProc)(void);
typedef void  (HeapProfilerDumpProc)(const char *reason);
typedef int   (IsHeapProfilerRunningProc)(void);
typedef char *(GetHeapProfileProc)(void);
typedef void  (GetStatsProc)(char *buffer, int bufferLength);
typedef int   (GetNumericPropertyProc)(const char *property, size_t *value);

/*
 * tcmalloc's sampling period: the exported C++ global behind
 * TCMALLOC_SAMPLE_PARAMETER, defined with gperftools' own DEFINE_int64
 * (checked with gperftools 2.4). Only "sampling -force" writes it.
 */
#define SAMPLE_PARAMETER_SYMBOL \
    "_ZN61FLAG__namespace_do_not_use_directly_use_DECLARE_int64_instead31FLAGS_tcmalloc_sample_parameterE"

static TCL_OBJCMDPROC_T HeapProfileObjCmd;
static TCL_OBJCMDPROC_T DumpObjCmd;
static TCL_OBJCMDPROC_T GetObjCmd;
static TCL_OBJCMDPROC_T HeapSampleObjCmd;
static TCL_OBJCMDPROC_T PropertyObjCmd;
static TCL_OBJCMDPROC_T RunningObjCmd;
static TCL_OBJCMDPROC_T SamplingObjCmd;
static TCL_OBJCMDPROC_T StartObjCmd;
static TCL_OBJCMDPROC_T StatsObjCmd;
static TCL_OBJCMDPROC_T StopObjCmd;

static Ns_TclTraceProc AddCmds;

static ns_funcptr_t LookupFunction(Tcl_Interp *interp, const char *name)
    NS_GNUC_NONNULL(2);
static int CheckInterval(Tcl_DString *dsPtr, const char *name, long long defaultValue)
    NS_GNUC_NONNULL(1,2);
static bool SamplePeriod(long long *periodPtr)
    NS_GNUC_NONNULL(1);


/*
 *----------------------------------------------------------------------
 *
 * Ns_ModuleInit --
 *
 *      Register ns_heapprofile in every interpreter of the server.
 *
 * Results:
 *      NS_OK or NS_ERROR.
 *
 * Side effects:
 *      Adds an interpreter-create trace.
 *
 *----------------------------------------------------------------------
 */

NS_EXPORT Ns_ReturnCode
Ns_ModuleInit(const char *server, const char *UNUSED(module))
{
    if (server == NULL) {
        Ns_Log(Error, "nsgperftools: load the module per server "
               "(ns/server/<server>/modules), not globally");
        return NS_ERROR;
    }
    if (dlsym(RTLD_DEFAULT, "HeapProfilerStart") == NULL) {
        Ns_Log(Warning, "nsgperftools: the gperftools heap profiler is not loaded "
               "(LD_PRELOAD the full libtcmalloc.so); ns_heapprofile will return errors");
    }
    return Ns_TclRegisterTrace(server, AddCmds, NULL, NS_TCL_TRACE_CREATE);
}

#ifdef NS_MODULE_INFO_VERSION
/*
 * Provide module build and ABI information for runtime introspection.
 */
NS_EXPORT void
Ns_ModuleGetInfo(Ns_ModuleInfo *infoPtr)
{
    Ns_ModuleInfoInit(infoPtr, NS_MODULE_INFO_VERSION,
                      "nsgperftools",
                      NSGPERFTOOLS_VERSION,
#ifdef NS_MODULE_TAG
                      NS_MODULE_TAG,    /* git describe, from Makefile.module */
#else
                      "",
#endif
                      "module",
                      1u);
}
#endif

/*
 *----------------------------------------------------------------------
 *
 * Nsgperftools_Init --
 *
 *      Entry point for Tcl's "load": registers ns_heapprofile in the
 *      calling interpreter only, without a server restart. The profiler
 *      itself is process-wide.
 *
 * Results:
 *      Tcl result code.
 *
 * Side effects:
 *      Creates a Tcl command.
 *
 *----------------------------------------------------------------------
 */

NS_EXPORT int
Nsgperftools_Init(Tcl_Interp *interp)
{
    (void)TCL_CREATEOBJCOMMAND(interp, "ns_heapprofile", HeapProfileObjCmd, NULL, NULL);
    return Tcl_PkgProvide(interp, "nsgperftools", NSGPERFTOOLS_VERSION);
}

static int
AddCmds(Tcl_Interp *interp, const void *UNUSED(arg))
{
    (void)TCL_CREATEOBJCOMMAND(interp, "ns_heapprofile", HeapProfileObjCmd, NULL, NULL);
    return TCL_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * LookupFunction --
 *
 *      Find a gperftools function in the process.
 *
 * Results:
 *      The function, or NULL with an error message in interp (when given).
 *      The caller casts it to the function's real type.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

static ns_funcptr_t
LookupFunction(Tcl_Interp *interp, const char *name)
{
    union {
        void         *object;
        ns_funcptr_t  function;
    } addr;

    /*
     * dlsym() returns a function as an object pointer, which ISO C does not
     * convert to a function pointer directly.
     */
    addr.object = dlsym(RTLD_DEFAULT, name);
    if (addr.object == NULL && interp != NULL) {
        Ns_TclPrintfResult(interp, "gperftools function \"%s\" not found: the full "
                           "tcmalloc (libtcmalloc.so, not libtcmalloc_minimal.so) must "
                           "be loaded, e.g. with LD_PRELOAD", name);
    }
    return (addr.object == NULL) ? NULL : addr.function;
}

/*
 *----------------------------------------------------------------------
 *
 * CheckInterval --
 *
 *      Check that an automatic-dump interval from the environment is 0.
 *
 * Results:
 *      0 when it is, 1 otherwise (with a description appended to dsPtr).
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

static int
CheckInterval(Tcl_DString *dsPtr, const char *name, long long defaultValue)
{
    const char *value = getenv(name);
    long long   interval = defaultValue;

    if (value != NULL) {
        char *end;

        errno = 0;
        interval = strtoll(value, &end, 10);
        if (errno != 0 || end == value || *end != '\0') {
            Ns_DStringPrintf(dsPtr, " %s=\"%s\" is not a number;", name, value);
            return 1;
        }
    }
    if (interval != 0) {
        Ns_DStringPrintf(dsPtr, " %s is %lld%s;", name, interval,
                         value == NULL ? " (unset: gperftools default)" : "");
        return 1;
    }
    return 0;
}

/*
 *----------------------------------------------------------------------
 *
 * SamplePeriod --
 *
 *      Get tcmalloc's current heap-sampling period, from its setting when
 *      the symbol is available, else from TCMALLOC_SAMPLE_PARAMETER.
 *
 * Results:
 *      NS_TRUE when the period is known (stored in *periodPtr).
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

static bool
SamplePeriod(long long *periodPtr)
{
    const int64_t *flagPtr = (const int64_t *)dlsym(RTLD_DEFAULT, SAMPLE_PARAMETER_SYMBOL);
    const char    *value;

    if (flagPtr != NULL) {
        *periodPtr = (long long)*flagPtr;
        return NS_TRUE;
    }
    value = getenv("TCMALLOC_SAMPLE_PARAMETER");
    *periodPtr = (value != NULL) ? strtoll(value, NULL, 10) : 0;
    return (value != NULL);
}

/*
 *----------------------------------------------------------------------
 *
 * HeapProfileObjCmd --
 *
 *      Implements "ns_heapprofile".
 *
 * Results:
 *      Tcl result code.
 *
 * Side effects:
 *      Depends on the subcommand.
 *
 *----------------------------------------------------------------------
 */

static int
HeapProfileObjCmd(ClientData clientData, Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
    const Ns_SubCmdSpec subcmds[] = {
        {"dump",       DumpObjCmd},
        {"get",        GetObjCmd},
        {"heapsample", HeapSampleObjCmd},
        {"property",   PropertyObjCmd},
        {"running",    RunningObjCmd},
        {"sampling",   SamplingObjCmd},
        {"start",      StartObjCmd},
        {"stats",      StatsObjCmd},
        {"stop",       StopObjCmd},
        {NULL, NULL}
    };

    return Ns_SubcmdObjv(subcmds, clientData, interp, objc, objv);
}

/*
 * ns_heapprofile start ?-force? ?--? ?prefix?
 *
 * Start the heap profiler. Dumps go to <prefix>.NNNN.heap. Without -force it
 * refuses unless all four automatic-dump intervals are 0.
 */
static int
StartObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
    int                        force = (int)NS_FALSE;
    char                      *prefix = (char *)"/tmp/nsgperftools";
    HeapProfilerStartProc     *startProc;
    IsHeapProfilerRunningProc *runningProc;
    Ns_ObjvSpec opts[] = {
        {"-force", Ns_ObjvBool,  &force, INT2PTR(NS_TRUE)},
        {"--",     Ns_ObjvBreak, NULL,   NULL},
        {NULL, NULL, NULL, NULL}
    };
    Ns_ObjvSpec args[] = {
        {"?prefix", Ns_ObjvString, &prefix, NULL},
        {NULL, NULL, NULL, NULL}
    };

    if (Ns_ParseObjv(opts, args, interp, 2, objc, objv) != NS_OK) {
        return TCL_ERROR;
    }
    startProc = (HeapProfilerStartProc *)LookupFunction(interp, "HeapProfilerStart");
    runningProc = (IsHeapProfilerRunningProc *)LookupFunction(interp, "IsHeapProfilerRunning");
    if (startProc == NULL || runningProc == NULL) {
        return TCL_ERROR;
    }
    if ((*runningProc)() != 0) {
        Ns_TclPrintfResult(interp, "the heap profiler is already running");
        return TCL_ERROR;
    }
    if (force == (int)NS_FALSE) {
        Tcl_DString ds;
        int         problems;

        Tcl_DStringInit(&ds);
        problems = CheckInterval(&ds, "HEAP_PROFILE_ALLOCATION_INTERVAL", 1LL << 30)
            + CheckInterval(&ds, "HEAP_PROFILE_DEALLOCATION_INTERVAL", 0)
            + CheckInterval(&ds, "HEAP_PROFILE_INUSE_INTERVAL", 100LL << 20)
            + CheckInterval(&ds, "HEAP_PROFILE_TIME_INTERVAL", 0);
        if (problems > 0) {
            Ns_TclPrintfResult(interp, "automatic heap-profile dumps are not off:%s set all "
                               "four HEAP_PROFILE_*_INTERVAL variables to 0 in the server's "
                               "launch environment (gperftools reads them only at startup), "
                               "or use -force", ds.string);
            Tcl_DStringFree(&ds);
            return TCL_ERROR;
        }
        Tcl_DStringFree(&ds);
    }
    (*startProc)(prefix);
    Ns_Log(Notice, "nsgperftools: heap profiler started, prefix %s%s",
           prefix, force ? " (-force)" : "");
    Tcl_SetObjResult(interp, Tcl_NewStringObj(prefix, TCL_INDEX_NONE));
    return TCL_OK;
}

/*
 * ns_heapprofile stop
 */
static int
StopObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
    HeapProfilerStopProc *stopProc;

    if (Ns_ParseObjv(NULL, NULL, interp, 2, objc, objv) != NS_OK) {
        return TCL_ERROR;
    }
    stopProc = (HeapProfilerStopProc *)LookupFunction(interp, "HeapProfilerStop");
    if (stopProc == NULL) {
        return TCL_ERROR;
    }
    (*stopProc)();
    Ns_Log(Notice, "nsgperftools: heap profiler stopped");
    return TCL_OK;
}

/*
 * ns_heapprofile running
 */
static int
RunningObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
    IsHeapProfilerRunningProc *runningProc;

    if (Ns_ParseObjv(NULL, NULL, interp, 2, objc, objv) != NS_OK) {
        return TCL_ERROR;
    }
    runningProc = (IsHeapProfilerRunningProc *)LookupFunction(interp, "IsHeapProfilerRunning");
    if (runningProc == NULL) {
        return TCL_ERROR;
    }
    Tcl_SetObjResult(interp, Tcl_NewBooleanObj((*runningProc)() != 0));
    return TCL_OK;
}

/*
 * ns_heapprofile dump ?reason?
 *
 * Write the next <prefix>.NNNN.heap file.
 */
static int
DumpObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
    char                      *reason = (char *)"ns_heapprofile dump";
    HeapProfilerDumpProc      *dumpProc;
    IsHeapProfilerRunningProc *runningProc;
    Ns_ObjvSpec args[] = {
        {"?reason", Ns_ObjvString, &reason, NULL},
        {NULL, NULL, NULL, NULL}
    };

    if (Ns_ParseObjv(NULL, args, interp, 2, objc, objv) != NS_OK) {
        return TCL_ERROR;
    }
    dumpProc = (HeapProfilerDumpProc *)LookupFunction(interp, "HeapProfilerDump");
    runningProc = (IsHeapProfilerRunningProc *)LookupFunction(interp, "IsHeapProfilerRunning");
    if (dumpProc == NULL || runningProc == NULL) {
        return TCL_ERROR;
    }
    if ((*runningProc)() == 0) {
        Ns_TclPrintfResult(interp, "the heap profiler is not running (ns_heapprofile start)");
        return TCL_ERROR;
    }
    (*dumpProc)(reason);
    return TCL_OK;
}

/*
 * ns_heapprofile get
 *
 * The current heap profile as text.
 */
static int
GetObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
    GetHeapProfileProc        *getProc;
    IsHeapProfilerRunningProc *runningProc;
    char                      *profile;

    if (Ns_ParseObjv(NULL, NULL, interp, 2, objc, objv) != NS_OK) {
        return TCL_ERROR;
    }
    getProc = (GetHeapProfileProc *)LookupFunction(interp, "GetHeapProfile");
    runningProc = (IsHeapProfilerRunningProc *)LookupFunction(interp, "IsHeapProfilerRunning");
    if (getProc == NULL || runningProc == NULL) {
        return TCL_ERROR;
    }
    /*
     * When the profiler is off, GetHeapProfile() returns an empty string
     * rather than NULL.
     */
    if ((*runningProc)() == 0) {
        Ns_TclPrintfResult(interp, "the heap profiler is not running (ns_heapprofile start)");
        return TCL_ERROR;
    }
    profile = (*getProc)();
    if (profile == NULL) {
        Ns_TclPrintfResult(interp, "GetHeapProfile() returned no profile");
        return TCL_ERROR;
    }
    Tcl_SetObjResult(interp, Tcl_NewStringObj(profile, TCL_INDEX_NONE));
    free(profile);    /* allocated by gperftools with malloc() */
    return TCL_OK;
}

/*
 * ns_heapprofile stats
 *
 * tcmalloc's human-readable statistics (MallocExtension_GetStats).
 */
static int
StatsObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
    const int     bufferLength = 64 * 1024;
    GetStatsProc *statsProc;
    char         *buffer;

    if (Ns_ParseObjv(NULL, NULL, interp, 2, objc, objv) != NS_OK) {
        return TCL_ERROR;
    }
    statsProc = (GetStatsProc *)LookupFunction(interp, "MallocExtension_GetStats");
    if (statsProc == NULL) {
        return TCL_ERROR;
    }
    buffer = ns_malloc((size_t)bufferLength);
    buffer[0] = '\0';
    (*statsProc)(buffer, bufferLength);
    Tcl_SetObjResult(interp, Tcl_NewStringObj(buffer, TCL_INDEX_NONE));
    ns_free(buffer);
    return TCL_OK;
}

/*
 * ns_heapprofile property name
 *
 * A tcmalloc numeric property, e.g. generic.current_allocated_bytes.
 */
static int
PropertyObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
    char                   *name = NULL;
    size_t                  value = 0u;
    GetNumericPropertyProc *propertyProc;
    Ns_ObjvSpec args[] = {
        {"name", Ns_ObjvString, &name, NULL},
        {NULL, NULL, NULL, NULL}
    };

    if (Ns_ParseObjv(NULL, args, interp, 2, objc, objv) != NS_OK) {
        return TCL_ERROR;
    }
    propertyProc = (GetNumericPropertyProc *)LookupFunction(interp, "MallocExtension_GetNumericProperty");
    if (propertyProc == NULL) {
        return TCL_ERROR;
    }
    if ((*propertyProc)(name, &value) == 0) {
        Ns_TclPrintfResult(interp, "unknown tcmalloc property \"%s\"", name);
        return TCL_ERROR;
    }
    Tcl_SetObjResult(interp, Tcl_NewWideIntObj((Tcl_WideInt)value));
    return TCL_OK;
}

/*
 * ns_heapprofile sampling ?-force? ?--? ?bytes?
 *
 * Return tcmalloc's heap-sampling period (0 = off). With bytes and -force,
 * change it at run time. The supported way to set it is
 * TCMALLOC_SAMPLE_PARAMETER in the launch environment; -force writes
 * gperftools' internal setting instead (checked with gperftools 2.4, where
 * it takes effect at once).
 */
static int
SamplingObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
    int         force = (int)NS_FALSE;
    Tcl_Obj    *bytesObj = NULL;
    Tcl_WideInt bytes;
    int64_t    *flagPtr;
    long long   period;
    Ns_ObjvSpec opts[] = {
        {"-force", Ns_ObjvBool,  &force, INT2PTR(NS_TRUE)},
        {"--",     Ns_ObjvBreak, NULL,   NULL},
        {NULL, NULL, NULL, NULL}
    };
    Ns_ObjvSpec args[] = {
        {"?bytes", Ns_ObjvObj, &bytesObj, NULL},
        {NULL, NULL, NULL, NULL}
    };

    if (Ns_ParseObjv(opts, args, interp, 2, objc, objv) != NS_OK) {
        return TCL_ERROR;
    }
    if (bytesObj == NULL) {
        (void) SamplePeriod(&period);
        Tcl_SetObjResult(interp, Tcl_NewWideIntObj((Tcl_WideInt)period));
        return TCL_OK;
    }
    if (Tcl_GetWideIntFromObj(interp, bytesObj, &bytes) != TCL_OK) {
        return TCL_ERROR;
    }
    if (bytes < 0) {
        Ns_TclPrintfResult(interp, "sampling period must be 0 (off) or positive");
        return TCL_ERROR;
    }
    if (force == (int)NS_FALSE) {
        Ns_TclPrintfResult(interp, "tcmalloc reads TCMALLOC_SAMPLE_PARAMETER only at startup: "
                           "set it in the server's launch environment, or use -force to "
                           "change gperftools' internal setting at run time");
        return TCL_ERROR;
    }
    flagPtr = (int64_t *)dlsym(RTLD_DEFAULT, SAMPLE_PARAMETER_SYMBOL);
    if (flagPtr == NULL) {
        Ns_TclPrintfResult(interp, "tcmalloc's sampling setting is not accessible in this "
                           "gperftools build; set TCMALLOC_SAMPLE_PARAMETER at startup");
        return TCL_ERROR;
    }
    Ns_Log(Notice, "nsgperftools: heap sampling period %lld -> %lld bytes (-force)",
           (long long)*flagPtr, (long long)bytes);
    *flagPtr = (int64_t)bytes;
    Tcl_SetObjResult(interp, Tcl_NewWideIntObj(bytes));
    return TCL_OK;
}

/*
 * ns_heapprofile heapsample
 *
 * The sampled live heap in pprof format (MallocExtension::GetHeapSample),
 * including the address map. It covers only allocations made while
 * sampling was on.
 */
static int
HeapSampleObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
    long long period;
    size_t    length = 0u;
    char     *sample;

    if (Ns_ParseObjv(NULL, NULL, interp, 2, objc, objv) != NS_OK) {
        return TCL_ERROR;
    }
    if (SamplePeriod(&period) && period <= 0) {
        Ns_TclPrintfResult(interp, "heap sampling is off: set TCMALLOC_SAMPLE_PARAMETER "
                           "(e.g. 524288) in the server's launch environment");
        return TCL_ERROR;
    }
    sample = NsgpHeapSample(&length);
    if (sample == NULL) {
        Ns_TclPrintfResult(interp, "MallocExtension::GetHeapSample not found: the full "
                           "tcmalloc must be loaded, built with the same C++ string ABI "
                           "as this module");
        return TCL_ERROR;
    }
    Tcl_SetObjResult(interp, Tcl_NewStringObj(sample, (TCL_SIZE_T)length));
    free(sample);
    return TCL_OK;
}

/*
 * Local Variables:
 * mode: c
 * c-basic-offset: 4
 * fill-column: 78
 * indent-tabs-mode: nil
 * End:
 */
