using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using Microsoft.VisualStudio;
using Microsoft.VisualStudio.Debugger.Interop;

namespace RAD
{
    internal sealed class RadDbgPendingBreakpoint : IDebugPendingBreakpoint2
    {
        private const string UnsupportedBreakpoint = "RAD does not support this breakpoint location.";
        private const string UnsupportedCondition = "RAD does not support conditional breakpoints yet.";
        private const string UnsupportedPassCount = "RAD does not support pass-count breakpoints yet.";

        private readonly RadDbgEngine engine;
        private readonly IDebugBreakpointRequest2 request;
        private BP_REQUEST_INFO requestInfo;
        private readonly List<RadDbgBoundBreakpoint> boundBreakpoints = new List<RadDbgBoundBreakpoint>();
        private RadDbgErrorBreakpoint? errorBreakpoint;
        private bool enabled = true;
        private bool deleted;

        private RadDbgPendingBreakpoint(RadDbgEngine engine, IDebugBreakpointRequest2 request, BP_REQUEST_INFO requestInfo)
        {
            this.engine = engine;
            this.request = request;
            this.requestInfo = requestInfo;
        }

        internal static int Create(RadDbgEngine engine, IDebugBreakpointRequest2 request, out RadDbgPendingBreakpoint? pendingBreakpoint)
        {
            pendingBreakpoint = null;
            if (request == null)
            {
                return VSConstants.E_INVALIDARG;
            }

            BP_REQUEST_INFO[] requestInfo = new BP_REQUEST_INFO[1];
            int result = request.GetRequestInfo(
                enum_BPREQI_FIELDS.BPREQI_BPLOCATION |
                enum_BPREQI_FIELDS.BPREQI_CONDITION |
                enum_BPREQI_FIELDS.BPREQI_PASSCOUNT,
                requestInfo);
            if (result < 0)
            {
                return result;
            }

            pendingBreakpoint = new RadDbgPendingBreakpoint(engine, request, requestInfo[0]);
            return VSConstants.S_OK;
        }

        private void SetError(string message, enum_BP_ERROR_TYPE errorType = enum_BP_ERROR_TYPE.BPET_GENERAL_ERROR, bool sendEvent = false)
        {
            this.errorBreakpoint = new RadDbgErrorBreakpoint(this, message, errorType);
            if (sendEvent)
            {
                this.engine.SendBreakpointError(this.errorBreakpoint);
            }
        }

        private bool VerifyCondition(BP_CONDITION condition)
        {
            return condition.styleCondition == enum_BP_COND_STYLE.BP_COND_NONE;
        }

        private bool VerifyPassCount(BP_PASSCOUNT passCount)
        {
            return passCount.stylePassCount == enum_BP_PASSCOUNT_STYLE.BP_PASSCOUNT_NONE;
        }

        private bool CanBindCore()
        {
            if (this.deleted)
            {
                this.SetError(UnsupportedBreakpoint);
                return false;
            }

            if ((this.requestInfo.dwFields & enum_BPREQI_FIELDS.BPREQI_BPLOCATION) == 0)
            {
                this.SetError(UnsupportedBreakpoint);
                return false;
            }

            enum_BP_LOCATION_TYPE locationType = (enum_BP_LOCATION_TYPE)this.requestInfo.bpLocation.bpLocationType;
            if (locationType != enum_BP_LOCATION_TYPE.BPLT_CODE_FILE_LINE &&
                locationType != enum_BP_LOCATION_TYPE.BPLT_CODE_CONTEXT)
            {
                this.SetError(UnsupportedBreakpoint);
                return false;
            }

            if ((this.requestInfo.dwFields & enum_BPREQI_FIELDS.BPREQI_CONDITION) != 0 &&
                !this.VerifyCondition(this.requestInfo.bpCondition))
            {
                this.SetError(UnsupportedCondition);
                return false;
            }

            if ((this.requestInfo.dwFields & enum_BPREQI_FIELDS.BPREQI_PASSCOUNT) != 0 &&
                !this.VerifyPassCount(this.requestInfo.bpPassCount))
            {
                this.SetError(UnsupportedPassCount);
                return false;
            }

            return true;
        }

