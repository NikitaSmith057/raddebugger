using System;
using System.ComponentModel;
using System.IO;
using System.Runtime.InteropServices;
using System.Text;
using Microsoft.VisualStudio.Debugger.Interop;
using Microsoft.Win32.SafeHandles;

namespace RAD
{
    [StructLayout(LayoutKind.Sequential)]
    internal struct String8
    {
        internal IntPtr str;
        internal ulong  size;
    }

    internal sealed class RadDbgSessionHandle : SafeHandleZeroOrMinusOneIsInvalid
    {
        internal RadDbgSessionHandle(IntPtr handle) : base(ownsHandle: true)
        {
            this.SetHandle(handle);
        }

        private RadDbgSessionHandle() : base(ownsHandle: true)
        {
        }

        protected override bool ReleaseHandle()
        {
            return RadDbgNative.DestroySession(this.handle) >= 0;
        }
    }

    internal sealed class RadDbgNativeEvent
    {
        internal Guid             EventIid;
        internal uint             Attributes;
        internal ulong            Sequence;
        internal uint             ExitCode;
        internal THREADPROPERTIES ThreadProperties;
        internal EXCEPTION_INFO   ExceptionInfo;
        internal string           Text = string.Empty;
    }

    internal static class RadDbgNative
    {
        private const string AbiVersionExport               = "radvs_ad7_bridge_abi_version";
        private const string SessionCreateExport            = "radvs_ad7_bridge_session_create";
        private const string SessionDestroyExport           = "radvs_ad7_bridge_session_destroy";
        private const string SessionLaunchExport            = "radvs_ad7_bridge_session_launch";
        private const string ContinueSynchronousEventExport = "radvs_ad7_bridge_continue_synchronous_event";
        private const string RunExport                      = "radvs_ad7_bridge_run";
        private const string BreakExport                    = "radvs_ad7_bridge_break";
        private const string TerminateExport                = "radvs_ad7_bridge_terminate";
        private const string CloseEventWaitExport           = "radvs_ad7_bridge_close_event_wait";
        private const string WaitEventExport                = "radvs_ad7_bridge_wait_event";
        private const string GetProgramNameExport           = "radvs_ad7_bridge_get_program_name";
        private const string GetHostNameExport              = "radvs_ad7_bridge_get_host_name";
        private const string GetHostPidExport               = "radvs_ad7_bridge_get_host_pid";
        private const string GetHostMachineNameExport       = "radvs_ad7_bridge_get_host_machine_name";
        private const string GetEngineInfoExport            = "radvs_ad7_bridge_get_engine_info";
        private const string CopyThreadsExport              = "radvs_ad7_bridge_copy_threads";
        private const string GetThreadPropertiesExport      = "radvs_ad7_bridge_get_thread_properties";

        private const int ErrorInsufficientBuffer = unchecked((int)0x8007007A);

        private static readonly Lazy<Exports> NativeExports = new Lazy<Exports>(LoadExports, isThreadSafe: true);

        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        private delegate uint AbiVersionDelegate();

        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        private delegate int SessionCreateDelegate(out IntPtr session);

        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        private delegate int SessionDestroyDelegate(IntPtr session);

        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        private delegate int SessionLaunchDelegate(
            RadDbgSessionHandle session,
            String8 executable,
            String8 arguments,
            String8 workingDirectory,
            out AD_PROCESS_ID processId);

        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        private delegate int SequenceDelegate(RadDbgSessionHandle session, ulong sequence);

        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        private delegate int SessionDelegate(RadDbgSessionHandle session);

        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        private delegate int WaitEventDelegate(
            RadDbgSessionHandle session,
            uint timeout,
            out Guid  eventIid,
            out uint  attributes,
            out ulong sequence,
            out uint  exitCode,
            [Out, MarshalAs(UnmanagedType.LPArray, SizeConst = 1)] THREADPROPERTIES[] threadProperties,
            [Out, MarshalAs(UnmanagedType.LPArray, SizeConst = 1)] EXCEPTION_INFO[] exceptionInfo,
            ref String8 text);

        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        private delegate int GetStringDelegate(RadDbgSessionHandle session, ref String8 value);

        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        private delegate int GetHostNameDelegate(
            RadDbgSessionHandle session,
            enum_GETHOSTNAME_TYPE type,
            ref String8 value);

        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        private delegate int GetProcessIdDelegate(RadDbgSessionHandle session, out AD_PROCESS_ID processId);

        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        private delegate int GetEngineInfoDelegate(RadDbgSessionHandle session, ref String8 name, out Guid engineId);

        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        private delegate int CopyThreadsDelegate(
            RadDbgSessionHandle session,
            [Out, MarshalAs(UnmanagedType.LPArray, SizeParamIndex = 2)] THREADPROPERTIES[]? buffer,
            ulong count,
            out ulong copiedCount);

        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        private delegate int GetThreadPropertiesDelegate(
            RadDbgSessionHandle session,
            uint threadId,
            enum_THREADPROPERTY_FIELDS fields,
            [Out, MarshalAs(UnmanagedType.LPArray, SizeConst = 1)] THREADPROPERTIES[] properties,
            ref String8 name);

