using System;
using System.Runtime.InteropServices;
using System.Threading;
using Microsoft.VisualStudio.Debugger.Interop;

namespace RAD
{
    [ComVisible(true)]
    [Guid(ClassIdString)]
    [ClassInterface(ClassInterfaceType.None)]
    public sealed class RadDbgEngine : IDebugEngine2, IDebugEngineLaunch2
    {
        public const string EngineIdString = "97bd1aec-93d9-4748-b28d-e7e8eca0f781";
        public const string ClassIdString = "56b7b5ea-2c5e-4486-904e-4bf87a110c9b";

        private static readonly Guid EngineId = new Guid(EngineIdString);

        private readonly object syncRoot = new object();
        private RadDbgSessionHandle? session;
        private IDebugPort2? port;
        private IDebugProcess2? process;
        private RadDbgProgram? program;
        private IDebugEventCallback2? callback;
        private Thread? eventThread;
        private RadDbgSessionHandle? workerDisposeSession;
        private bool eventWorkerStarted;
        private bool hasLaunched;
        private int programExited;

        public int EnumPrograms(out IEnumDebugPrograms2 ppEnum)
        {
            lock (this.syncRoot)
            {
                if (this.program == null)
                {
                    ppEnum = null!;
                    return RadDbgHResult.S_OK;
                }
                ppEnum = new RadDbgProgramEnumerator(this.program);
                return RadDbgHResult.S_OK;
            }
        }

        public int Attach(
            IDebugProgram2[] rgpPrograms,
            IDebugProgramNode2[] rgpProgramNodes,
            uint celtPrograms,
            IDebugEventCallback2 pCallback,
            enum_ATTACH_REASON dwReason)
        {
            if (rgpPrograms == null || celtPrograms != 1 || rgpPrograms.Length == 0 ||
                rgpPrograms[0] == null || pCallback == null)
            {
                return RadDbgHResult.E_INVALIDARG;
            }

            lock (this.syncRoot)
            {
                if (this.session == null)
                {
                    return RadDbgHResult.E_INVALIDARG;
                }
                if (this.callback != null)
                {
                    return RadDbgHResult.E_UNEXPECTED;
                }
            }

            int result = rgpPrograms[0].GetProgramId(out Guid programId);
            if (result < 0)
            {
                return result;
            }

            IDebugProcess2? attachedProcess;
            lock (this.syncRoot)
            {
                attachedProcess = this.process;
            }
            if (attachedProcess == null)
            {
                result = rgpPrograms[0].GetProcess(out attachedProcess);
                if (result < 0)
                {
                    return result;
                }
            }

            RadDbgProgram attachedProgram = new RadDbgProgram(this, attachedProcess, programId);
            lock (this.syncRoot)
            {
                if (this.callback != null || this.session == null)
                {
                    return RadDbgHResult.E_UNEXPECTED;
                }
                this.process = attachedProcess;
                this.program = attachedProgram;
                this.callback = pCallback;
            }

            RadDbgEngineCreateEvent engineEvent = new RadDbgEngineCreateEvent(this);
            Guid engineEventIid = typeof(IDebugEngineCreateEvent2).GUID;
            result = pCallback.Event(
                this,
                null!,
                attachedProgram,
                null!,
                engineEvent,
                ref engineEventIid,
                (uint)enum_EVENTATTRIBUTES.EVENT_ASYNCHRONOUS);
            if (result < 0)
            {
                this.RollBackAttach(attachedProgram, pCallback);
                return result;
            }

            RadDbgProgramCreateEvent programEvent = new RadDbgProgramCreateEvent();
            Guid programEventIid = typeof(IDebugProgramCreateEvent2).GUID;
            result = pCallback.Event(
                this,
                null!,
                attachedProgram,
                null!,
                programEvent,
                ref programEventIid,
                (uint)enum_EVENTATTRIBUTES.EVENT_SYNCHRONOUS);
            if (result < 0)
            {
                this.RollBackAttach(attachedProgram, pCallback);
            }
            return result;
        }

        public int CreatePendingBreakpoint(IDebugBreakpointRequest2 pBPRequest, out IDebugPendingBreakpoint2 ppPendingBP)
        {
            ppPendingBP = null!;
            return RadDbgHResult.E_NOTIMPL;
        }

