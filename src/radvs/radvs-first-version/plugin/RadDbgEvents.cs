using System;
using Microsoft.VisualStudio.Debugger.Interop;

namespace RAD
{
    internal class RadDbgEvent : IDebugEvent2
    {
        private readonly uint attributes;
        internal RadDbgEvent(uint attributes)
        {
            this.attributes = attributes;
        }
        public int GetAttributes(out uint pdwAttrib)
        {
            pdwAttrib = this.attributes;
            return 0;
        }
    }

    internal sealed class RadEngineCreateEvent : RadDbgEvent, IDebugEngineCreateEvent2
    {
        private readonly IDebugEngine2 engine;
        internal RadEngineCreateEvent(IDebugEngine2 engine) : base((uint)enum_EVENTATTRIBUTES.EVENT_ASYNCHRONOUS)
        {
            this.engine = engine;
        }
        public int GetEngine(out IDebugEngine2 ppEngine)
        {
            ppEngine = this.engine;
            return 0;
        }
    }

    internal sealed class RadProgramCreateEvent : RadDbgEvent, IDebugProgramCreateEvent2
    {
        internal bool StartsNativeSession { get; }

        internal RadProgramCreateEvent(bool startsNativeSession = true) : base((uint)enum_EVENTATTRIBUTES.EVENT_SYNCHRONOUS)
        {
            this.StartsNativeSession = startsNativeSession;
        }
    }

    internal sealed class RadLoadCompleteEvent : RadDbgEvent, IDebugLoadCompleteEvent2
    {
        internal RadLoadCompleteEvent(uint attributes) : base(attributes) {}
    }

    internal sealed class RadThreadCreateEvent : RadDbgEvent, IDebugThreadCreateEvent2
    {
        internal RadThreadCreateEvent(uint attributes) : base(attributes) {}
    }

    internal sealed class RadThreadDestroyEvent : RadDbgEvent, IDebugThreadDestroyEvent2
    {
        private readonly uint exitCode;
        internal RadThreadDestroyEvent(uint exitCode, uint attributes) : base(attributes)
        {
            this.exitCode = exitCode;
        }
        public int GetExitCode(out uint pdwExitCode)
        {
            pdwExitCode = this.exitCode;
            return 0;
        }
    }

    internal sealed class RadProgramDestroyEvent : RadDbgEvent, IDebugProgramDestroyEvent2
    {
        private readonly uint exitCode;
        internal ulong Sequence { get; }

        internal RadProgramDestroyEvent(uint exitCode, ulong sequence, uint attributes) : base(attributes)
        {
            this.exitCode = exitCode;
            this.Sequence = sequence;
        }
        public int GetExitCode(out uint pdwExitCode)
        {
            pdwExitCode = this.exitCode;
            return 0;
        }
    }

    internal sealed class RadBreakEvent : RadDbgEvent, IDebugBreakEvent2
    {
        internal RadBreakEvent(uint attributes) : base(attributes) {}
    }

    internal sealed class RadStopCompleteEvent : RadDbgEvent, IDebugStopCompleteEvent2
    {
        internal RadStopCompleteEvent(uint attributes) : base(attributes) {}
    }

    internal sealed class RadBreakpointBoundEvent : RadDbgEvent, IDebugBreakpointBoundEvent2
    {
        private readonly IDebugPendingBreakpoint2 pendingBreakpoint;
        private readonly IDebugBoundBreakpoint2 boundBreakpoint;

        internal RadBreakpointBoundEvent(IDebugPendingBreakpoint2 pendingBreakpoint, IDebugBoundBreakpoint2 boundBreakpoint) : base((uint)enum_EVENTATTRIBUTES.EVENT_ASYNCHRONOUS)
        {
            this.pendingBreakpoint = pendingBreakpoint;
            this.boundBreakpoint = boundBreakpoint;
        }

        public int EnumBoundBreakpoints(out IEnumDebugBoundBreakpoints2 ppEnum)
        {
            ppEnum = new RadDbgBoundBreakpointEnum(new IDebugBoundBreakpoint2[] { this.boundBreakpoint });
            return 0;
        }

        public int GetPendingBreakpoint(out IDebugPendingBreakpoint2 ppPendingBP)
        {
            ppPendingBP = this.pendingBreakpoint;
            return 0;
        }
    }

    internal sealed class RadBreakpointErrorEvent : RadDbgEvent, IDebugBreakpointErrorEvent2
    {
        private readonly IDebugErrorBreakpoint2 errorBreakpoint;

        internal RadBreakpointErrorEvent(IDebugErrorBreakpoint2 errorBreakpoint) : base((uint)enum_EVENTATTRIBUTES.EVENT_ASYNCHRONOUS)
        {
            this.errorBreakpoint = errorBreakpoint;
        }

        public int GetErrorBreakpoint(out IDebugErrorBreakpoint2 ppErrorBP)
        {
            ppErrorBP = this.errorBreakpoint;
            return 0;
        }
    }

    internal sealed class RadBreakpointEvent : RadDbgEvent, IDebugBreakpointEvent2
    {
        private readonly IDebugBoundBreakpoint2[] boundBreakpoints;

        internal RadBreakpointEvent(IDebugBoundBreakpoint2[] boundBreakpoints, uint attributes) : base(attributes)
        {
            this.boundBreakpoints = boundBreakpoints;
        }

        public int EnumBreakpoints(out IEnumDebugBoundBreakpoints2 ppEnum)
        {
            ppEnum = new RadDbgBoundBreakpointEnum(this.boundBreakpoints);
            return 0;
        }
    }

    internal sealed class RadExceptionEvent : RadDbgEvent, IDebugExceptionEvent2
    {
        private readonly uint code;
        private readonly bool repeated;
        internal RadExceptionEvent(uint code, bool repeated, uint attributes) : base(attributes)
        {
            this.code     = code;
            this.repeated = repeated;
        }
        public int CanPassToDebuggee()
        {
            return 1;
        }
        public int GetException(EXCEPTION_INFO[] pExceptionInfo)
        {
            pExceptionInfo[0] = new EXCEPTION_INFO
            {
                bstrExceptionName = $"0x{this.code:X8}",
                dwCode            = this.code,
                dwState           = this.repeated ? enum_EXCEPTION_STATE.EXCEPTION_STOP_SECOND_CHANCE : enum_EXCEPTION_STATE.EXCEPTION_STOP_FIRST_CHANCE,
                guidType          = Guid.Empty,
            };
            return 0;
        }
        public int GetExceptionDescription(out string pbstrDescription)
        {
            pbstrDescription = $"Debuggee exception 0x{this.code:X8}";
            return 0;
        }
        public int PassToDebuggee(int fPass)
        {
            return 0;
        }
    }

    internal sealed class RadOutputStringEvent : RadDbgEvent, IDebugOutputStringEvent2
    {
        private readonly string text;
        internal RadOutputStringEvent(string text, uint attributes) : base(attributes)
        {
            this.text = text;
        }
        public int GetString(out string pbstrString)
        {
            pbstrString = this.text;
            return 0;
        }
    }
}