        private delegate int FillString(ref String8 value);

        internal static int CreateSession(out RadDbgSessionHandle? session)
        {
            session = null;
            Exports exports;
            try
            {
                exports = NativeExports.Value;
            }
            catch (Exception exception)
            {
                return Marshal.GetHRForException(exception);
            }

            int result = exports.SessionCreate(out IntPtr nativeSession);
            if (result < 0)
            {
                return result;
            }
            if (nativeSession == IntPtr.Zero)
            {
                return RadDbgHResult.E_FAIL;
            }

            session = new RadDbgSessionHandle(nativeSession);
            return result;
        }

        internal static int Launch(
            RadDbgSessionHandle session,
            string              executable,
            string?             arguments,
            string?             workingDirectory,
            out AD_PROCESS_ID processId)
        {
            processId = default;
            try
            {
                using (PinnedUtf8 executableUtf8 = new PinnedUtf8(executable))
                using (PinnedUtf8 argumentsUtf8  = new PinnedUtf8(arguments))
                using (PinnedUtf8 directoryUtf8  = new PinnedUtf8(workingDirectory))
                {
                    return NativeExports.Value.SessionLaunch(
                        session,
                        executableUtf8.Value,
                        argumentsUtf8.Value,
                        directoryUtf8.Value,
                        out processId);
                }
            }
            catch (Exception exception)
            {
                return Marshal.GetHRForException(exception);
            }
        }

        internal static int ContinueSynchronousEvent(RadDbgSessionHandle session, ulong sequence)
        {
            return Invoke(() => NativeExports.Value.ContinueSynchronousEvent(session, sequence));
        }

        internal static int Run(RadDbgSessionHandle session)
        {
            return Invoke(() => NativeExports.Value.Run(session));
        }

        internal static int Break(RadDbgSessionHandle session)
        {
            return Invoke(() => NativeExports.Value.Break(session));
        }

        internal static int Terminate(RadDbgSessionHandle session)
        {
            return Invoke(() => NativeExports.Value.Terminate(session));
        }

        internal static int CloseEventWait(RadDbgSessionHandle session)
        {
            return Invoke(() => NativeExports.Value.CloseEventWait(session));
        }

        internal static int WaitEvent(RadDbgSessionHandle session, uint timeout, out RadDbgNativeEvent? nativeEvent)
        {
            nativeEvent = null;
            try
            {
                THREADPROPERTIES[] threadProperties = new THREADPROPERTIES[1];
                EXCEPTION_INFO[] exceptionInfo = new EXCEPTION_INFO[1];
                String8 text = default;
                int result = NativeExports.Value.WaitEvent(
                    session,
                    timeout,
                    out Guid eventIid,
                    out uint attributes,
                    out ulong sequence,
                    out uint exitCode,
                    threadProperties,
                    exceptionInfo,
                    ref text);

                if (result < 0 && result != ErrorInsufficientBuffer)
                {
                    return result;
                }

                string decodedText = string.Empty;
                if (text.size != 0)
                {
                    if (text.size > int.MaxValue)
                    {
                        return RadDbgHResult.E_OUTOFMEMORY;
                    }

                    byte[] buffer = new byte[(int)text.size];
                    GCHandle pin = GCHandle.Alloc(buffer, GCHandleType.Pinned);
                    try
                    {
                        text.str = pin.AddrOfPinnedObject();
                        text.size = (ulong)buffer.Length;
                        result = NativeExports.Value.WaitEvent(
                            session,
                            timeout,
                            out eventIid,
                            out attributes,
                            out sequence,
                            out exitCode,
                            threadProperties,
                            exceptionInfo,
                            ref text);
                        if (result < 0)
                        {
                            return result;
                        }
                        if (text.size > (ulong)buffer.Length)
                        {
                            return ErrorInsufficientBuffer;
                        }
                        decodedText = Encoding.UTF8.GetString(buffer, 0, (int)text.size);
                    }
                    finally
                    {
                        pin.Free();
                    }
                }

                nativeEvent = new RadDbgNativeEvent
                {
                    EventIid         = eventIid,
                    Attributes       = attributes,
                    Sequence         = sequence,
                    ExitCode         = exitCode,
                    ThreadProperties = threadProperties[0],
                    ExceptionInfo    = exceptionInfo[0],
                    Text             = decodedText,
                };
                return result;
            }
            catch (Exception exception)
            {
                return Marshal.GetHRForException(exception);
            }
        }

