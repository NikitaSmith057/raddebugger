using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using Microsoft.VisualStudio;
using Microsoft.VisualStudio.Debugger.Interop;

namespace RAD
{
    [Guid(ClassIdString)]
    [ClassInterface(ClassInterfaceType.None)]
    [ComVisible(true)]
    public sealed class RadDbgEngine : IDebugEngine2, IDebugEngineLaunch2
    {
        public const string EngineIdString = "97bd1aec-93d9-4748-b28d-e7e8eca0f781";
        public const string ClassIdString = "56b7b5ea-2c5e-4486-904e-4bf87a110c9b";
        private static readonly Guid EngineId = new Guid(EngineIdString);

        private RadDbgBridge? bridgeSession;
        private IDebugProcess2? launchProcess;
        private IDebugEventCallback2? callback;
        private RadDbgProgram? program;
        private uint launchSystemProcessId;

        private readonly object programLock = new object();
        private readonly object breakpointLock = new object();
        private readonly Dictionary<ulong, RadDbgProgram> programs = new Dictionary<ulong, RadDbgProgram>();
        private readonly Dictionary<ulong, RadDbgProgramNode> programNodes = new Dictionary<ulong, RadDbgProgramNode>();
        private readonly Dictionary<ulong, RadDbgBoundBreakpoint> boundBreakpoints = new Dictionary<ulong, RadDbgBoundBreakpoint>();
        public int LaunchSuspended(string pszServer, IDebugPort2 pPort, string pszExe, string pszArgs, string pszDir, string bstrEnv, string pszOptions, enum_LAUNCH_FLAGS dwLaunchFlags, uint hStdInput, uint hStdOutput, uint hStdError, IDebugEventCallback2 pCallback, out IDebugProcess2 ppProcess)
        {
            ppProcess = null!;
            if (this.bridgeSession != null)
            {
                return VSConstants.E_FAIL;
            }

            int result = RadDbgBridge.LaunchSuspended(pszExe, pszArgs ?? string.Empty, pszDir ?? string.Empty, out RadDbgBridge? session, out uint processId);
            if (result != VSConstants.S_OK)
            {
                return result;
            }

            AD_PROCESS_ID adProcessId = new AD_PROCESS_ID
            {
                ProcessIdType = (uint)enum_AD_PROCESS_ID.AD_PROCESS_ID_SYSTEM,
                dwProcessId = processId,
            };
            int processResult = pPort.GetProcess(adProcessId, out IDebugProcess2 debugProcess);
            if (processResult < 0)
            {
                session!.Terminate(0);
                session.Dispose();
                return processResult;
            }

            this.bridgeSession = session;
            this.launchProcess = debugProcess;
            this.launchSystemProcessId = processId;
            this.callback = pCallback;
            ppProcess = debugProcess;
            return VSConstants.S_OK;
        }

        public int Attach(IDebugProgram2[] rgpPrograms, IDebugProgramNode2[] rgpProgramNodes, uint celtPrograms, IDebugEventCallback2 pCallback, enum_ATTACH_REASON dwReason)
        {
            if (celtPrograms != 1 || rgpPrograms == null || rgpPrograms.Length == 0 || this.bridgeSession == null || this.program != null)
            {
                return VSConstants.E_FAIL;
            }

            this.callback = pCallback;
            int attachResult = this.bridgeSession.Attach(rgpPrograms, rgpProgramNodes, celtPrograms, pCallback, dwReason);
            if (attachResult < 0 && attachResult != VSConstants.E_NOTIMPL)
            {
                return attachResult;
            }

            int programIdResult = rgpPrograms[0].GetProgramId(out Guid programId);
            if (programIdResult < 0)
            {
                return programIdResult;
            }

            IDebugProcess2? attachedProcess = this.launchProcess;
            if (attachedProcess == null)
            {
                int processResult = rgpPrograms[0].GetProcess(out attachedProcess);
                if (processResult < 0)
                {
                    return processResult;
                }
            }

            this.program = new RadDbgProgram(this, attachedProcess, programId);
            this.program.MarkRegistered();
            this.SendEvent(null, new RadEngineCreateEvent(this), typeof(IDebugEngineCreateEvent2).GUID, null);
            this.SendEvent(this.program, new RadProgramCreateEvent(), typeof(IDebugProgramCreateEvent2).GUID, null);
            return VSConstants.S_OK;
        }

