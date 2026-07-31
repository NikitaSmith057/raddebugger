using Microsoft.VisualStudio;
using Microsoft.VisualStudio.Debugger.Interop;

namespace RAD
{
    internal sealed class RadDbgThread : IDebugThread2
    {
        private readonly RadDbgProgram program;
        private readonly uint          threadId;
        private readonly ulong         handle;
        private string                 name;

        internal RadDbgThread(RadDbgProgram program, uint threadId, ulong handle, string name)
        {
            this.program  = program;
            this.threadId = threadId;
            this.handle = handle;
            this.name = name;
        }

        internal ulong Handle => this.handle;

        public int EnumFrameInfo(enum_FRAMEINFO_FLAGS dwFieldSpec, uint nRadix, out IEnumDebugFrameInfo2 ppEnum)
        {
            int nativeResult = this.program.Engine.EnumFrameInfo(this.handle, dwFieldSpec, nRadix, out ppEnum);
            if (nativeResult != VSConstants.E_NOTIMPL)
            {
                return nativeResult;
            }

            if (!this.program.TryGetTopFrame(this, out RadDbgFrameInfo frame, out string sourcePath))
            {
                return VSConstants.E_FAIL;
            }

            RadDbgStackFrame stackFrame = new RadDbgStackFrame(this, frame, sourcePath);
            stackFrame.GetFrameInfo(dwFieldSpec, out FRAMEINFO frameInfo);
            ppEnum = new RadDbgFrameInfoEnum([frameInfo]);
            return VSConstants.S_OK;
        }

        public int GetName(out string pbstrName)
        {
            int result = this.program.Engine.GetThreadName(this.handle, out pbstrName);
            if (result == VSConstants.S_OK)
            {
                this.name = pbstrName;
                return VSConstants.S_OK;
            }
            if (result != VSConstants.E_NOTIMPL)
            {
                return result;
            }

            pbstrName = this.name;
            return VSConstants.S_OK;
        }

        public int SetThreadName(string pszName)
        {
            int result = this.program.Engine.SetThreadName(this.handle, pszName);
            if (result == VSConstants.S_OK)
            {
                this.name = pszName;
            }
            return result;
        }

        public int GetProgram(out IDebugProgram2 ppProgram)
        {
            int result = this.program.Engine.GetThreadProgram(this.handle, out ppProgram);
            if (result != VSConstants.E_NOTIMPL)
            {
                return result;
            }

            ppProgram = this.program;
            return 0;
        }

        public int CanSetNextStatement(IDebugStackFrame2 pStackFrame, IDebugCodeContext2 pCodeContext)
        {
            return this.program.Engine.CanSetNextStatement(this.handle, pStackFrame, pCodeContext);
        }

        public int SetNextStatement(IDebugStackFrame2 pStackFrame, IDebugCodeContext2 pCodeContext)
        {
            return this.program.Engine.SetNextStatement(this.handle, pStackFrame, pCodeContext);
        }

        public int GetThreadId(out uint pdwThreadId)
        {
            int result = this.program.Engine.GetNativeThreadId(this.handle, out pdwThreadId);
            if (result != VSConstants.E_NOTIMPL)
            {
                return result;
            }

            pdwThreadId = this.threadId;
            return 0;
        }

        public int Suspend(out uint pdwSuspendCount)
        {
            return this.program.Engine.SuspendThread(this.handle, out pdwSuspendCount);
        }

        public int Resume(out uint pdwSuspendCount)
        {
            return this.program.Engine.ResumeThread(this.handle, out pdwSuspendCount);
        }

        public int GetThreadProperties(enum_THREADPROPERTY_FIELDS dwFields, THREADPROPERTIES[] ptp)
        {
            if (ptp == null || ptp.Length == 0)
            {
                return VSConstants.E_INVALIDARG;
            }

            THREADPROPERTIES properties = new THREADPROPERTIES();
            int result = this.program.Engine.GetThreadProperties(this.handle, dwFields, ref properties);
            if (result != VSConstants.E_NOTIMPL)
            {
                ptp[0] = properties;
                return result;
            }

            if ((dwFields & enum_THREADPROPERTY_FIELDS.TPF_ID) != 0)
            {
                properties.dwThreadId = this.threadId;
                properties.dwFields |= enum_THREADPROPERTY_FIELDS.TPF_ID;
            }
            if ((dwFields & enum_THREADPROPERTY_FIELDS.TPF_SUSPENDCOUNT) != 0)
            {
                properties.dwSuspendCount = 0;
                properties.dwFields |= enum_THREADPROPERTY_FIELDS.TPF_SUSPENDCOUNT;
            }
            if ((dwFields & enum_THREADPROPERTY_FIELDS.TPF_STATE) != 0)
            {
                properties.dwThreadState = (uint)enum_THREADSTATE.THREADSTATE_RUNNING;
                properties.dwFields |= enum_THREADPROPERTY_FIELDS.TPF_STATE;
            }
            if ((dwFields & enum_THREADPROPERTY_FIELDS.TPF_PRIORITY) != 0)
            {
                properties.bstrPriority = "Normal";
                properties.dwFields |= enum_THREADPROPERTY_FIELDS.TPF_PRIORITY;
            }
            if ((dwFields & enum_THREADPROPERTY_FIELDS.TPF_NAME) != 0)
            {
                properties.bstrName = this.name;
                properties.dwFields |= enum_THREADPROPERTY_FIELDS.TPF_NAME;
            }
            if ((dwFields & enum_THREADPROPERTY_FIELDS.TPF_LOCATION) != 0)
            {
                properties.bstrLocation = this.program.TryGetTopFrame(this, out RadDbgFrameInfo frame, out _) ? $"0x{frame.InstructionPointer:X}" : string.Empty;
                properties.dwFields |= enum_THREADPROPERTY_FIELDS.TPF_LOCATION;
            }
            ptp[0] = properties;
            return VSConstants.S_OK;
        }

        public int GetLogicalThread(IDebugStackFrame2 pStackFrame, out IDebugLogicalThread2 ppLogicalThread)
        {
            return this.program.Engine.GetLogicalThread(this.handle, pStackFrame, out ppLogicalThread);
        }
    }
}
