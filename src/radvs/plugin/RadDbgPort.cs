using RAD;

namespace RadShim
{
    internal sealed class RadDbgPort : IDebugDefaultPort2
    {
        private readonly IDebugPortNotify2 _portNotify;
        public int GetPortName(out string pbstrName)
        {
            pbstrName = "RadDbgPort";
        }

        public int GetPortId(out Guid pguidPort)
        {
            throw new NotImplementedException();
        }

        public int GetPortRequest(out IDebugPortRequest2 ppRequest)
        {
            throw new NotImplementedException();
        }

        public int GetPortSupplier(out IDebugPortSupplier2 ppSupplier)
        {
            throw new NotImplementedException();
        }

        public int GetProcess(AD_PROCESS_ID ProcessId, out IDebugProcess2 ppProcess)
        {
            return RadDbgBridge.GetProcess(ProcessId, out ppProcess);
        }

        public int EnumProcesses(out IEnumDebugProcesses2 ppEnum)
        {
            ppEnum = null;

            int result = this.bridge.EnumProcesses(out RaddbgProcessDesc[] descs);
            if (result != VSConstants.S_OK)
            {
                return result;
            }

            IDebugProcess2[] processes = new IDebugProcess2[descs.Length];
            for (int i = 0; i < descs.Length; i++)
            {
                processes[i] = new RadDbgProcess(this, descs[i]);
            }

            ppEnum = new RadDbgProcessEnumerator(processes);
            return VSConstants.S_OK;
        }
    }

    class RadDbgPortRequest : IDebugPortRequest2
    {
        public int GetPortName(out string pbstrPortName)
        {
            throw new NotImplementedException();
        }
    }

}