        internal static int GetProgramName(RadDbgSessionHandle session, out string name)
        {
            return GetString((ref String8 value) => NativeExports.Value.GetProgramName(session, ref value), out name);
        }

        internal static int GetHostName(
            RadDbgSessionHandle   session,
            enum_GETHOSTNAME_TYPE type,
            out string name)
        {
            return GetString((ref String8 value) => NativeExports.Value.GetHostName(session, type, ref value), out name);
        }

        internal static int GetHostPid(RadDbgSessionHandle session, out AD_PROCESS_ID processId)
        {
            processId = default;
            try
            {
                return NativeExports.Value.GetHostPid(session, out processId);
            }
            catch (Exception exception)
            {
                return Marshal.GetHRForException(exception);
            }
        }

        internal static int GetHostMachineName(RadDbgSessionHandle session, out string name)
        {
            return GetString((ref String8 value) => NativeExports.Value.GetHostMachineName(session, ref value), out name);
        }

        internal static int GetEngineInfo(RadDbgSessionHandle session, out string name, out Guid engineId)
        {
            Guid returnedEngineId = Guid.Empty;
            int result = GetString(
                (ref String8 value) => NativeExports.Value.GetEngineInfo(session, ref value, out returnedEngineId),
                out name);
            engineId = returnedEngineId;
            return result;
        }

        internal static int CopyThreads(RadDbgSessionHandle session, out THREADPROPERTIES[] threads)
        {
            threads = Array.Empty<THREADPROPERTIES>();
            try
            {
                THREADPROPERTIES[]? buffer = null;
                for (;;)
                {
                    ulong capacity = (ulong)(buffer?.Length ?? 0);
                    int result = NativeExports.Value.CopyThreads(session, buffer, capacity, out ulong copiedCount);
                    if (result >= 0)
                    {
                        if (buffer == null || copiedCount == 0)
                        {
                            return RadDbgHResult.S_OK;
                        }
                        if (copiedCount > (ulong)buffer.Length)
                        {
                            return ErrorInsufficientBuffer;
                        }
                        if (copiedCount != (ulong)buffer.Length)
                        {
                            Array.Resize(ref buffer, (int)copiedCount);
                        }
                        threads = buffer;
                        return result;
                    }
                    if (result != ErrorInsufficientBuffer)
                    {
                        return result;
                    }
                    if (copiedCount > int.MaxValue)
                    {
                        return RadDbgHResult.E_OUTOFMEMORY;
                    }
                    buffer = new THREADPROPERTIES[(int)copiedCount];
                }
            }
            catch (Exception exception)
            {
                return Marshal.GetHRForException(exception);
            }
        }

        internal static int GetThreadProperties(
            RadDbgSessionHandle session,
            uint threadId,
            enum_THREADPROPERTY_FIELDS fields,
            THREADPROPERTIES[] properties)
        {
            if (properties == null || properties.Length == 0)
            {
                return RadDbgHResult.E_POINTER;
            }

            string name;
            int result = GetString(
                (ref String8 value) => NativeExports.Value.GetThreadProperties(session, threadId, fields, properties, ref value),
                out name);
            if (result >= 0 && (properties[0].dwFields & enum_THREADPROPERTY_FIELDS.TPF_NAME) != 0)
            {
                properties[0].bstrName = name;
            }
            return result;
        }

        internal static bool IsTimeout(int result)
        {
            return result == RadDbgHResult.S_FALSE || result == unchecked((int)0x800705B4);
        }