        public int SetException(EXCEPTION_INFO[] pException)
        {
            return RadDbgHResult.E_NOTIMPL;
        }

        public int RemoveSetException(EXCEPTION_INFO[] pException)
        {
            return RadDbgHResult.E_NOTIMPL;
        }

        public int RemoveAllSetExceptions(ref Guid guidType)
        {
            return RadDbgHResult.E_NOTIMPL;
        }

        public int GetEngineId(out Guid pguidEngine)
        {
            pguidEngine = EngineId;
            return RadDbgHResult.S_OK;
        }

        public int DestroyProgram(IDebugProgram2 pProgram)
        {
            lock (this.syncRoot)
            {
                if (this.program == null || !ReferenceEquals(this.program, pProgram))
                {
                    return RadDbgHResult.E_INVALIDARG;
                }
                this.program = null;
            }

            this.CloseSession();
            return RadDbgHResult.S_OK;
        }

        public int ContinueFromSynchronousEvent(IDebugEvent2 pEvent)
        {
            if (pEvent is RadDbgProgramDestroyEvent destroyEvent)
            {
                return this.WithSession(session => RadDbgNative.ContinueSynchronousEvent(session, destroyEvent.Sequence));
            }
            if (pEvent is RadDbgProgramCreateEvent)
            {
                return this.ContinueInitialSynchronousEvent();
            }
            return RadDbgHResult.E_INVALIDARG;
        }

        public int SetLocale(ushort wLangID)
        {
            return RadDbgHResult.S_OK;
        }

        public int SetRegistryRoot(string pszRegistryRoot)
        {
            return RadDbgHResult.S_OK;
        }

        public int SetMetric(string pszMetric, object varValue)
        {
            return RadDbgHResult.S_OK;
        }

        public int CauseBreak()
        {
            return this.BreakSession();
        }

        public int LaunchSuspended(
            string pszServer,
            IDebugPort2 pPort,
            string pszExe,
            string pszArgs,
            string pszDir,
            string bstrEnv,
            string pszOptions,
            enum_LAUNCH_FLAGS dwLaunchFlags,
            uint hStdInput,
            uint hStdOutput,
            uint hStdError,
            IDebugEventCallback2 pCallback,
            out IDebugProcess2 ppProcess)
        {
            ppProcess = null!;
            if (pPort == null || pszExe == null)
            {
                return RadDbgHResult.E_INVALIDARG;
            }

            lock (this.syncRoot)
            {
                if (this.session != null || this.hasLaunched)
                {
                    return RadDbgHResult.E_INVALIDARG;
                }
            }

            int result = RadDbgNative.CreateSession(out RadDbgSessionHandle? createdSession);
            if (result < 0 || createdSession == null)
            {
                return result < 0 ? result : RadDbgHResult.E_FAIL;
            }

            result = RadDbgNative.Launch(createdSession, pszExe, pszArgs, pszDir, out AD_PROCESS_ID processId);
            if (result < 0)
            {
                createdSession.Dispose();
                return result;
            }

            result = pPort.GetProcess(processId, out IDebugProcess2 launchedProcess);
            if (result < 0)
            {
                RadDbgNative.Terminate(createdSession);
                createdSession.Dispose();
                return result;
            }

            lock (this.syncRoot)
            {
                this.session = createdSession;
                this.hasLaunched = true;
                this.port = pPort;
                this.process = launchedProcess;
                this.programExited = 0;
            }
            ppProcess = launchedProcess;
            return RadDbgHResult.S_OK;
        }

        public int ResumeProcess(IDebugProcess2 pProcess)
        {
            IDebugPort2? currentPort;
            lock (this.syncRoot)
            {
                if (this.session == null || pProcess == null || this.port == null)
                {
                    return RadDbgHResult.E_INVALIDARG;
                }
                currentPort = this.port;
            }

            if (!(currentPort is IDebugDefaultPort2 defaultPort))
            {
                return RadDbgHResult.E_NOINTERFACE;
            }
            int result = defaultPort.GetPortNotify(out IDebugPortNotify2 notify);
            if (result < 0)
            {
                return result;
            }
            return notify.AddProgramNode(new RadDbgProgramNode(this));
        }

