using System;
using System.Runtime.InteropServices;
using Microsoft.VisualStudio;
using Microsoft.VisualStudio.Debugger.Interop;

namespace RAD
{
    [ComVisible(true)]
    [ClassInterface(ClassInterfaceType.None)]
    internal sealed class RadDbgProgramNode : IDebugProgramNode2
    {
        private readonly RadDbgEngine engine;
        private readonly ulong processHandle;

        internal RadDbgProgramNode(RadDbgEngine engine, ulong processHandle = 0)
        {
            this.engine = engine;
            this.processHandle = processHandle;
        }

        public int GetProgramName(out string pbstrProgramName)
        {
            return this.engine.GetProgramDescriptor(this.processHandle, out _, out pbstrProgramName, out _, out _, out _);
        }

        public int GetHostName(enum_GETHOSTNAME_TYPE dwHostNameType, out string pbstrHostName)
        {
            return this.engine.GetProgramDescriptor(this.processHandle, out _, out _, out pbstrHostName, out _, out _);
        }

        public int GetHostPid(AD_PROCESS_ID[] pHostProcessId)
        {
            if (pHostProcessId == null || pHostProcessId.Length == 0)
            {
                return VSConstants.E_NOTIMPL;
            }

            int result = this.engine.GetProgramDescriptor(this.processHandle, out uint processId, out _, out _, out _, out _);
            if (result != VSConstants.S_OK)
            {
                return result;
            }

            pHostProcessId[0] = new AD_PROCESS_ID
            {
                ProcessIdType = (uint)enum_AD_PROCESS_ID.AD_PROCESS_ID_SYSTEM,
                dwProcessId = processId,
            };
            return 0;
        }

        public int GetHostMachineName_V7(out string pbstrHostMachineName)
        {
            return this.engine.GetProgramDescriptor(this.processHandle, out _, out _, out pbstrHostMachineName, out _, out _);
        }

        public int Attach_V7(IDebugProgram2 pMDMProgram, IDebugEventCallback2 pCallback, uint dwReason)
        {
            return VSConstants.E_NOTIMPL;
        }

        public int GetEngineInfo(out string pbstrEngine, out Guid pguidEngine)
        {
            int result = this.engine.GetProgramDescriptor(this.processHandle, out _, out _, out _, out pbstrEngine, out string engineId);
            if (result != VSConstants.S_OK || !Guid.TryParse(engineId, out pguidEngine))
            {
                pguidEngine = Guid.Empty;
                return VSConstants.E_FAIL;
            }
            return VSConstants.S_OK;
        }

        public int DetachDebugger_V7()
        {
            return VSConstants.E_NOTIMPL;
        }

    }
}
