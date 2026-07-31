using System;
using Microsoft.VisualStudio;
using Microsoft.VisualStudio.Debugger.Interop;

namespace RAD
{
    internal sealed class RadDbgProgram : IDebugProgram2
    {
        private readonly RadDbgEngine engine;
        private readonly IDebugProcess2 process;
        private readonly Guid programId;
        private ulong processHandle;
        private bool registered;

        internal RadDbgProgram(RadDbgEngine engine, IDebugProcess2 process, Guid programId, ulong processHandle = 0)
        {
            this.engine = engine;
            this.process = process;
            this.programId = programId;
            this.processHandle = processHandle;
        }

        internal ulong ProcessHandle => this.processHandle;
        internal bool IsRegistered => this.registered;
        internal RadDbgEngine Engine => this.engine;

        internal void BindNativeProcess(ulong processHandle)
        {
            this.processHandle = processHandle;
        }

        internal void MarkRegistered()
        {
            this.registered = true;
        }

        internal bool TryGetTopFrame(RadDbgThread thread, out RadDbgFrameInfo frame, out string sourcePath)
        {
            frame = default;
            sourcePath = string.Empty;
            return this.engine.GetTopFrame(thread.Handle, out frame, out sourcePath) == VSConstants.S_OK;
        }

        public int CanDetach()
        {
            return this.engine.ProgramCanDetach();
        }

        public int CauseBreak()
        {
            return this.engine.BreakProgram();
        }

        public int Continue(IDebugThread2 pThread)
        {
            return pThread is RadDbgThread thread ? this.engine.ContinueProgram(thread.Handle) : VSConstants.E_INVALIDARG;
        }

        public int Detach()
        {
            return this.engine.DetachProgram();
        }

        public int EnumCodeContexts(IDebugDocumentPosition2 pDocPos, out IEnumDebugCodeContexts2 ppEnum)
        {
            return this.engine.EnumCodeContexts(pDocPos, out ppEnum);
        }

        public int EnumCodePaths(string pszHint, IDebugCodeContext2 pStart, IDebugStackFrame2 pFrame, int fSource, out IEnumCodePaths2 ppEnum, out IDebugCodeContext2 ppSafety)
        {
            return this.engine.EnumCodePaths(pszHint, pStart, pFrame, fSource, out ppEnum, out ppSafety);
        }

        public int EnumModules(out IEnumDebugModules2 ppEnum)
        {
            ppEnum = null!;
            int result = this.engine.GetModules(this, out IDebugModule2[] modules);
            if (result != VSConstants.S_OK)
            {
                return result;
            }
            ppEnum = new RadDbgModuleEnum(modules);
            return VSConstants.S_OK;
        }

        public int EnumThreads(out IEnumDebugThreads2 ppEnum)
        {
            ppEnum = null!;
            int result = this.engine.GetThreads(this, out IDebugThread2[] wrappers);
            if (result != VSConstants.S_OK)
            {
                return VSConstants.E_FAIL;
            }
            ppEnum = new RadDbgThreadEnum(wrappers);
            return VSConstants.S_OK;
        }

        public int Execute()
        {
            int result = this.engine.ExecuteProgram();
            return result == VSConstants.E_NOTIMPL ? this.engine.ContinueProgram(0) : result;
        }

        public int GetDebugProperty(out IDebugProperty2 ppProperty)
        {
            return this.engine.GetDebugProperty(out ppProperty);
        }

        public int GetDisassemblyStream(enum_DISASSEMBLY_STREAM_SCOPE dwScope, IDebugCodeContext2 pCodeContext, out IDebugDisassemblyStream2 ppDisassemblyStream)
        {
            return this.engine.GetDisassemblyStream(dwScope, pCodeContext, out ppDisassemblyStream);
        }

        public int GetENCUpdate(out object pUpdate)
        {
            return this.engine.GetEncUpdate(out pUpdate);
        }

        public int GetEngineInfo(out string pbstrEngine, out Guid pguidEngine)
        {
            int engineInfoResult = this.engine.GetProgramEngineInfo(out pbstrEngine, out pguidEngine);
            if (engineInfoResult != VSConstants.E_NOTIMPL)
            {
                return engineInfoResult;
            }

            int result = this.engine.GetProgramDescriptor(this.processHandle, out _, out _, out _, out pbstrEngine, out string engineId);
            if (result != VSConstants.S_OK || !Guid.TryParse(engineId, out pguidEngine))
            {
                pguidEngine = Guid.Empty;
                return VSConstants.E_FAIL;
            }
            return VSConstants.S_OK;
        }

        public int GetMemoryBytes(out IDebugMemoryBytes2 ppMemoryBytes)
        {
            return this.engine.GetMemoryBytes(this.processHandle, out ppMemoryBytes);
        }

        public int GetName(out string pbstrName)
        {
            int result = this.engine.GetProgramName(out pbstrName);
            return result == VSConstants.E_NOTIMPL ? this.engine.GetProgramDescriptor(this.processHandle, out _, out pbstrName, out _, out _, out _) : result;
        }

        public int GetProcess(out IDebugProcess2 ppProcess)
        {
            ppProcess = this.process;
            return 0;
        }

        public int GetProgramId(out Guid pguidProgramId)
        {
            int result = this.engine.GetProgramId(out pguidProgramId);
            if (result != VSConstants.E_NOTIMPL)
            {
                return result;
            }

            pguidProgramId = this.programId;
            return 0;
        }

        public int Step(IDebugThread2 pThread, enum_STEPKIND sk, enum_STEPUNIT step)
        {
            if (pThread is not RadDbgThread thread)
            {
                return VSConstants.E_INVALIDARG;
            }

            int result = this.engine.StepProgram(thread.Handle, sk, step);
            return result == VSConstants.E_NOTIMPL ? this.engine.StepThread(thread.Handle, sk, step) : result;
        }

        public int Terminate()
        {
            int result = this.engine.TerminateProgramViaProgram();
            return result == VSConstants.E_NOTIMPL ? this.engine.TerminateProgram(this.processHandle) : result;
        }

        public int Attach(IDebugEventCallback2 pCallback)
        {
            return this.engine.AttachProgram(pCallback);
        }

        public int WriteDump(enum_DUMPTYPE dumptype, string pszDumpUrl)
        {
            return this.engine.WriteDump(dumptype, pszDumpUrl);
        }
    }
}
