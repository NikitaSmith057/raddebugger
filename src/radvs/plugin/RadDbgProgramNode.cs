using System;
using System.Runtime.InteropServices;
using Microsoft.VisualStudio.Debugger.Interop;

namespace RAD
{
    [ComVisible(true)]
    [ClassInterface(ClassInterfaceType.None)]
    internal sealed class RadDbgProgramNode : IDebugProgramNode2
    {
        private readonly RadDbgEngine engine;

        internal RadDbgProgramNode(RadDbgEngine engine)
        {
            this.engine = engine;
        }

        public int GetProgramName(out string pbstrProgramName)
        {
            return this.engine.GetProgramName(out pbstrProgramName);
        }

        public int GetHostName(enum_GETHOSTNAME_TYPE dwHostNameType, out string pbstrHostName)
        {
            return this.engine.GetHostName(dwHostNameType, out pbstrHostName);
        }

        public int GetHostPid(AD_PROCESS_ID[] pHostProcessId)
        {
            if (pHostProcessId == null || pHostProcessId.Length == 0)
            {
                return RadDbgHResult.E_POINTER;
            }
            int result = this.engine.GetHostPid(out AD_PROCESS_ID processId);
            if (result >= 0)
            {
                pHostProcessId[0] = processId;
            }
            return result;
        }

        public int GetHostMachineName_V7(out string pbstrHostMachineName)
        {
            return this.engine.GetHostMachineName(out pbstrHostMachineName);
        }

        public int Attach_V7(IDebugProgram2 pMDMProgram, IDebugEventCallback2 pCallback, uint dwReason)
        {
            return RadDbgHResult.E_NOTIMPL;
        }

        public int GetEngineInfo(out string pbstrEngine, out Guid pguidEngine)
        {
            return this.engine.GetEngineInfo(out pbstrEngine, out pguidEngine);
        }

        public int DetachDebugger_V7()
        {
            return RadDbgHResult.E_NOTIMPL;
        }
    }
}