        public int ContinueFromSynchronousEvent(IDebugEvent2 pEvent)
        {
            if (this.bridgeSession != null)
            {
                if (pEvent is RadProgramCreateEvent createEvent)
                {
                    if (createEvent.StartsNativeSession)
                    {
                        if (this.program is RadDbgProgram program)
                        {
                            return this.bridgeSession.ContinueFromSynchronousEvent();
                        }
                    }
                }
                else if (pEvent is RadProgramDestroyEvent destroyEvent)
                {
                    return this.bridgeSession.AcknowledgeEvent(destroyEvent.Sequence);
                }
            }
            return VSConstants.E_FAIL;
        }

        public int CauseBreak()
        {
            return this.BreakProgram();
        }

        public int CreatePendingBreakpoint(IDebugBreakpointRequest2 pBPRequest, out IDebugPendingBreakpoint2 ppPendingBP)
        {
            ppPendingBP = null!;
            int result = RadDbgPendingBreakpoint.Create(this, pBPRequest, out RadDbgPendingBreakpoint? pendingBreakpoint);
            if (result == VSConstants.S_OK)
            {
                ppPendingBP = pendingBreakpoint!;
            }
            return result;
        }

        public int DestroyProgram(IDebugProgram2 pProgram)
        {
            if (!ReferenceEquals(pProgram, this.program))
            {
                return VSConstants.E_FAIL;
            }

            if (this.bridgeSession != null)
            {
                int result = this.bridgeSession.DestroyProgram(pProgram);
                if (result < 0 && result != VSConstants.E_NOTIMPL)
                {
                    return result;
                }
            }

            this.program = null;
            this.DisposeBridgeSession();
            return VSConstants.S_OK;
        }

        public int EnumPrograms(out IEnumDebugPrograms2 ppEnum)
        {
            if (this.bridgeSession != null)
            {
                int result = this.bridgeSession.EnumProgramDescriptors(out _, out _);
                if (result < 0 && result != VSConstants.E_NOTIMPL)
                {
                    ppEnum = null!;
                    return result;
                }
            }

            List<IDebugProgram2> snapshot = new List<IDebugProgram2>();
            lock (this.programLock)
            {
                if (this.program != null)
                {
                    snapshot.Add(this.program);
                }
                foreach (RadDbgProgram childProgram in this.programs.Values)
                {
                    if (!ReferenceEquals(childProgram, this.program))
                    {
                        snapshot.Add(childProgram);
                    }
                }
            }
            ppEnum = new RadDbgProgramEnum(snapshot.ToArray());
            return VSConstants.S_OK;
        }

        public int GetEngineId(out Guid pguidEngine)
        {
            if (this.bridgeSession != null)
            {
                int result = this.bridgeSession.GetEngineId(out pguidEngine);
                if (result == VSConstants.S_OK || result != VSConstants.E_NOTIMPL)
                {
                    return result;
                }
            }

            pguidEngine = EngineId;
            return VSConstants.S_OK;
        }

        public int RemoveAllSetExceptions(ref Guid guidType)
        {
            return this.bridgeSession == null ? VSConstants.E_NOTIMPL : this.bridgeSession.RemoveAllSetExceptions(ref guidType);
        }

        public int RemoveSetException(EXCEPTION_INFO[] pException)
        {
            if (pException == null || pException.Length == 0)
            {
                return VSConstants.E_INVALIDARG;
            }
            return this.bridgeSession == null ? VSConstants.E_NOTIMPL : this.bridgeSession.RemoveSetException(ref pException[0]);
        }