        private int TryCreateNativeBreakpoint(out ulong breakpointId, out RadDbgBreakpointResolution? resolution)
        {
            breakpointId = 0;
            resolution = null;

            enum_BP_LOCATION_TYPE locationType = (enum_BP_LOCATION_TYPE)this.requestInfo.bpLocation.bpLocationType;
            if (locationType == enum_BP_LOCATION_TYPE.BPLT_CODE_FILE_LINE)
            {
                int result = this.GetDocumentPosition(out string fileName, out TEXT_POSITION position);
                if (result < 0)
                {
                    return result;
                }

                uint oneBasedLine = position.dwLine + 1;
                uint oneBasedColumn = position.dwColumn + 1;
                result = this.engine.CreateSourceBreakpoint(fileName, oneBasedLine, oneBasedColumn, out breakpointId);
                if (result == VSConstants.S_OK)
                {
                    RadDbgCodeContext codeContext = new RadDbgCodeContext(0);
                    RadDbgDocumentContext documentContext = new RadDbgDocumentContext(fileName, oneBasedLine, oneBasedColumn, codeContext);
                    codeContext.SetDocumentContext(documentContext);
                    resolution = new RadDbgBreakpointResolution(this.engine.CurrentProgram, codeContext);
                }
                return result;
            }

            if (locationType == enum_BP_LOCATION_TYPE.BPLT_CODE_CONTEXT)
            {
                int result = this.GetCodeContext(out RadDbgCodeContext codeContext);
                if (result < 0)
                {
                    return result;
                }

                result = this.engine.CreateAddressBreakpoint(codeContext.Address, this.enabled, RadDbgAddressBreakpointMode.Auto, out breakpointId);
                if (result == VSConstants.S_OK)
                {
                    resolution = new RadDbgBreakpointResolution(this.engine.CurrentProgram, codeContext);
                }
                return result;
            }

            return VSConstants.E_NOTIMPL;
        }

        private int GetDocumentPosition(out string fileName, out TEXT_POSITION position)
        {
            fileName = string.Empty;
            position = new TEXT_POSITION();
            IntPtr location = this.requestInfo.bpLocation.unionmember2;
            if (location == IntPtr.Zero)
            {
                return VSConstants.E_INVALIDARG;
            }

            try
            {
                IDebugDocumentPosition2 documentPosition = (IDebugDocumentPosition2)Marshal.GetObjectForIUnknown(location);
                int result = documentPosition.GetFileName(out fileName);
                if (result < 0)
                {
                    return result;
                }

                TEXT_POSITION[] start = new TEXT_POSITION[1];
                TEXT_POSITION[] end = new TEXT_POSITION[1];
                result = documentPosition.GetRange(start, end);
                if (result < 0)
                {
                    return result;
                }

                position = start[0];
                return VSConstants.S_OK;
            }
            finally
            {
                Marshal.Release(location);
            }
        }

        private int GetCodeContext(out RadDbgCodeContext codeContext)
        {
            codeContext = null!;
            IntPtr location = this.requestInfo.bpLocation.unionmember1;
            if (location == IntPtr.Zero)
            {
                return VSConstants.E_INVALIDARG;
            }

            try
            {
                IDebugCodeContext2 context = (IDebugCodeContext2)Marshal.GetObjectForIUnknown(location);
                if (context is not RadDbgCodeContext radContext)
                {
                    this.SetError(UnsupportedBreakpoint);
                    return VSConstants.E_NOTIMPL;
                }

                codeContext = radContext;
                return VSConstants.S_OK;
            }
            finally
            {
                Marshal.Release(location);
            }
        }

        int IDebugPendingBreakpoint2.Bind()
        {
            lock (this.boundBreakpoints)
            {
                if (!this.CanBindCore())
                {
                    if (this.errorBreakpoint != null)
                    {
                        this.engine.SendBreakpointError(this.errorBreakpoint);
                    }
                    return VSConstants.S_FALSE;
                }

                if (this.boundBreakpoints.Count != 0)
                {
                    return VSConstants.S_FALSE;
                }

                int result = this.TryCreateNativeBreakpoint(out ulong breakpointId, out RadDbgBreakpointResolution? resolution);
                if (result != VSConstants.S_OK || resolution == null)
                {
                    this.SetError($"RAD failed to bind breakpoint (HRESULT 0x{result:X8}).", enum_BP_ERROR_TYPE.BPET_GENERAL_ERROR, sendEvent: true);
                    return VSConstants.S_FALSE;
                }

                RadDbgBoundBreakpoint boundBreakpoint = new RadDbgBoundBreakpoint(this.engine, this, resolution, breakpointId, this.enabled);
                this.boundBreakpoints.Add(boundBreakpoint);
                this.engine.RegisterBoundBreakpoint(boundBreakpoint);
                this.engine.SendBreakpointBound(this, boundBreakpoint);
                return VSConstants.S_OK;
            }
        }