        public int CanTerminateProcess(IDebugProcess2 pProcess)
        {
            lock (this.syncRoot)
            {
                return this.session != null && pProcess != null
                    ? RadDbgHResult.S_OK
                    : RadDbgHResult.E_INVALIDARG;
            }
        }

        public int TerminateProcess(IDebugProcess2 pProcess)
        {
            lock (this.syncRoot)
            {
                if (this.session == null || pProcess == null)
                {
                    return RadDbgHResult.E_INVALIDARG;
                }
            }
            return this.TerminateSession();
        }

        internal int RunSession()
        {
            if (Interlocked.CompareExchange(ref this.programExited, 0, 0) != 0)
            {
                return RadDbgHResult.S_OK;
            }

            int result = this.StartEventWorker();
            return result < 0 ? result : this.WithSession(RadDbgNative.Run);
        }

        internal int BreakSession()
        {
            return this.WithSession(RadDbgNative.Break);
        }

        internal int TerminateSession()
        {
            return this.WithSession(RadDbgNative.Terminate);
        }

        internal int GetProgramName(out string name)
        {
            return this.WithSession(RadDbgNative.GetProgramName, out name);
        }

        internal int GetHostName(enum_GETHOSTNAME_TYPE type, out string name)
        {
            name = string.Empty;
            RadDbgSessionHandle? currentSession = this.GetSession();
            return currentSession == null
                ? RadDbgHResult.E_UNEXPECTED
                : RadDbgNative.GetHostName(currentSession, type, out name);
        }

        internal int GetHostPid(out AD_PROCESS_ID processId)
        {
            processId = default;
            RadDbgSessionHandle? currentSession = this.GetSession();
            return currentSession == null
                ? RadDbgHResult.E_UNEXPECTED
                : RadDbgNative.GetHostPid(currentSession, out processId);
        }

        internal int GetHostMachineName(out string name)
        {
            return this.WithSession(RadDbgNative.GetHostMachineName, out name);
        }

        internal int GetEngineInfo(out string name, out Guid engineId)
        {
            name = string.Empty;
            engineId = EngineId;
            RadDbgSessionHandle? currentSession = this.GetSession();
            return currentSession == null
                ? RadDbgHResult.E_UNEXPECTED
                : RadDbgNative.GetEngineInfo(currentSession, out name, out engineId);
        }

        internal int CopyThreads(out THREADPROPERTIES[] threads)
        {
            threads = Array.Empty<THREADPROPERTIES>();
            RadDbgSessionHandle? currentSession = this.GetSession();
            return currentSession == null
                ? RadDbgHResult.E_UNEXPECTED
                : RadDbgNative.CopyThreads(currentSession, out threads);
        }

        internal int GetThreadProperties(uint threadId, enum_THREADPROPERTY_FIELDS fields, THREADPROPERTIES[] properties)
        {
            RadDbgSessionHandle? currentSession = this.GetSession();
            return currentSession == null
                ? RadDbgHResult.E_UNEXPECTED
                : RadDbgNative.GetThreadProperties(currentSession, threadId, fields, properties);
        }

        private int StartEventWorker()
        {
            lock (this.syncRoot)
            {
                if (this.eventWorkerStarted)
                {
                    return RadDbgHResult.S_OK;
                }
                if (this.session == null || this.callback == null || this.program == null)
                {
                    return RadDbgHResult.E_UNEXPECTED;
                }

                this.eventWorkerStarted = true;
                this.eventThread = new Thread(this.EventWorker)
                {
                    IsBackground = true,
                    Name = "RAD AD7 event wait",
                };
                this.eventThread.SetApartmentState(ApartmentState.MTA);
                try
                {
                    this.eventThread.Start();
                }
                catch (Exception exception)
                {
                    this.eventThread = null;
                    this.eventWorkerStarted = false;
                    return Marshal.GetHRForException(exception);
                }
                return RadDbgHResult.S_OK;
            }
        }

        private int ContinueInitialSynchronousEvent()
        {
            int result = this.StartEventWorker();
            return result < 0
                ? result
                : this.WithSession(session => RadDbgNative.ContinueSynchronousEvent(session, 0));
        }