        public int SetException(EXCEPTION_INFO[] pException)
        {
            if (pException == null || pException.Length == 0)
            {
                return VSConstants.E_INVALIDARG;
            }
            return this.bridgeSession == null ? VSConstants.E_NOTIMPL : this.bridgeSession.SetException(ref pException[0]);
        }

        public int SetLocale(ushort wLangID)
        {
            return this.bridgeSession == null ? VSConstants.S_OK : this.bridgeSession.SetLocale(wLangID);
        }

        public int SetMetric(string pszMetric, object varValue)
        {
            return this.bridgeSession == null ? VSConstants.S_OK : this.bridgeSession.SetMetric(pszMetric, varValue);
        }

        public int SetRegistryRoot(string pszRegistryRoot)
        {
            return this.bridgeSession == null ? VSConstants.S_OK : this.bridgeSession.SetRegistryRoot(pszRegistryRoot);
        }

        public int ResumeProcess(IDebugProcess2 pProcess)
        {
            if (this.launchProcess == null)
            {
                return VSConstants.E_FAIL;
            }

            if (!ReferenceEquals(pProcess, this.launchProcess))
            {
                return VSConstants.E_INVALIDARG;
            }

            int resumeResult = this.bridgeSession?.ResumeLaunchProcess(this.program?.ProcessHandle ?? 0) ?? VSConstants.S_OK;
            if (resumeResult < 0 && resumeResult != VSConstants.E_NOTIMPL)
            {
                return resumeResult;
            }

            int portResult = this.launchProcess.GetPort(out IDebugPort2 port);
            if (portResult < 0 || !(port is IDebugDefaultPort2 defaultPort))
            {
                return VSConstants.E_FAIL;
            }

            int notifyResult = defaultPort.GetPortNotify(out IDebugPortNotify2 notify);
            if (notifyResult < 0)
            {
                return notifyResult;
            }

            return notify.AddProgramNode(new RadDbgProgramNode(this));
        }

        public int CanTerminateProcess(IDebugProcess2 pProcess)
        {
            if (!ReferenceEquals(pProcess, this.launchProcess))
            {
                return VSConstants.E_INVALIDARG;
            }
            return this.bridgeSession == null ? VSConstants.E_NOTIMPL : this.bridgeSession.CanTerminateLaunchProcess(this.program?.ProcessHandle ?? 0);
        }

        public int TerminateProcess(IDebugProcess2 pProcess)
        {
            if (!ReferenceEquals(pProcess, this.launchProcess))
            {
                return VSConstants.E_INVALIDARG;
            }

            int result = this.bridgeSession?.TerminateLaunchProcess(this.program?.ProcessHandle ?? 0) ?? VSConstants.E_NOTIMPL;
            return result == VSConstants.E_NOTIMPL ? this.TerminateProgram() : result;
        }

        internal void SetCallback(IDebugEventCallback2 callback)
        {
            this.callback = callback;
        }

        internal RadDbgProgram? CurrentProgram => this.program;

        internal int BreakProgram()
        {
            if (this.bridgeSession == null)
            {
                return VSConstants.E_NOTIMPL;
            }

            int result = this.bridgeSession.EngineBreak();
            return result == VSConstants.E_NOTIMPL ? this.bridgeSession.Break() : result;
        }

        internal int CreateAddressBreakpoint(ulong address, bool enabled, RadDbgAddressBreakpointMode mode, out ulong breakpointId)
        {
            breakpointId = 0;
            return this.bridgeSession == null ? VSConstants.E_NOTIMPL : this.bridgeSession.CreateAddressBreakpoint(address, enabled, mode, out breakpointId);
        }

        internal int CreateSourceBreakpoint(string sourcePath, uint line, uint column, out ulong breakpointId)
        {
            breakpointId = 0;
            return this.bridgeSession == null ? VSConstants.E_NOTIMPL : this.bridgeSession.CreateSourceBreakpoint(sourcePath, line, column, out breakpointId);
        }