        int IDebugPendingBreakpoint2.CanBind(out IEnumDebugErrorBreakpoints2 ppErrorEnum)
        {
            ppErrorEnum = null!;
            if (!this.CanBindCore())
            {
                ppErrorEnum = new RadDbgErrorBreakpointEnum(this.errorBreakpoint == null ? Array.Empty<IDebugErrorBreakpoint2>() : new IDebugErrorBreakpoint2[] { this.errorBreakpoint });
                return VSConstants.S_FALSE;
            }
            return VSConstants.S_OK;
        }

        int IDebugPendingBreakpoint2.Delete()
        {
            lock (this.boundBreakpoints)
            {
                for (int index = this.boundBreakpoints.Count - 1; index >= 0; index--)
                {
                    ((IDebugBoundBreakpoint2)this.boundBreakpoints[index]).Delete();
                }
                this.boundBreakpoints.Clear();
                this.deleted = true;
            }
            return VSConstants.S_OK;
        }

        int IDebugPendingBreakpoint2.Enable(int fEnable)
        {
            this.enabled = fEnable != 0;
            lock (this.boundBreakpoints)
            {
                foreach (RadDbgBoundBreakpoint breakpoint in this.boundBreakpoints)
                {
                    ((IDebugBoundBreakpoint2)breakpoint).Enable(fEnable);
                }
            }
            return VSConstants.S_OK;
        }

        int IDebugPendingBreakpoint2.EnumBoundBreakpoints(out IEnumDebugBoundBreakpoints2 ppEnum)
        {
            lock (this.boundBreakpoints)
            {
                ppEnum = new RadDbgBoundBreakpointEnum(this.boundBreakpoints.ToArray());
            }
            return VSConstants.S_OK;
        }

        int IDebugPendingBreakpoint2.EnumErrorBreakpoints(enum_BP_ERROR_TYPE bpErrorType, out IEnumDebugErrorBreakpoints2 ppEnum)
        {
            if (this.errorBreakpoint == null)
            {
                ppEnum = null!;
                return VSConstants.S_FALSE;
            }

            ppEnum = new RadDbgErrorBreakpointEnum(new IDebugErrorBreakpoint2[] { this.errorBreakpoint });
            return VSConstants.S_OK;
        }

        int IDebugPendingBreakpoint2.GetBreakpointRequest(out IDebugBreakpointRequest2 ppBPRequest)
        {
            ppBPRequest = this.request;
            return VSConstants.S_OK;
        }

        int IDebugPendingBreakpoint2.GetState(PENDING_BP_STATE_INFO[] pState)
        {
            pState[0].state = this.deleted ? enum_PENDING_BP_STATE.PBPS_DELETED : this.enabled ? enum_PENDING_BP_STATE.PBPS_ENABLED : enum_PENDING_BP_STATE.PBPS_DISABLED;
            return VSConstants.S_OK;
        }

        int IDebugPendingBreakpoint2.SetCondition(BP_CONDITION bpCondition)
        {
            if (!this.VerifyCondition(bpCondition))
            {
                this.SetError(UnsupportedCondition, enum_BP_ERROR_TYPE.BPET_GENERAL_ERROR, sendEvent: true);
                return VSConstants.E_FAIL;
            }

            this.requestInfo.bpCondition = bpCondition;
            this.requestInfo.dwFields |= enum_BPREQI_FIELDS.BPREQI_CONDITION;
            return VSConstants.S_OK;
        }

        int IDebugPendingBreakpoint2.SetPassCount(BP_PASSCOUNT bpPassCount)
        {
            if (!this.VerifyPassCount(bpPassCount))
            {
                this.SetError(UnsupportedPassCount, enum_BP_ERROR_TYPE.BPET_GENERAL_ERROR, sendEvent: true);
                return VSConstants.E_FAIL;
            }

            this.requestInfo.bpPassCount = bpPassCount;
            this.requestInfo.dwFields |= enum_BPREQI_FIELDS.BPREQI_PASSCOUNT;
            return VSConstants.S_OK;
        }

        int IDebugPendingBreakpoint2.Virtualize(int fVirtualize)
        {
            return VSConstants.S_OK;
        }
    }

    internal sealed class RadDbgBoundBreakpoint : IDebugBoundBreakpoint2
    {
        private readonly RadDbgEngine engine;
        private readonly RadDbgPendingBreakpoint pendingBreakpoint;
        private readonly RadDbgBreakpointResolution resolution;
        private bool enabled;
        private bool deleted;

        internal RadDbgBoundBreakpoint(RadDbgEngine engine, RadDbgPendingBreakpoint pendingBreakpoint, RadDbgBreakpointResolution resolution, ulong id, bool enabled)
        {
            this.engine = engine;
            this.pendingBreakpoint = pendingBreakpoint;
            this.resolution = resolution;
            this.Id = id;
            this.enabled = enabled;
        }