        internal static int DestroySession(IntPtr session)
        {
            if (session == IntPtr.Zero)
            {
                return RadDbgHResult.S_OK;
            }
            try
            {
                return NativeExports.Value.SessionDestroy(session);
            }
            catch
            {
                return RadDbgHResult.E_FAIL;
            }
        }

        private static int GetString(FillString fill, out string value)
        {
            value = string.Empty;
            try
            {
                String8 nativeString = default;
                int result = fill(ref nativeString);
                if (result < 0 && result != ErrorInsufficientBuffer)
                {
                    return result;
                }
                if (nativeString.size == 0)
                {
                    return RadDbgHResult.S_OK;
                }
                if (nativeString.size > int.MaxValue)
                {
                    return RadDbgHResult.E_OUTOFMEMORY;
                }

                byte[] buffer = new byte[(int)nativeString.size];
                GCHandle pin = GCHandle.Alloc(buffer, GCHandleType.Pinned);
                try
                {
                    nativeString.str  = pin.AddrOfPinnedObject();
                    nativeString.size = (ulong)buffer.Length;
                    result = fill(ref nativeString);
                    if (result < 0)
                    {
                        return result;
                    }
                    if (nativeString.size > (ulong)buffer.Length)
                    {
                        return ErrorInsufficientBuffer;
                    }
                    value = Encoding.UTF8.GetString(buffer, 0, (int)nativeString.size);
                    return result;
                }
                finally
                {
                    pin.Free();
                }
            }
            catch (Exception exception)
            {
                return Marshal.GetHRForException(exception);
            }
        }

        private static int Invoke(Func<int> action)
        {
            try
            {
                return action();
            }
            catch (Exception exception)
            {
                return Marshal.GetHRForException(exception);
            }
        }

        private const uint LoadLibrarySearchDllLoadDir  = 0x00000100;
        private const uint LoadLibrarySearchDefaultDirs = 0x00001000;

        private static Exports LoadExports()
        {
            string assemblyDirectory = Path.GetDirectoryName(typeof(RadDbgNative).Assembly.Location)
                ?? throw new InvalidOperationException("The RAD package directory is unavailable.");

            string            libraryPath = Path.Combine(assemblyDirectory, "RadDbg.dll");
            SafeLibraryHandle library     = LoadLibraryEx(libraryPath, IntPtr.Zero, LoadLibrarySearchDllLoadDir | LoadLibrarySearchDefaultDirs);
            if (library.IsInvalid)
            {
                throw new DllNotFoundException(
                    $"Unable to load '{libraryPath}': {new Win32Exception(Marshal.GetLastWin32Error()).Message}");
            }

            AbiVersionDelegate abiVersion    = GetExport<AbiVersionDelegate>(library, AbiVersionExport);
            uint               actualVersion = abiVersion();
            if (actualVersion != RadDbgBridgeVersion.ExpectedAbiVersion)
            {
                library.Dispose();
                throw new COMException(
                    $"RadDbg.dll AD7 bridge ABI {actualVersion} is incompatible with expected ABI {RadDbgBridgeVersion.ExpectedAbiVersion}.",
                    RadDbgHResult.E_NOINTERFACE);
            }

            try
            {
                Exports exports = new Exports(library);
                GC.SuppressFinalize(library);
                return exports;
            }
            catch
            {
                library.Dispose();
                throw;
            }
        }

        private static T GetExport<T>(SafeLibraryHandle library, string name)
            where T : Delegate
        {
            IntPtr address = GetProcAddress(library, name);
            if (address == IntPtr.Zero)
            {
                throw new EntryPointNotFoundException($"RadDbg.dll does not export '{name}'.");
            }
            return (T)Marshal.GetDelegateForFunctionPointer(address, typeof(T));
        }