        internal int SetBreakpointEnabled(ulong breakpointId, bool enabled)
        {
            return this.bridgeSession == null ? VSConstants.E_NOTIMPL : this.bridgeSession.SetBreakpointEnabled(breakpointId, enabled);
        }

        internal int DeleteBreakpoint(ulong breakpointId)
        {
            return this.bridgeSession == null ? VSConstants.E_NOTIMPL : this.bridgeSession.DeleteBreakpoint(breakpointId);
        }

        internal void RegisterBoundBreakpoint(RadDbgBoundBreakpoint breakpoint)
        {
            lock (this.breakpointLock)
            {
                this.boundBreakpoints[breakpoint.Id] = breakpoint;
            }
        }

        internal void UnregisterBoundBreakpoint(ulong breakpointId)
        {
            lock (this.breakpointLock)
            {
                this.boundBreakpoints.Remove(breakpointId);
            }
        }

        internal IDebugBoundBreakpoint2? BoundBreakpointFromId(ulong breakpointId)
        {
            lock (this.breakpointLock)
            {
                return breakpointId != 0 && this.boundBreakpoints.TryGetValue(breakpointId, out RadDbgBoundBreakpoint? breakpoint) ? breakpoint : null;
            }
        }

        internal void SendBreakpointBound(RadDbgPendingBreakpoint pendingBreakpoint, RadDbgBoundBreakpoint boundBreakpoint)
        {
            this.SendEvent(this.program, new RadBreakpointBoundEvent(pendingBreakpoint, boundBreakpoint), typeof(IDebugBreakpointBoundEvent2).GUID, null);
        }

        internal void SendBreakpointError(RadDbgErrorBreakpoint errorBreakpoint)
        {
            this.SendEvent(this.program, new RadBreakpointErrorEvent(errorBreakpoint), typeof(IDebugBreakpointErrorEvent2).GUID, null);
        }

        internal int ContinueProgram(ulong threadHandle)
        {
            return this.bridgeSession == null ? VSConstants.E_NOTIMPL : this.bridgeSession.Continue(threadHandle);
        }

        internal int TerminateProgram(ulong processHandle = 0)
        {
            if (this.bridgeSession == null)
            {
                return VSConstants.E_NOTIMPL;
            }

            if (processHandle == 0)
            {
                int result = this.bridgeSession.GetProgramDescriptor(out processHandle, out _, out _, out _, out _, out _);
                if (result != VSConstants.S_OK)
                {
                    return result;
                }
            }
            return this.bridgeSession.Terminate(processHandle);
        }

        internal int GetTopFrame(ulong threadHandle, out RadDbgFrameInfo frame, out string sourcePath)
        {
            if (this.bridgeSession == null)
            {
                frame = default;
                sourcePath = string.Empty;
                return VSConstants.E_FAIL;
            }
            return this.bridgeSession.GetTopFrame(threadHandle, out frame, out sourcePath);
        }

        internal int GetThreads(RadDbgProgram program, out IDebugThread2[] threads)
        {
            if (this.bridgeSession == null)
            {
                threads = Array.Empty<IDebugThread2>();
                return VSConstants.E_FAIL;
            }
            return this.bridgeSession.GetThreadWrappers(program, out threads);
        }

        internal int ProgramCanDetach()
        {
            return this.bridgeSession == null ? VSConstants.E_NOTIMPL : this.bridgeSession.CanDetachProgram();
        }

        internal int AttachProgram(IDebugEventCallback2 callback)
        {
            this.SetCallback(callback);
            if (this.bridgeSession == null)
            {
                return VSConstants.S_OK;
            }

            int result = this.bridgeSession.ProgramAttach(callback);
            return result == VSConstants.E_NOTIMPL ? VSConstants.S_OK : result;
        }

        internal int DetachProgram()
        {
            return this.bridgeSession == null ? VSConstants.E_NOTIMPL : this.bridgeSession.DetachProgram();
        }

        internal int EnumCodeContexts(IDebugDocumentPosition2 documentPosition, out IEnumDebugCodeContexts2 contexts)
        {
            contexts = null!;
            return this.bridgeSession == null ? VSConstants.E_NOTIMPL : this.bridgeSession.EnumCodeContexts(documentPosition, out contexts);
        }