        internal ulong Id { get; }

        int IDebugBoundBreakpoint2.Delete()
        {
            if (this.deleted)
            {
                return VSConstants.S_OK;
            }

            int result = this.engine.DeleteBreakpoint(this.Id);
            if (result == VSConstants.S_OK || result == VSConstants.E_NOTIMPL)
            {
                this.deleted = true;
                this.engine.UnregisterBoundBreakpoint(this.Id);
            }
            return result;
        }

        int IDebugBoundBreakpoint2.Enable(int fEnable)
        {
            bool newEnabled = fEnable != 0;
            int result = this.engine.SetBreakpointEnabled(this.Id, newEnabled);
            if (result == VSConstants.S_OK || result == VSConstants.E_NOTIMPL)
            {
                this.enabled = newEnabled;
            }
            return result;
        }

        int IDebugBoundBreakpoint2.GetBreakpointResolution(out IDebugBreakpointResolution2 ppBPResolution)
        {
            ppBPResolution = this.resolution;
            return VSConstants.S_OK;
        }

        int IDebugBoundBreakpoint2.GetHitCount(out uint pdwHitCount)
        {
            pdwHitCount = 0;
            return VSConstants.S_OK;
        }

        int IDebugBoundBreakpoint2.GetPendingBreakpoint(out IDebugPendingBreakpoint2 ppPendingBreakpoint)
        {
            ppPendingBreakpoint = this.pendingBreakpoint;
            return VSConstants.S_OK;
        }

        int IDebugBoundBreakpoint2.GetState(enum_BP_STATE[] pState)
        {
            pState[0] = this.deleted ? enum_BP_STATE.BPS_DELETED : this.enabled ? enum_BP_STATE.BPS_ENABLED : enum_BP_STATE.BPS_DISABLED;
            return VSConstants.S_OK;
        }

        int IDebugBoundBreakpoint2.SetCondition(BP_CONDITION bpCondition)
        {
            return ((IDebugPendingBreakpoint2)this.pendingBreakpoint).SetCondition(bpCondition);
        }

        int IDebugBoundBreakpoint2.SetHitCount(uint dwHitCount)
        {
            return VSConstants.E_NOTIMPL;
        }

        int IDebugBoundBreakpoint2.SetPassCount(BP_PASSCOUNT bpPassCount)
        {
            return ((IDebugPendingBreakpoint2)this.pendingBreakpoint).SetPassCount(bpPassCount);
        }
    }

    internal sealed class RadDbgBreakpointResolution : IDebugBreakpointResolution2
    {
        private readonly IDebugProgram2? program;
        private readonly RadDbgCodeContext codeContext;

        internal RadDbgBreakpointResolution(IDebugProgram2? program, RadDbgCodeContext codeContext)
        {
            this.program = program;
            this.codeContext = codeContext;
        }

        int IDebugBreakpointResolution2.GetBreakpointType(enum_BP_TYPE[] pBPType)
        {
            pBPType[0] = enum_BP_TYPE.BPT_CODE;
            return VSConstants.S_OK;
        }

        int IDebugBreakpointResolution2.GetResolutionInfo(enum_BPRESI_FIELDS dwFields, BP_RESOLUTION_INFO[] pBPResolutionInfo)
        {
            pBPResolutionInfo[0].dwFields = 0;
            if ((dwFields & enum_BPRESI_FIELDS.BPRESI_BPRESLOCATION) != 0)
            {
                BP_RESOLUTION_LOCATION location = new BP_RESOLUTION_LOCATION
                {
                    bpType = (uint)enum_BP_TYPE.BPT_CODE,
                    unionmember1 = Marshal.GetComInterfaceForObject(this.codeContext, typeof(IDebugCodeContext2)),
                };
                pBPResolutionInfo[0].bpResLocation = location;
                pBPResolutionInfo[0].dwFields |= enum_BPRESI_FIELDS.BPRESI_BPRESLOCATION;
            }
            if ((dwFields & enum_BPRESI_FIELDS.BPRESI_PROGRAM) != 0 && this.program != null)
            {
                pBPResolutionInfo[0].pProgram = this.program;
                pBPResolutionInfo[0].dwFields |= enum_BPRESI_FIELDS.BPRESI_PROGRAM;
            }
            return VSConstants.S_OK;
        }
    }

    internal sealed class RadDbgErrorBreakpoint : IDebugErrorBreakpoint2
    {
        private readonly IDebugPendingBreakpoint2 pendingBreakpoint;
        private readonly string message;
        private readonly enum_BP_ERROR_TYPE errorType;

