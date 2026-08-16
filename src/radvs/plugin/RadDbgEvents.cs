using System;
using Microsoft.VisualStudio.Debugger.Interop;

namespace RAD
{
    internal abstract class RadDbgEvent : IDebugEvent2
    {
        private readonly uint attributes;

        protected RadDbgEvent(uint attributes)
        {
            this.attributes = attributes;
        }

        public int GetAttributes(out uint pdwAttrib)
        {
            pdwAttrib = this.attributes;
            return RadDbgHResult.S_OK;
        }
    }

    internal sealed class RadDbgEngineCreateEvent : RadDbgEvent, IDebugEngineCreateEvent2
    {
        private readonly IDebugEngine2 engine;

        internal RadDbgEngineCreateEvent(IDebugEngine2 engine)
            : base((uint)enum_EVENTATTRIBUTES.EVENT_ASYNCHRONOUS)
        {
            this.engine = engine;
        }

        public int GetEngine(out IDebugEngine2 pEngine)
        {
            pEngine = this.engine;
            return RadDbgHResult.S_OK;
        }
    }

    internal sealed class RadDbgProgramCreateEvent : RadDbgEvent, IDebugProgramCreateEvent2
    {
        internal RadDbgProgramCreateEvent()
            : base((uint)enum_EVENTATTRIBUTES.EVENT_SYNCHRONOUS)
        {
        }
    }

    internal sealed class RadDbgLoadCompleteEvent : RadDbgEvent, IDebugLoadCompleteEvent2
    {
        internal RadDbgLoadCompleteEvent(uint attributes)
            : base(attributes)
        {
        }
    }

    internal sealed class RadDbgProgramDestroyEvent : RadDbgEvent, IDebugProgramDestroyEvent2
    {
        private readonly uint exitCode;

        internal RadDbgProgramDestroyEvent(uint attributes, uint exitCode, ulong sequence)
            : base(attributes)
        {
            this.exitCode = exitCode;
            this.Sequence = sequence;
        }

        internal ulong Sequence { get; }

        public int GetExitCode(out uint pdwExit)
        {
            pdwExit = this.exitCode;
            return RadDbgHResult.S_OK;
        }
    }

    internal sealed class RadDbgOutputStringEvent : RadDbgEvent, IDebugOutputStringEvent2
    {
        private readonly string text;

        internal RadDbgOutputStringEvent(uint attributes, string text)
            : base(attributes)
        {
            this.text = text;
        }

        public int GetString(out string pbstrString)
        {
            pbstrString = this.text;
            return RadDbgHResult.S_OK;
        }
    }

    internal sealed class RadDbgThreadCreateEvent : RadDbgEvent, IDebugThreadCreateEvent2
    {
        internal RadDbgThreadCreateEvent(uint attributes)
            : base(attributes)
        {
        }
    }

    internal sealed class RadDbgThreadDestroyEvent : RadDbgEvent, IDebugThreadDestroyEvent2
    {
        private readonly uint exitCode;

        internal RadDbgThreadDestroyEvent(uint attributes, uint exitCode)
            : base(attributes)
        {
            this.exitCode = exitCode;
        }

        public int GetExitCode(out uint pdwExit)
        {
            pdwExit = this.exitCode;
            return RadDbgHResult.S_OK;
        }
    }

    internal sealed class RadDbgStopCompleteEvent : RadDbgEvent, IDebugStopCompleteEvent2
    {
        internal RadDbgStopCompleteEvent(uint attributes)
            : base(attributes)
        {
        }
    }

    internal sealed class RadDbgBreakEvent : RadDbgEvent, IDebugBreakEvent2
    {
        internal RadDbgBreakEvent(uint attributes)
            : base(attributes)
        {
        }
    }

    internal sealed class RadDbgExceptionEvent : RadDbgEvent, IDebugExceptionEvent2
    {
        private readonly EXCEPTION_INFO exceptionInfo;
        private readonly string description;

        internal RadDbgExceptionEvent(uint attributes, EXCEPTION_INFO exceptionInfo, string description)
            : base(attributes)
        {
            this.exceptionInfo = exceptionInfo;
            this.description = description;
        }

        public int GetException(EXCEPTION_INFO[] pExceptionInfo)
        {
            if (pExceptionInfo == null || pExceptionInfo.Length == 0)
            {
                return RadDbgHResult.E_POINTER;
            }
            pExceptionInfo[0] = this.exceptionInfo;
            return RadDbgHResult.S_OK;
        }

        public int GetExceptionDescription(out string pbstrDescription)
        {
            pbstrDescription = this.description;
            return RadDbgHResult.S_OK;
        }

        public int CanPassToDebuggee()
        {
            return RadDbgHResult.S_FALSE;
        }

        public int PassToDebuggee(int fPass)
        {
            return RadDbgHResult.S_OK;
        }
    }

    internal static class RadDbgEventFactory
    {
        internal static IDebugEvent2? Create(RadDbgNativeEvent nativeEvent)
        {
            Guid iid = nativeEvent.EventIid;
            if (iid == typeof(IDebugLoadCompleteEvent2).GUID)
            {
                return new RadDbgLoadCompleteEvent(nativeEvent.Attributes);
            }
            if (iid == typeof(IDebugProgramDestroyEvent2).GUID)
            {
                return new RadDbgProgramDestroyEvent(nativeEvent.Attributes, nativeEvent.ExitCode, nativeEvent.Sequence);
            }
            if (iid == typeof(IDebugThreadCreateEvent2).GUID)
            {
                return new RadDbgThreadCreateEvent(nativeEvent.Attributes);
            }
            if (iid == typeof(IDebugThreadDestroyEvent2).GUID)
            {
                return new RadDbgThreadDestroyEvent(nativeEvent.Attributes, nativeEvent.ExitCode);
            }
            if (iid == typeof(IDebugStopCompleteEvent2).GUID)
            {
                return new RadDbgStopCompleteEvent(nativeEvent.Attributes);
            }
            if (iid == typeof(IDebugBreakEvent2).GUID)
            {
                return new RadDbgBreakEvent(nativeEvent.Attributes);
            }
            if (iid == typeof(IDebugExceptionEvent2).GUID)
            {
                return new RadDbgExceptionEvent(nativeEvent.Attributes, nativeEvent.ExceptionInfo, nativeEvent.Text);
            }
            if (iid == typeof(IDebugOutputStringEvent2).GUID)
            {
                return new RadDbgOutputStringEvent(nativeEvent.Attributes, nativeEvent.Text);
            }
            return null;
        }
    }
}