        internal int EnumCodePaths(string hint, IDebugCodeContext2 startContext, IDebugStackFrame2 stackFrame, int source, out IEnumCodePaths2 paths, out IDebugCodeContext2 safetyContext)
        {
            paths = null!;
            safetyContext = null!;
            return this.bridgeSession == null ? VSConstants.E_NOTIMPL : this.bridgeSession.EnumCodePaths(hint, startContext, stackFrame, source, out paths, out safetyContext);
        }

        internal int GetModules(RadDbgProgram program, out IDebugModule2[] modules)
        {
            modules = Array.Empty<IDebugModule2>();
            if (this.bridgeSession == null)
            {
                return VSConstants.E_NOTIMPL;
            }

            int result = this.bridgeSession.EnumModules(out RadDbgModuleDesc[] nativeModules);
            if (result == VSConstants.E_NOTIMPL)
            {
                result = this.bridgeSession.CopyModules(out nativeModules);
            }
            if (result != VSConstants.S_OK)
            {
                return result;
            }

            List<IDebugModule2> wrappers = new List<IDebugModule2>();
            foreach (RadDbgModuleDesc nativeModule in nativeModules)
            {
                if (program.ProcessHandle == 0 || nativeModule.ProcessHandle == program.ProcessHandle)
                {
                    wrappers.Add(new RadDbgModule(nativeModule));
                }
            }
            modules = wrappers.ToArray();
            return VSConstants.S_OK;
        }

        internal int ExecuteProgram()
        {
            return this.bridgeSession == null ? VSConstants.E_NOTIMPL : this.bridgeSession.ExecuteProgram();
        }

        internal int GetDebugProperty(out IDebugProperty2 property)
        {
            property = null!;
            return this.bridgeSession == null ? VSConstants.E_NOTIMPL : this.bridgeSession.GetDebugProperty(out property);
        }

        internal int GetDisassemblyStream(enum_DISASSEMBLY_STREAM_SCOPE scope, IDebugCodeContext2 codeContext, out IDebugDisassemblyStream2 stream)
        {
            stream = null!;
            return this.bridgeSession == null ? VSConstants.E_NOTIMPL : this.bridgeSession.GetDisassemblyStream(scope, codeContext, out stream);
        }

        internal int GetEncUpdate(out object update)
        {
            update = null!;
            return this.bridgeSession == null ? VSConstants.E_NOTIMPL : this.bridgeSession.GetEncUpdate(out update);
        }

        internal int GetMemoryBytes(ulong processHandle, out IDebugMemoryBytes2 memoryBytes)
        {
            memoryBytes = null!;
            if (this.bridgeSession == null)
            {
                return VSConstants.E_NOTIMPL;
            }

            int result = this.bridgeSession.GetNativeMemoryBytes(out memoryBytes);
            if (result == VSConstants.E_NOTIMPL)
            {
                memoryBytes = new RadDbgMemoryBytes(this, processHandle);
                return VSConstants.S_OK;
            }
            return result;
        }

        internal int GetProgramName(out string name)
        {
            name = string.Empty;
            if (this.bridgeSession == null)
            {
                return VSConstants.E_NOTIMPL;
            }

            int result = this.bridgeSession.GetProgramName(out name);
            return result == VSConstants.E_NOTIMPL ? this.bridgeSession.GetProgramDescriptor(out _, out _, out name, out _, out _, out _) : result;
        }

        internal int GetProgramEngineInfo(out string engineName, out Guid engineId)
        {
            engineName = string.Empty;
            engineId = Guid.Empty;
            if (this.bridgeSession == null)
            {
                return VSConstants.E_NOTIMPL;
            }

            int result = this.bridgeSession.GetProgramEngineInfo(out engineName, out engineId);
            if (result == VSConstants.E_NOTIMPL)
            {
                result = this.bridgeSession.GetProgramDescriptor(out _, out _, out _, out _, out engineName, out string engineIdText);
                if (result == VSConstants.S_OK && !Guid.TryParse(engineIdText, out engineId))
                {
                    return VSConstants.E_FAIL;
                }
            }
            return result;
        }