        private void EventWorker()
        {
            RadDbgSessionHandle? currentSession = this.GetSession();
            if (currentSession == null)
            {
                return;
            }

            try
            {
                for (;;)
                {
                    int result = RadDbgNative.WaitEvent(currentSession, uint.MaxValue, out RadDbgNativeEvent? nativeEvent);
                    if (RadDbgNative.IsTimeout(result))
                    {
                        continue;
                    }
                    if (result < 0 || nativeEvent == null)
                    {
                        break;
                    }
                    if (this.DispatchEvent(nativeEvent) < 0)
                    {
                        break;
                    }
                }
            }
            catch (Exception)
            {
                // A disconnected AD7 callback must not become an unhandled exception on devenv's worker thread.
            }
            finally
            {
                RadDbgSessionHandle? disposeSession = null;
                lock (this.syncRoot)
                {
                    this.eventThread = null;
                    this.eventWorkerStarted = false;
                    if (ReferenceEquals(this.workerDisposeSession, currentSession))
                    {
                        disposeSession = this.workerDisposeSession;
                        this.workerDisposeSession = null;
                    }
                }
                disposeSession?.Dispose();
            }
        }

        private int DispatchEvent(RadDbgNativeEvent nativeEvent)
        {
            IDebugEventCallback2? currentCallback;
            RadDbgProgram? currentProgram;
            lock (this.syncRoot)
            {
                currentCallback = this.callback;
                currentProgram = this.program;
            }
            if (currentCallback == null || currentProgram == null)
            {
                return RadDbgHResult.E_UNEXPECTED;
            }

            IDebugEvent2? debugEvent = RadDbgEventFactory.Create(nativeEvent);
            if (debugEvent == null)
            {
                return RadDbgHResult.S_OK;
            }
            if (debugEvent is RadDbgProgramDestroyEvent)
            {
                Interlocked.Exchange(ref this.programExited, 1);
            }

            IDebugThread2? thread = nativeEvent.ThreadProperties.dwThreadId == 0
                ? null
                : new RadDbgThread(this, currentProgram, nativeEvent.ThreadProperties.dwThreadId);
            Guid eventIid = nativeEvent.EventIid;
            return currentCallback.Event(
                this,
                null!,
                currentProgram,
                thread!,
                debugEvent,
                ref eventIid,
                nativeEvent.Attributes);
        }

        private void CloseSession()
        {
            RadDbgSessionHandle? closingSession;
            Thread? worker;
            RadDbgSessionHandle? disposeSession = null;
            lock (this.syncRoot)
            {
                closingSession = this.session;
                if (closingSession == null)
                {
                    return;
                }
                this.session = null;
                worker = this.eventThread;
                this.callback = null;
                this.process = null;
                this.port = null;
                if (worker != null && worker.IsAlive)
                {
                    this.workerDisposeSession = closingSession;
                }
                else
                {
                    disposeSession = closingSession;
                    this.eventThread = null;
                    this.eventWorkerStarted = false;
                }
            }

            RadDbgNative.CloseEventWait(closingSession);
            disposeSession?.Dispose();
        }

        private void RollBackAttach(RadDbgProgram attachedProgram, IDebugEventCallback2 attachedCallback)
        {
            lock (this.syncRoot)
            {
                if (ReferenceEquals(this.program, attachedProgram))
                {
                    this.program = null;
                }
                if (ReferenceEquals(this.callback, attachedCallback))
                {
                    this.callback = null;
                }
            }
        }

        private RadDbgSessionHandle? GetSession()
        {
            lock (this.syncRoot)
            {
                return this.session;
            }
        }

        private int WithSession(Func<RadDbgSessionHandle, int> action)
        {
            RadDbgSessionHandle? currentSession = this.GetSession();
            return currentSession == null ? RadDbgHResult.E_UNEXPECTED : action(currentSession);
        }

        private int WithSession(SessionStringOperation action, out string value)
        {
            value = string.Empty;
            RadDbgSessionHandle? currentSession = this.GetSession();
            return currentSession == null ? RadDbgHResult.E_UNEXPECTED : action(currentSession, out value);
        }

        private delegate int SessionStringOperation(RadDbgSessionHandle session, out string value);
    }
}