        internal RadDbgErrorBreakpoint(IDebugPendingBreakpoint2 pendingBreakpoint, string message, enum_BP_ERROR_TYPE errorType)
        {
            this.pendingBreakpoint = pendingBreakpoint;
            this.message = message;
            this.errorType = errorType;
        }

        int IDebugErrorBreakpoint2.GetBreakpointResolution(out IDebugErrorBreakpointResolution2 ppErrorResolution)
        {
            ppErrorResolution = new RadDbgErrorBreakpointResolution(this.message, this.errorType);
            return VSConstants.S_OK;
        }

        int IDebugErrorBreakpoint2.GetPendingBreakpoint(out IDebugPendingBreakpoint2 ppPendingBreakpoint)
        {
            ppPendingBreakpoint = this.pendingBreakpoint;
            return VSConstants.S_OK;
        }
    }

    internal sealed class RadDbgErrorBreakpointResolution : IDebugErrorBreakpointResolution2
    {
        private readonly string message;
        private readonly enum_BP_ERROR_TYPE errorType;

        internal RadDbgErrorBreakpointResolution(string message, enum_BP_ERROR_TYPE errorType)
        {
            this.message = message;
            this.errorType = errorType;
        }

        int IDebugErrorBreakpointResolution2.GetBreakpointType(enum_BP_TYPE[] pBPType)
        {
            pBPType[0] = enum_BP_TYPE.BPT_CODE;
            return VSConstants.S_OK;
        }

        int IDebugErrorBreakpointResolution2.GetResolutionInfo(enum_BPERESI_FIELDS dwFields, BP_ERROR_RESOLUTION_INFO[] info)
        {
            info[0].dwFields = 0;
            if ((dwFields & enum_BPERESI_FIELDS.BPERESI_MESSAGE) != 0)
            {
                info[0].bstrMessage = this.message;
                info[0].dwFields |= enum_BPERESI_FIELDS.BPERESI_MESSAGE;
            }
            if ((dwFields & enum_BPERESI_FIELDS.BPERESI_TYPE) != 0)
            {
                info[0].dwType = this.errorType;
                info[0].dwFields |= enum_BPERESI_FIELDS.BPERESI_TYPE;
            }
            return VSConstants.S_OK;
        }
    }

    internal sealed class RadDbgBoundBreakpointEnum : RadDbgEnum<IDebugBoundBreakpoint2, IEnumDebugBoundBreakpoints2>, IEnumDebugBoundBreakpoints2
    {
        internal RadDbgBoundBreakpointEnum(IDebugBoundBreakpoint2[] data) : base(data) { }

        public int Next(uint celt, IDebugBoundBreakpoint2[] rgelt, ref uint pceltFetched)
        {
            return this.Move(celt, rgelt, ref pceltFetched);
        }
    }

    internal sealed class RadDbgErrorBreakpointEnum : RadDbgEnum<IDebugErrorBreakpoint2, IEnumDebugErrorBreakpoints2>, IEnumDebugErrorBreakpoints2
    {
        internal RadDbgErrorBreakpointEnum(IDebugErrorBreakpoint2[] data) : base(data) { }

        public int Next(uint celt, IDebugErrorBreakpoint2[] rgelt, ref uint pceltFetched)
        {
            return this.Move(celt, rgelt, ref pceltFetched);
        }
    }

    internal abstract class RadDbgEnum<T, TEnum> where TEnum : class
    {
        private readonly T[] data;
        private uint position;

        protected RadDbgEnum(T[] data)
        {
            this.data = data;
        }

        public int Clone(out TEnum ppEnum)
        {
            ppEnum = null!;
            return VSConstants.E_NOTIMPL;
        }

        public int GetCount(out uint pcelt)
        {
            pcelt = (uint)this.data.Length;
            return VSConstants.S_OK;
        }

        public int Reset()
        {
            this.position = 0;
            return VSConstants.S_OK;
        }

        public int Skip(uint celt)
        {
            uint fetched = 0;
            return this.Move(celt, null, ref fetched);
        }

        protected int Move(uint celt, T[]? rgelt, ref uint pceltFetched)
        {
            uint remaining = (uint)this.data.Length - this.position;
            pceltFetched = Math.Min(celt, remaining);
            if (rgelt != null)
            {
                for (uint index = 0; index < pceltFetched; index++)
                {
                    rgelt[index] = this.data[this.position + index];
                }
            }
            this.position += pceltFetched;
            return pceltFetched == celt ? VSConstants.S_OK : VSConstants.S_FALSE;
        }
    }
}