        internal int GetProgramId(out Guid programId)
        {
            programId = Guid.Empty;
            return this.bridgeSession == null ? VSConstants.E_NOTIMPL : this.bridgeSession.GetProgramId(out programId);
        }

        internal int ResolveSourcePosition(string sourcePath, uint line, uint column)
        {
            return this.bridgeSession == null ? VSConstants.E_NOTIMPL : this.bridgeSession.ResolveSourcePosition(sourcePath, line, column);
        }

        internal int StepProgram(ulong threadHandle, enum_STEPKIND stepKind, enum_STEPUNIT stepUnit)
        {
            return this.bridgeSession == null ? VSConstants.E_NOTIMPL : this.bridgeSession.ProgramStep(threadHandle, (uint)stepKind, (uint)stepUnit);
        }

        internal int TerminateProgramViaProgram()
        {
            return this.bridgeSession == null ? VSConstants.E_NOTIMPL : this.bridgeSession.TerminateProgram();
        }

        internal int WriteDump(enum_DUMPTYPE dumpType, string dumpUrl)
        {
            return this.bridgeSession == null ? VSConstants.E_NOTIMPL : this.bridgeSession.WriteDump((uint)dumpType, dumpUrl);
        }

        internal int EnumFrameInfo(ulong threadHandle, enum_FRAMEINFO_FLAGS fieldSpec, uint radix, out IEnumDebugFrameInfo2 frames)
        {
            frames = null!;
            return this.bridgeSession == null ? VSConstants.E_NOTIMPL : this.bridgeSession.EnumFrameInfo(threadHandle, (uint)fieldSpec, radix, out frames);
        }

        internal int GetThreadName(ulong threadHandle, out string name)
        {
            name = string.Empty;
            return this.bridgeSession == null ? VSConstants.E_NOTIMPL : this.bridgeSession.GetThreadName(threadHandle, out name);
        }

        internal int CanSetNextStatement(ulong threadHandle, IDebugStackFrame2 stackFrame, IDebugCodeContext2 codeContext)
        {
            return this.bridgeSession == null ? VSConstants.E_NOTIMPL : this.bridgeSession.CanSetNextStatement(threadHandle, stackFrame, codeContext);
        }

        internal int SetNextStatement(ulong threadHandle, IDebugStackFrame2 stackFrame, IDebugCodeContext2 codeContext)
        {
            return this.bridgeSession == null ? VSConstants.E_NOTIMPL : this.bridgeSession.SetNextStatement(threadHandle, stackFrame, codeContext);
        }

        internal int GetNativeThreadId(ulong threadHandle, out uint threadId)
        {
            threadId = 0;
            return this.bridgeSession == null ? VSConstants.E_NOTIMPL : this.bridgeSession.GetThreadId(threadHandle, out threadId);
        }

        internal int GetThreadProgram(ulong threadHandle, out IDebugProgram2 program)
        {
            program = null!;
            return this.bridgeSession == null ? VSConstants.E_NOTIMPL : this.bridgeSession.GetThreadProgram(threadHandle, out program);
        }

        internal int GetThreadProperties(ulong threadHandle, enum_THREADPROPERTY_FIELDS fields, ref THREADPROPERTIES properties)
        {
            return this.bridgeSession == null ? VSConstants.E_NOTIMPL : this.bridgeSession.GetThreadProperties(threadHandle, (uint)fields, ref properties);
        }

        internal int GetLogicalThread(ulong threadHandle, IDebugStackFrame2 stackFrame, out IDebugLogicalThread2 logicalThread)
        {
            logicalThread = null!;
            return this.bridgeSession == null ? VSConstants.E_NOTIMPL : this.bridgeSession.GetLogicalThread(threadHandle, stackFrame, out logicalThread);
        }

