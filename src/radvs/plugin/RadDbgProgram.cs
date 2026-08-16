using System;
using Microsoft.VisualStudio.Debugger.Interop;

namespace RAD
{
    internal sealed class RadDbgProgram : IDebugProgram2
    {
        private readonly RadDbgEngine   engine;
        private readonly IDebugProcess2 process;
        private readonly Guid           programId;

        internal RadDbgProgram(RadDbgEngine engine, IDebugProcess2 process, Guid programId)
        {
            this.engine    = engine;
            this.process   = process;
            this.programId = programId;
        }

        public int EnumThreads(out IEnumDebugThreads2 ppEnum)
        {
            int result = this.engine.CopyThreads(out THREADPROPERTIES[] nativeThreads);
            if (result < 0)
            {
                ppEnum = null!;
                return result;
            }

            IDebugThread2[] threads = new IDebugThread2[nativeThreads.Length];
            for (int index = 0; index < nativeThreads.Length; ++index)
            {
                threads[index] = new RadDbgThread(this.engine, this, nativeThreads[index].dwThreadId);
            }

            ppEnum = new RadDbgThreadEnumerator(threads);
            return RadDbgHResult.S_OK;
        }

        public int GetName(out string pbstrName)
        {
            return this.engine.GetProgramName(out pbstrName);
        }

        public int GetProcess(out IDebugProcess2 ppProcess)
        {
            ppProcess = this.process;
            return RadDbgHResult.S_OK;
        }

        public int Terminate()
        {
            return this.engine.TerminateSession();
        }

        public int Attach(IDebugEventCallback2 pCallback)
        {
            return RadDbgHResult.S_OK;
        }

        public int CanDetach()
        {
            return RadDbgHResult.E_NOTIMPL;
        }

        public int Detach()
        {
            return RadDbgHResult.E_NOTIMPL;
        }

        public int GetProgramId(out Guid pguidProgramId)
        {
            pguidProgramId = this.programId;
            return RadDbgHResult.S_OK;
        }

        public int GetDebugProperty(out IDebugProperty2 ppProperty)
        {
            ppProperty = null!;
            return RadDbgHResult.E_NOTIMPL;
        }

        public int Execute()
        {
            return this.engine.RunSession();
        }

        public int Continue(IDebugThread2 pThread)
        {
            return this.engine.RunSession();
        }

        public int Step(IDebugThread2 pThread, enum_STEPKIND sk, enum_STEPUNIT Step)
        {
            return RadDbgHResult.E_NOTIMPL;
        }

        public int CauseBreak()
        {
            return this.engine.BreakSession();
        }

        public int GetEngineInfo(out string pbstrEngine, out Guid pguidEngine)
        {
            return this.engine.GetEngineInfo(out pbstrEngine, out pguidEngine);
        }

        public int EnumCodeContexts(IDebugDocumentPosition2 pDocPos, out IEnumDebugCodeContexts2 ppEnum)
        {
            ppEnum = null!;
            return RadDbgHResult.E_NOTIMPL;
        }

        public int GetMemoryBytes(out IDebugMemoryBytes2 ppMemoryBytes)
        {
            ppMemoryBytes = null!;
            return RadDbgHResult.E_NOTIMPL;
        }

        public int GetDisassemblyStream(
            enum_DISASSEMBLY_STREAM_SCOPE dwScope,
            IDebugCodeContext2 pCodeContext,
            out IDebugDisassemblyStream2 ppDisassemblyStream)
        {
            ppDisassemblyStream = null!;
            return RadDbgHResult.E_NOTIMPL;
        }

        public int EnumModules(out IEnumDebugModules2 ppEnum)
        {
            ppEnum = null!;
            return RadDbgHResult.E_NOTIMPL;
        }

        public int GetENCUpdate(out object ppUpdate)
        {
            ppUpdate = null!;
            return RadDbgHResult.E_NOTIMPL;
        }

        public int EnumCodePaths(
            string pszHint,
            IDebugCodeContext2 pStart,
            IDebugStackFrame2 pFrame,
            int fSource,
            out IEnumCodePaths2 ppEnum,
            out IDebugCodeContext2 ppSafety)
        {
            ppEnum = null!;
            ppSafety = null!;
            return RadDbgHResult.E_NOTIMPL;
        }

        public int WriteDump(enum_DUMPTYPE DUMPTYPE, string pszDumpUrl)
        {
            return RadDbgHResult.E_NOTIMPL;
        }
    }
}