        [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        private static extern SafeLibraryHandle LoadLibraryEx(string fileName, IntPtr file, uint flags);

        [DllImport("kernel32.dll", CharSet = CharSet.Ansi, SetLastError = true)]
        private static extern IntPtr GetProcAddress(SafeLibraryHandle module, string name);

        private sealed class SafeLibraryHandle : SafeHandleZeroOrMinusOneIsInvalid
        {
            private SafeLibraryHandle() : base(ownsHandle: true)
            {
            }

            [DllImport("kernel32.dll")]
            [return: MarshalAs(UnmanagedType.Bool)]
            private static extern bool FreeLibrary(IntPtr module);

            protected override bool ReleaseHandle()
            {
                return FreeLibrary(this.handle);
            }
        }

        private sealed class Exports
        {
            private readonly SafeLibraryHandle library; // live for the duration of the process lifetime

            internal Exports(SafeLibraryHandle library)
            {
                this.library                  = library;
                this.SessionCreate            = GetExport<SessionCreateDelegate>(library, SessionCreateExport);
                this.SessionDestroy           = GetExport<SessionDestroyDelegate>(library, SessionDestroyExport);
                this.SessionLaunch            = GetExport<SessionLaunchDelegate>(library, SessionLaunchExport);
                this.ContinueSynchronousEvent = GetExport<SequenceDelegate>(library, ContinueSynchronousEventExport);
                this.Run                      = GetExport<SessionDelegate>(library, RunExport);
                this.Break                    = GetExport<SessionDelegate>(library, BreakExport);
                this.Terminate                = GetExport<SessionDelegate>(library, TerminateExport);
                this.CloseEventWait           = GetExport<SessionDelegate>(library, CloseEventWaitExport);
                this.WaitEvent                = GetExport<WaitEventDelegate>(library, WaitEventExport);
                this.GetProgramName           = GetExport<GetStringDelegate>(library, GetProgramNameExport);
                this.GetHostName              = GetExport<GetHostNameDelegate>(library, GetHostNameExport);
                this.GetHostPid               = GetExport<GetProcessIdDelegate>(library, GetHostPidExport);
                this.GetHostMachineName       = GetExport<GetStringDelegate>(library, GetHostMachineNameExport);
                this.GetEngineInfo            = GetExport<GetEngineInfoDelegate>(library, GetEngineInfoExport);
                this.CopyThreads              = GetExport<CopyThreadsDelegate>(library, CopyThreadsExport);
                this.GetThreadProperties      = GetExport<GetThreadPropertiesDelegate>(library, GetThreadPropertiesExport);
            }

            internal SessionCreateDelegate       SessionCreate            { get; }
            internal SessionDestroyDelegate      SessionDestroy           { get; }
            internal SessionLaunchDelegate       SessionLaunch            { get; }
            internal SequenceDelegate            ContinueSynchronousEvent { get; }
            internal SessionDelegate             Run                      { get; }
            internal SessionDelegate             Break                    { get; }
            internal SessionDelegate             Terminate                { get; }
            internal SessionDelegate             CloseEventWait           { get; }
            internal WaitEventDelegate           WaitEvent                { get; }
            internal GetStringDelegate           GetProgramName           { get; }
            internal GetHostNameDelegate         GetHostName              { get; }
            internal GetProcessIdDelegate        GetHostPid               { get; }
            internal GetStringDelegate           GetHostMachineName       { get; }
            internal GetEngineInfoDelegate       GetEngineInfo            { get; }
            internal CopyThreadsDelegate         CopyThreads              { get; }
            internal GetThreadPropertiesDelegate GetThreadProperties      { get; }
        }

        private sealed class PinnedUtf8 : IDisposable
        {
            private GCHandle pin;

            internal PinnedUtf8(string? value)
            {
                if (string.IsNullOrEmpty(value))
                {
                    this.Value = default;
                    return;
                }

                byte[] bytes = Encoding.UTF8.GetBytes(value);
                this.pin   = GCHandle.Alloc(bytes, GCHandleType.Pinned);
                this.Value = new String8
                {
                    str  = this.pin.AddrOfPinnedObject(),
                    size = (ulong)bytes.Length,
                };
            }

            internal String8 Value { get; }

            public void Dispose()
            {
                if (this.pin.IsAllocated)
                {
                    this.pin.Free();
                }
            }
        }
    }

    internal static class RadDbgHResult
    {
        internal const int S_OK          = 0;
        internal const int S_FALSE       = 1;
        internal const int E_NOTIMPL     = unchecked((int)0x80004001);
        internal const int E_NOINTERFACE = unchecked((int)0x80004002);
        internal const int E_POINTER     = unchecked((int)0x80004003);
        internal const int E_FAIL        = unchecked((int)0x80004005);
        internal const int E_UNEXPECTED  = unchecked((int)0x8000FFFF);
        internal const int E_OUTOFMEMORY = unchecked((int)0x8007000E);
        internal const int E_INVALIDARG  = unchecked((int)0x80070057);
    }
}