        internal int ResumeThread(ulong threadHandle, out uint suspendCount)
        {
            suspendCount = 0;
            return this.bridgeSession == null ? VSConstants.E_NOTIMPL : this.bridgeSession.ResumeThread(threadHandle, out suspendCount);
        }

        internal int SetThreadName(ulong threadHandle, string name)
        {
            return this.bridgeSession == null ? VSConstants.E_NOTIMPL : this.bridgeSession.SetThreadName(threadHandle, name);
        }

        internal int SuspendThread(ulong threadHandle, out uint suspendCount)
        {
            suspendCount = 0;
            return this.bridgeSession == null ? VSConstants.E_NOTIMPL : this.bridgeSession.SuspendThread(threadHandle, out suspendCount);
        }

        internal int SetInstructionPointer(ulong threadHandle, ulong address)
        {
            return this.bridgeSession == null ? VSConstants.E_NOTIMPL : this.bridgeSession.SetInstructionPointer(threadHandle, address);
        }

        internal int GetMemorySize(ulong processHandle, out ulong size)
        {
            size = 0;
            return this.bridgeSession == null ? VSConstants.E_NOTIMPL : this.bridgeSession.GetMemorySize(processHandle, out size);
        }

        internal int ReadMemoryAt(ulong processHandle, ulong address, uint byteCount, byte[] buffer, out uint read, out uint unreadable)
        {
            read = 0;
            unreadable = 0;
            return this.bridgeSession == null ? VSConstants.E_NOTIMPL : this.bridgeSession.ReadMemoryAt(processHandle, address, byteCount, buffer, out read, out unreadable);
        }

        internal int WriteMemoryAt(ulong processHandle, ulong address, uint byteCount, byte[] buffer)
        {
            return this.bridgeSession == null ? VSConstants.E_NOTIMPL : this.bridgeSession.WriteMemoryAt(processHandle, address, byteCount, buffer);
        }

        internal int StepThread(ulong threadHandle, enum_STEPKIND stepKind, enum_STEPUNIT stepUnit)
        {
            return this.bridgeSession == null ? VSConstants.E_NOTIMPL : this.bridgeSession.Step(threadHandle, (uint)stepKind, (uint)stepUnit);
        }

        internal int GetProgramDescriptor(out ulong processHandle, out uint processId, out string programName, out string hostName, out string engineName, out string engineId)
        {
            if (this.bridgeSession == null)
            {
                processHandle = 0;
                processId = 0;
                programName = string.Empty;
                hostName = string.Empty;
                engineName = string.Empty;
                engineId = string.Empty;
                return VSConstants.E_FAIL;
            }
            return this.bridgeSession.GetProgramDescriptor(out processHandle, out processId, out programName, out hostName, out engineName, out engineId);
        }

        internal int GetProgramDescriptor(ulong processHandle, out uint processId, out string programName, out string hostName, out string engineName, out string engineId)
        {
            if (this.bridgeSession == null)
            {
                processId = 0;
                programName = string.Empty;
                hostName = string.Empty;
                engineName = string.Empty;
                engineId = string.Empty;
                return VSConstants.E_FAIL;
            }
            return this.bridgeSession.GetProgramDescriptor(processHandle, out processId, out programName, out hostName, out engineName, out engineId);
        }

        internal void SendEvent(IDebugProgram2? program, IDebugEvent2 @event, Guid eventGuid, IDebugThread2? thread)
        {
            if (this.callback == null)
            {
                return;
            }

            @event.GetAttributes(out uint attributes);
            this.callback.Event(this, null!, program!, thread!, @event, ref eventGuid, attributes);
        }

        private void DisposeBridgeSession()
        {
            this.bridgeSession?.Dispose();
            this.bridgeSession = null;
            this.launchProcess = null;
            this.launchSystemProcessId = 0;
            lock (this.programLock)
            {
                this.programs.Clear();
                this.programNodes.Clear();
            }
            lock (this.breakpointLock)
            {
                this.boundBreakpoints.Clear();
            }
        }
    }
}
