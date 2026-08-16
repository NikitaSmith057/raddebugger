using Microsoft.VisualStudio.Debugger.Interop;

namespace RAD
{
    internal sealed class RadDbgThread : IDebugThread2
    {
        private readonly RadDbgEngine  engine;
        private readonly RadDbgProgram program;
        private readonly uint          threadId;

        internal RadDbgThread(RadDbgEngine engine, RadDbgProgram program, uint threadId)
        {
            this.engine = engine;
            this.program = program;
            this.threadId = threadId;
        }

        public int EnumFrameInfo(enum_FRAMEINFO_FLAGS dwFieldSpec, uint nRadix, out IEnumDebugFrameInfo2 ppEnum)
        {
            ppEnum = null!;
            return RadDbgHResult.E_NOTIMPL;
        }

        public int GetName(out string pbstrName)
        {
            THREADPROPERTIES[] properties = new THREADPROPERTIES[1];
            int result = this.engine.GetThreadProperties(
                this.threadId,
                enum_THREADPROPERTY_FIELDS.TPF_NAME,
                properties);
            pbstrName = result >= 0 ? properties[0].bstrName ?? string.Empty : string.Empty;
            return result;
        }

        public int SetThreadName(string pszName)
        {
            return RadDbgHResult.E_NOTIMPL;
        }

        public int GetProgram(out IDebugProgram2 ppProgram)
        {
            ppProgram = this.program;
            return RadDbgHResult.S_OK;
        }

        public int CanSetNextStatement(IDebugStackFrame2 pStackFrame, IDebugCodeContext2 pCodeContext)
        {
            return RadDbgHResult.E_NOTIMPL;
        }

        public int SetNextStatement(IDebugStackFrame2 pStackFrame, IDebugCodeContext2 pCodeContext)
        {
            return RadDbgHResult.E_NOTIMPL;
        }

        public int GetThreadId(out uint pdwThreadId)
        {
            pdwThreadId = this.threadId;
            return RadDbgHResult.S_OK;
        }

        public int Suspend(out uint pdwSuspendCount)
        {
            pdwSuspendCount = 0;
            return RadDbgHResult.E_NOTIMPL;
        }

        public int Resume(out uint pdwSuspendCount)
        {
            pdwSuspendCount = 0;
            return RadDbgHResult.E_NOTIMPL;
        }

        public int GetThreadProperties(enum_THREADPROPERTY_FIELDS dwFields, THREADPROPERTIES[] ptp)
        {
            if (ptp == null || ptp.Length == 0)
            {
                return RadDbgHResult.E_POINTER;
            }
            return this.engine.GetThreadProperties(this.threadId, dwFields, ptp);
        }

        public int GetLogicalThread(IDebugStackFrame2 pStackFrame, out IDebugLogicalThread2 ppLogicalThread)
        {
            ppLogicalThread = null!;
            return RadDbgHResult.E_NOTIMPL;
        }
    }
}
