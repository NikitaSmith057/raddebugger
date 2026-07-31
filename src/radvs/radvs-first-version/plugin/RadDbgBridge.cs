using System;
using System.Collections.Generic;
using System.IO;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading;
using System.Threading.Tasks;
using Microsoft.VisualStudio;
using Microsoft.VisualStudio.Debugger.Interop;
using Microsoft.VisualStudio.Shell;
using Microsoft.VisualStudio.Threading;

namespace RAD
{
    [StructLayout(LayoutKind.Sequential)]
    internal struct RadDbgFrameInfo
    {
        internal ulong ProcessStateEpoch;
        internal ulong InstructionPointer;
        internal ulong ModuleBaseAddress;
        internal ulong SourceVoffFirst;
        internal ulong SourceVoffOpl;
        internal uint SourceLine;
        internal uint SourceColumn;
        internal uint HasSource;
        internal uint Reserved;
    }

    internal enum RadDbgAd7EventKind : uint
    {
        Null,
        ProgramCreated,
        LoadComplete,
        ThreadCreated,
        ThreadExited,
        Stopped,
        Output,
        ProgramDestroyed,
    }

    internal enum RadDbgAd7StopReason : uint
    {
        Null,
        Generic,
        Step,
        Exception,
        Break,
    }

    internal enum RadDbgAddressBreakpointMode : uint
    {
        Auto,
        Software,
        Hardware,
    }

    internal enum RadDbgBreakpointLocationKind : uint
    {
        Address,
        Source,
    }

    internal enum RadDbgBreakpointConditionKind : uint
    {
        Always,
        Expression,
    }

    [StructLayout(LayoutKind.Sequential)]
    internal struct RadDbgString8
    {
        internal IntPtr Str;
        internal ulong Size;
    }

    [StructLayout(LayoutKind.Sequential)]
    internal struct RadDbgSourceBreakpointSpec
    {
        internal RadDbgString8 Path;
        internal uint Line;
        internal uint Column;
    }

    [StructLayout(LayoutKind.Explicit)]
    internal struct RadDbgBreakpointSpec
    {
        [FieldOffset(0)]
        internal uint LocationKind;
        [FieldOffset(4)]
        internal uint ConditionKind;
        [FieldOffset(8)]
        internal uint Enabled;
        [FieldOffset(12)]
        internal uint AddressMode;
        [FieldOffset(16)]
        internal ulong Address;
        [FieldOffset(16)]
        internal RadDbgSourceBreakpointSpec Source;
        [FieldOffset(40)]
        internal RadDbgString8 ConditionExpression;
    }

    [StructLayout(LayoutKind.Sequential)]
    internal struct RadDbgDmnEvent
    {
        internal uint Kind;
        internal uint ErrorKind;
        internal uint MemoryKind;
        internal uint ExceptionKind;
        internal ulong Process;
        internal ulong ParentProcess;
        internal ulong Thread;
        internal ulong Module;
        internal uint SystemProcessId;
        internal uint SystemThreadId;
        internal uint Arch;
        internal ulong Address;
        internal ulong Size;
        internal RadDbgString8 String;
        internal uint Code;
        internal uint Flags;
        internal ulong InstructionPointer;
        internal ulong StackPointer;
        internal ulong UserData;
        internal uint ExceptionRepeated;
        internal uint TlsModel;
        internal IntPtr ModuleInfo;
        internal ulong TlsRootVaddr;
    }

    [StructLayout(LayoutKind.Sequential)]
    internal struct RadDbgNativeEvent
    {
        internal RadDbgDmnEvent Raw;
        internal ulong ProcessHandle;
        internal ulong ParentProcessHandle;
        internal ulong ThreadHandle;
        internal ulong ModuleHandle;
        internal ulong ProcessStateEpoch;
        internal uint SystemProcessId;
        internal uint SystemThreadId;
        internal uint ThreadCreated;
        internal uint SessionProcessCount;
        internal ulong BreakpointId;
    }

    [StructLayout(LayoutKind.Sequential)]
    internal struct RadDbgAd7Event
    {
        internal RadDbgNativeEvent Event;
        internal ulong Sequence;
        internal ulong TextSize;
        internal RadDbgAd7EventKind Kind;
        internal uint Attributes;
        internal RadDbgAd7StopReason StopReason;
    }

    [StructLayout(LayoutKind.Sequential)]
    internal struct RadDbgThreadDesc
    {
        internal ulong ThreadHandle;
        internal ulong ProcessHandle;
        internal ulong ProcessStateEpoch;
        internal uint SystemThreadId;
        internal uint State;
    }

    [StructLayout(LayoutKind.Sequential)]
    internal struct RadDbgAd7ProgramDesc
    {
        internal ulong ProcessHandle;
        internal ulong ParentProcessHandle;
        internal uint SystemProcessId;
        internal uint State;
        internal ulong ProgramNameOffset;
        internal ulong ProgramNameSize;
        internal ulong HostNameOffset;
        internal ulong HostNameSize;
        internal ulong EngineNameOffset;
        internal ulong EngineNameSize;
        internal ulong EngineIdOffset;
        internal ulong EngineIdSize;
    }

    [StructLayout(LayoutKind.Sequential)]
    internal struct RadDbgModuleDesc
    {
        internal ulong ModuleHandle;
        internal ulong ProcessHandle;
        internal ulong BaseAddress;
        internal ulong Size;
        internal uint SymbolState;
        internal uint Reserved;
    }

    internal sealed class RadDbgBridge : IDisposable
    {
        private const string RadDbgBridgeFileName      = "RadDbg.dll";
        private const int    HResultInsufficientBuffer = unchecked((int)0x8007007A);
        private const int    HResultNoInterface        = unchecked((int)0x80004002);
        private const int    HResultOutOfMemory        = unchecked((int)0x8007000E);
        private const int    NativeVariantSize         = 16;

        static RadDbgBridge()
        {
            string? assemblyDirectory = Path.GetDirectoryName(typeof(RadDbgBridge).Assembly.Location);
            if (string.IsNullOrEmpty(assemblyDirectory))
            {
                throw new InvalidOperationException("Could not determine the RAD Visual Studio Extension Directory");
            }

            string bridgePath = Path.Combine(assemblyDirectory, RadDbgBridgeFileName);
            if (!File.Exists(bridgePath))
            {
                throw new FileNotFoundException($"The RAD Debugger bridge was not found: '{bridgePath}'.", bridgePath);
            }

            IntPtr bridgeDLL = Kernel32.LoadLibrary(bridgePath);
            if (bridgeDLL == IntPtr.Zero)
            {
                throw new DllNotFoundException($"Could not load the RAD Debugger bridge: '{bridgePath}' (Win32 error {Marshal.GetLastWin32Error()}).");
            }
        }

        internal static int LaunchSuspended(string exe, string cmdLine, string workingDirectory, out RadDbgBridge? session, out uint systemProcessId)
        {
            session = null;
            systemProcessId = 0;
            int result = Bridge.IDebugEngineLaunch2_LaunchSuspended(exe, cmdLine, workingDirectory, out IntPtr sessionPtr, out systemProcessId);
            if (result == VSConstants.S_OK)
            {
                session = new RadDbgBridge(sessionPtr);
            }
            return result;
        }

        private IntPtr sessionPtr;
        private readonly object threadLock = new object();
        private readonly Dictionary<ulong, RadDbgThread> threads = new Dictionary<ulong, RadDbgThread>();

        private RadDbgBridge(IntPtr sessionPtr)
        {
            this.sessionPtr = sessionPtr;
        }

        internal int GetEventAttributes(ulong sequence, out uint attributes)
        {
            return Bridge.IDebugEvent2_GetAttributes(this.sessionPtr, sequence, out attributes);
        }

        internal int CanTerminateLaunchProcess(ulong processHandle)
        {
            return Bridge.IDebugEngineLaunch2_CanTerminateProcess(this.sessionPtr, processHandle);
        }

        internal int ResumeLaunchProcess(ulong processHandle)
        {
            return Bridge.IDebugEngineLaunch2_ResumeProcess(this.sessionPtr, processHandle);
        }

        internal int TerminateLaunchProcess(ulong processHandle)
        {
            return Bridge.IDebugEngineLaunch2_TerminateProcess(this.sessionPtr, processHandle);
        }

        internal int Attach(IDebugProgram2[] programs, IDebugProgramNode2[] programNodes, uint programCount, IDebugEventCallback2 callback, enum_ATTACH_REASON reason)
        {
            using ComPointerArray nativePrograms = new ComPointerArray(programs, programCount);
            using ComPointerArray nativeProgramNodes = new ComPointerArray(programNodes, programCount);
            using ComPointer nativeCallback = new ComPointer(callback);
            return Bridge.IDebugEngine2_Attach(this.sessionPtr, nativePrograms.Pointer, nativeProgramNodes.Pointer, programCount, nativeCallback.Pointer, (uint)reason);
        }

        internal int EngineBreak()
        {
            return Bridge.IDebugEngine2_CauseBreak(this.sessionPtr);
        }

        internal int ContinueFromSynchronousEvent()
        {
            return Bridge.IDebugEngine2_ContinueFromSynchronousEvent(this.sessionPtr);
        }

        internal int CreatePendingBreakpoint(ref RadDbgBreakpointSpec request, out ulong breakpointId)
        {
            return Bridge.IDebugEngine2_CreatePendingBreakpoint(this.sessionPtr, ref request, out breakpointId);
        }

        internal int CreateAddressBreakpoint(ulong address, bool enabled, RadDbgAddressBreakpointMode mode, out ulong breakpointId)
        {
            return Bridge.IDebugEngine2_CreateAddressBreakpoint(this.sessionPtr, address, enabled ? 1u : 0u, (uint)mode, out breakpointId);
        }

        internal int CreateSourceBreakpoint(string sourcePath, uint line, uint column, out ulong breakpointId)
        {
            return Bridge.IDebugEngine2_CreateBreakpoint(this.sessionPtr, sourcePath, line, column, out breakpointId);
        }

        internal int SetBreakpointEnabled(ulong breakpointId, bool enabled)
        {
            return Bridge.IDebugEngine2_SetBreakpointEnabled(this.sessionPtr, breakpointId, enabled ? 1u : 0u);
        }

        internal int DeleteBreakpoint(ulong breakpointId)
        {
            return Bridge.IDebugEngine2_DeleteBreakpoint(this.sessionPtr, breakpointId);
        }

        internal int DestroyProgram(IDebugProgram2 program)
        {
            using ComPointer nativeProgram = new ComPointer(program);
            return Bridge.IDebugEngine2_DestroyProgram(this.sessionPtr, nativeProgram.Pointer);
        }

        internal int EnumProgramDescriptors(out RadDbgAd7ProgramDesc[] descriptors, out byte[] text)
        {
            descriptors = Array.Empty<RadDbgAd7ProgramDesc>();
            text = Array.Empty<byte>();
            int result = Bridge.IDebugEngine2_EnumPrograms(this.sessionPtr, null, 0, out ulong count, IntPtr.Zero, 0, out ulong textSize);
            while (result == VSConstants.S_OK || result == HResultInsufficientBuffer)
            {
                if (count > int.MaxValue || textSize > int.MaxValue)
                {
                    return HResultOutOfMemory;
                }

                RadDbgAd7ProgramDesc[] snapshot = new RadDbgAd7ProgramDesc[(int)count];
                byte[] textSnapshot = new byte[(int)textSize];
                GCHandle textHandle = default;
                try
                {
                    IntPtr textBuffer = IntPtr.Zero;
                    if (textSnapshot.Length != 0)
                    {
                        textHandle = GCHandle.Alloc(textSnapshot, GCHandleType.Pinned);
                        textBuffer = textHandle.AddrOfPinnedObject();
                    }
                    result = Bridge.IDebugEngine2_EnumPrograms(this.sessionPtr, snapshot, (ulong)snapshot.LongLength, out count, textBuffer, (ulong)textSnapshot.LongLength, out textSize);
                }
                finally
                {
                    if (textHandle.IsAllocated)
                    {
                        textHandle.Free();
                    }
                }

                if (result == VSConstants.S_OK)
                {
                    if (count != (ulong)snapshot.LongLength)
                    {
                        Array.Resize(ref snapshot, checked((int)count));
                    }
                    if (textSize != (ulong)textSnapshot.LongLength)
                    {
                        Array.Resize(ref textSnapshot, checked((int)textSize));
                    }
                    descriptors = snapshot;
                    text = textSnapshot;
                    return VSConstants.S_OK;
                }
            }
            return result;
        }

        internal int GetEngineId(out Guid engineId)
        {
            return Bridge.IDebugEngine2_GetEngineId(this.sessionPtr, out engineId);
        }

        internal int RemoveAllSetExceptions(ref Guid exceptionType)
        {
            return Bridge.IDebugEngine2_RemoveAllSetExceptions(this.sessionPtr, ref exceptionType);
        }

        internal int RemoveSetException(ref EXCEPTION_INFO exceptionInfo)
        {
            return Bridge.IDebugEngine2_RemoveSetException(this.sessionPtr, ref exceptionInfo);
        }

        internal int SetException(ref EXCEPTION_INFO exceptionInfo)
        {
            return Bridge.IDebugEngine2_SetException(this.sessionPtr, ref exceptionInfo);
        }

        internal int SetLocale(ushort languageId)
        {
            return Bridge.IDebugEngine2_SetLocale(this.sessionPtr, languageId);
        }

        internal int SetMetric(string metric, object? value)
        {
            IntPtr nativeVariant = Marshal.AllocCoTaskMem(NativeVariantSize);
            try
            {
                Marshal.GetNativeVariantForObject(value, nativeVariant);
                return Bridge.IDebugEngine2_SetMetric(this.sessionPtr, metric, nativeVariant);
            }
            finally
            {
                OleAut32.VariantClear(nativeVariant);
                Marshal.FreeCoTaskMem(nativeVariant);
            }
        }

        internal int SetRegistryRoot(string registryRoot)
        {
            return Bridge.IDebugEngine2_SetRegistryRoot(this.sessionPtr, registryRoot);
        }

        internal int ProgramAttach(IDebugEventCallback2 callback)
        {
            using ComPointer nativeCallback = new ComPointer(callback);
            return Bridge.IDebugProgram2_Attach(this.sessionPtr, nativeCallback.Pointer);
        }

        internal int CanDetachProgram()
        {
            return Bridge.IDebugProgram2_CanDetach(this.sessionPtr);
        }

        internal int CopyModules(out RadDbgModuleDesc[] modules)
        {
            modules = Array.Empty<RadDbgModuleDesc>();
            int result = Bridge.IDebugProgram2_CopyModules(this.sessionPtr, null, 0, out ulong count);
            while (result == VSConstants.S_OK || result == HResultInsufficientBuffer)
            {
                if (count == 0)
                {
                    return VSConstants.S_OK;
                }
                if (count > int.MaxValue)
                {
                    return HResultOutOfMemory;
                }

                RadDbgModuleDesc[] snapshot = new RadDbgModuleDesc[(int)count];
                result = Bridge.IDebugProgram2_CopyModules(this.sessionPtr, snapshot, (ulong)snapshot.LongLength, out count);
                if (result == VSConstants.S_OK)
                {
                    if (count != (ulong)snapshot.LongLength)
                    {
                        Array.Resize(ref snapshot, checked((int)count));
                    }
                    modules = snapshot;
                    return VSConstants.S_OK;
                }
            }
            return result;
        }

        internal int DetachProgram()
        {
            return Bridge.IDebugProgram2_Detach(this.sessionPtr);
        }

        internal int EnumCodeContexts(IDebugDocumentPosition2 documentPosition, out IEnumDebugCodeContexts2 contexts)
        {
            using ComPointer nativeDocumentPosition = new ComPointer(documentPosition);
            int result = Bridge.IDebugProgram2_EnumCodeContexts(this.sessionPtr, nativeDocumentPosition.Pointer, out IntPtr nativeContexts);
            return GetNativeObject(result, nativeContexts, out contexts);
        }

        internal int EnumCodePaths(string hint, IDebugCodeContext2 startContext, IDebugStackFrame2 stackFrame, int source, out IEnumCodePaths2 paths, out IDebugCodeContext2 safetyContext)
        {
            paths = null!;
            safetyContext = null!;
            using ComPointer nativeStartContext = new ComPointer(startContext);
            using ComPointer nativeStackFrame = new ComPointer(stackFrame);
            int result = Bridge.IDebugProgram2_EnumCodePaths(this.sessionPtr, hint, nativeStartContext.Pointer, nativeStackFrame.Pointer, source, out IntPtr nativePaths, out IntPtr nativeSafetyContext);
            int pathsResult = GetNativeObject(result, nativePaths, out paths);
            if (pathsResult != VSConstants.S_OK)
            {
                ReleaseIfNonZero(nativeSafetyContext);
                return pathsResult;
            }
            return GetNativeObject(VSConstants.S_OK, nativeSafetyContext, out safetyContext);
        }

        internal int EnumModules(out RadDbgModuleDesc[] modules)
        {
            modules = Array.Empty<RadDbgModuleDesc>();
            int result = Bridge.IDebugProgram2_EnumModules(this.sessionPtr, null, 0, out ulong count);
            while (result == VSConstants.S_OK || result == HResultInsufficientBuffer)
            {
                if (count == 0)
                {
                    return VSConstants.S_OK;
                }
                if (count > int.MaxValue)
                {
                    return HResultOutOfMemory;
                }

                RadDbgModuleDesc[] snapshot = new RadDbgModuleDesc[(int)count];
                result = Bridge.IDebugProgram2_EnumModules(this.sessionPtr, snapshot, (ulong)snapshot.LongLength, out count);
                if (result == VSConstants.S_OK)
                {
                    if (count != (ulong)snapshot.LongLength)
                    {
                        Array.Resize(ref snapshot, checked((int)count));
                    }
                    modules = snapshot;
                    return VSConstants.S_OK;
                }
            }
            return result;
        }

        internal int EnumThreads(out RadDbgThreadDesc[] threads)
        {
            threads = Array.Empty<RadDbgThreadDesc>();
            int result = Bridge.IDebugProgram2_EnumThreads(this.sessionPtr, null, 0, out ulong count);
            while (result == VSConstants.S_OK || result == HResultInsufficientBuffer)
            {
                if (count == 0)
                {
                    return VSConstants.S_OK;
                }
                if (count > int.MaxValue)
                {
                    return HResultOutOfMemory;
                }

                RadDbgThreadDesc[] snapshot = new RadDbgThreadDesc[(int)count];
                result = Bridge.IDebugProgram2_EnumThreads(this.sessionPtr, snapshot, (ulong)snapshot.LongLength, out count);
                if (result == VSConstants.S_OK)
                {
                    if (count != (ulong)snapshot.LongLength)
                    {
                        Array.Resize(ref snapshot, checked((int)count));
                    }
                    threads = snapshot;
                    return VSConstants.S_OK;
                }
            }
            return result;
        }

        internal int ExecuteProgram()
        {
            return Bridge.IDebugProgram2_Execute(this.sessionPtr);
        }

        internal int GetDebugProperty(out IDebugProperty2 property)
        {
            int result = Bridge.IDebugProgram2_GetDebugProperty(this.sessionPtr, out IntPtr nativeProperty);
            return GetNativeObject(result, nativeProperty, out property);
        }

        internal int GetDisassemblyStream(enum_DISASSEMBLY_STREAM_SCOPE scope, IDebugCodeContext2 codeContext, out IDebugDisassemblyStream2 stream)
        {
            using ComPointer nativeCodeContext = new ComPointer(codeContext);
            int result = Bridge.IDebugProgram2_GetDisassemblyStream(this.sessionPtr, (uint)scope, nativeCodeContext.Pointer, out IntPtr nativeStream);
            return GetNativeObject(result, nativeStream, out stream);
        }

        internal int GetEncUpdate(out object update)
        {
            int result = Bridge.IDebugProgram2_GetENCUpdate(this.sessionPtr, out IntPtr nativeUpdate);
            return GetNativeObject(result, nativeUpdate, out update);
        }

        internal int GetNativeMemoryBytes(out IDebugMemoryBytes2 memoryBytes)
        {
            int result = Bridge.IDebugProgram2_GetMemoryBytes(this.sessionPtr, out IntPtr nativeMemoryBytes);
            return GetNativeObject(result, nativeMemoryBytes, out memoryBytes);
        }

        internal int GetProgramProcessHandle(out ulong processHandle)
        {
            return Bridge.IDebugProgram2_GetProcess(this.sessionPtr, out processHandle);
        }

        internal int GetProgramId(out Guid programId)
        {
            return Bridge.IDebugProgram2_GetProgramId(this.sessionPtr, out programId);
        }

        internal int ResolveSourcePosition(string sourcePath, uint line, uint column)
        {
            return Bridge.IDebugProgram2_ResolveSourcePosition(this.sessionPtr, sourcePath, line, column);
        }

        internal int Continue(ulong threadHandle)
        {
            return Bridge.IDebugProgram2_Continue(this.sessionPtr, threadHandle);
        }

        internal int Break()
        {
            return Bridge.IDebugProgram2_CauseBreak(this.sessionPtr);
        }

        internal int ProgramStep(ulong threadHandle, uint stepKind, uint stepUnit)
        {
            return Bridge.IDebugProgram2_Step(this.sessionPtr, threadHandle, stepKind, stepUnit);
        }

        internal int TerminateProgram()
        {
            return Bridge.IDebugProgram2_Terminate(this.sessionPtr);
        }

        internal int WriteDump(uint dumpType, string dumpUrl)
        {
            return Bridge.IDebugProgram2_WriteDump(this.sessionPtr, dumpType, dumpUrl);
        }

        internal int ProcessAttach(ulong processHandle, IDebugEventCallback2 callback, Guid[] engineIds, int[] attachResults)
        {
            using ComPointer nativeCallback = new ComPointer(callback);
            GCHandle engineIdsHandle = default;
            GCHandle attachResultsHandle = default;
            try
            {
                IntPtr engineIdsPointer = IntPtr.Zero;
                IntPtr attachResultsPointer = IntPtr.Zero;
                if (engineIds.Length != 0)
                {
                    engineIdsHandle = GCHandle.Alloc(engineIds, GCHandleType.Pinned);
                    engineIdsPointer = engineIdsHandle.AddrOfPinnedObject();
                }
                if (attachResults.Length != 0)
                {
                    attachResultsHandle = GCHandle.Alloc(attachResults, GCHandleType.Pinned);
                    attachResultsPointer = attachResultsHandle.AddrOfPinnedObject();
                }
                return Bridge.IDebugProcess2_Attach(this.sessionPtr, processHandle, nativeCallback.Pointer, engineIdsPointer, (uint)engineIds.Length, attachResultsPointer);
            }
            finally
            {
                if (attachResultsHandle.IsAllocated)
                {
                    attachResultsHandle.Free();
                }
                if (engineIdsHandle.IsAllocated)
                {
                    engineIdsHandle.Free();
                }
            }
        }

        internal int CanDetachProcess(ulong processHandle)
        {
            return Bridge.IDebugProcess2_CanDetach(this.sessionPtr, processHandle);
        }

        internal int BreakProcess(ulong processHandle)
        {
            return Bridge.IDebugProcess2_CauseBreak(this.sessionPtr, processHandle);
        }

        internal int DetachProcess(ulong processHandle)
        {
            return Bridge.IDebugProcess2_Detach(this.sessionPtr, processHandle);
        }

        internal int EnumProcessProgramDescriptors(ulong processHandle, out RadDbgAd7ProgramDesc[] descriptors, out byte[] text)
        {
            descriptors = Array.Empty<RadDbgAd7ProgramDesc>();
            text = Array.Empty<byte>();
            int result = Bridge.IDebugProcess2_EnumPrograms(this.sessionPtr, processHandle, null, 0, out ulong count, IntPtr.Zero, 0, out ulong textSize);
            while (result == VSConstants.S_OK || result == HResultInsufficientBuffer)
            {
                if (count > int.MaxValue || textSize > int.MaxValue)
                {
                    return HResultOutOfMemory;
                }

                RadDbgAd7ProgramDesc[] snapshot = new RadDbgAd7ProgramDesc[(int)count];
                byte[] textSnapshot = new byte[(int)textSize];
                GCHandle textHandle = default;
                try
                {
                    IntPtr textBuffer = IntPtr.Zero;
                    if (textSnapshot.Length != 0)
                    {
                        textHandle = GCHandle.Alloc(textSnapshot, GCHandleType.Pinned);
                        textBuffer = textHandle.AddrOfPinnedObject();
                    }
                    result = Bridge.IDebugProcess2_EnumPrograms(this.sessionPtr, processHandle, snapshot, (ulong)snapshot.LongLength, out count, textBuffer, (ulong)textSnapshot.LongLength, out textSize);
                }
                finally
                {
                    if (textHandle.IsAllocated)
                    {
                        textHandle.Free();
                    }
                }

                if (result == VSConstants.S_OK)
                {
                    if (count != (ulong)snapshot.LongLength)
                    {
                        Array.Resize(ref snapshot, checked((int)count));
                    }
                    if (textSize != (ulong)textSnapshot.LongLength)
                    {
                        Array.Resize(ref textSnapshot, checked((int)textSize));
                    }
                    descriptors = snapshot;
                    text = textSnapshot;
                    return VSConstants.S_OK;
                }
            }
            return result;
        }

        internal int EnumProcessThreads(ulong processHandle, out RadDbgThreadDesc[] threads)
        {
            threads = Array.Empty<RadDbgThreadDesc>();
            int result = Bridge.IDebugProcess2_EnumThreads(this.sessionPtr, processHandle, null, 0, out ulong count);
            while (result == VSConstants.S_OK || result == HResultInsufficientBuffer)
            {
                if (count == 0)
                {
                    return VSConstants.S_OK;
                }
                if (count > int.MaxValue)
                {
                    return HResultOutOfMemory;
                }

                RadDbgThreadDesc[] snapshot = new RadDbgThreadDesc[(int)count];
                result = Bridge.IDebugProcess2_EnumThreads(this.sessionPtr, processHandle, snapshot, (ulong)snapshot.LongLength, out count);
                if (result == VSConstants.S_OK)
                {
                    if (count != (ulong)snapshot.LongLength)
                    {
                        Array.Resize(ref snapshot, checked((int)count));
                    }
                    threads = snapshot;
                    return VSConstants.S_OK;
                }
            }
            return result;
        }

        internal int GetAttachedSessionName(ulong processHandle, out string name)
        {
            return GetUtf8String((IntPtr buffer, ulong bufferSize, out ulong textSize) => Bridge.IDebugProcess2_GetAttachedSessionName(this.sessionPtr, processHandle, buffer, bufferSize, out textSize), out name);
        }

        internal int GetProcessInfo(ulong processHandle, uint fields, IntPtr processInfo)
        {
            return Bridge.IDebugProcess2_GetInfo(this.sessionPtr, processHandle, fields, processInfo);
        }

        internal int GetProcessName(ulong processHandle, uint nameKind, out string name)
        {
            return GetUtf8String((IntPtr buffer, ulong bufferSize, out ulong textSize) => Bridge.IDebugProcess2_GetName(this.sessionPtr, processHandle, nameKind, buffer, bufferSize, out textSize), out name);
        }

        internal int GetPhysicalProcessId(ulong processHandle, out uint systemProcessId)
        {
            return Bridge.IDebugProcess2_GetPhysicalProcessId(this.sessionPtr, processHandle, out systemProcessId);
        }

        internal int GetProcessPort(ulong processHandle, out IDebugPort2 port)
        {
            int result = Bridge.IDebugProcess2_GetPort(this.sessionPtr, processHandle, out IntPtr nativePort);
            return GetNativeObject(result, nativePort, out port);
        }

        internal int GetProcessId(ulong processHandle, out Guid processId)
        {
            return Bridge.IDebugProcess2_GetProcessId(this.sessionPtr, processHandle, out processId);
        }

        internal int GetProcessServer(ulong processHandle, out IDebugCoreServer2 server)
        {
            int result = Bridge.IDebugProcess2_GetServer(this.sessionPtr, processHandle, out IntPtr nativeServer);
            return GetNativeObject(result, nativeServer, out server);
        }

        internal int Terminate(ulong processHandle)
        {
            return Bridge.IDebugProcess2_Terminate(this.sessionPtr, processHandle);
        }

        internal int CanSetNextStatement(ulong threadHandle, IDebugStackFrame2 stackFrame, IDebugCodeContext2 codeContext)
        {
            using ComPointer nativeStackFrame = new ComPointer(stackFrame);
            using ComPointer nativeCodeContext = new ComPointer(codeContext);
            return Bridge.IDebugThread2_CanSetNextStatement(this.sessionPtr, threadHandle, nativeStackFrame.Pointer, nativeCodeContext.Pointer);
        }

        internal int EnumFrameInfo(ulong threadHandle, uint fieldSpec, uint radix, out IEnumDebugFrameInfo2 frames)
        {
            int result = Bridge.IDebugThread2_EnumFrameInfo(this.sessionPtr, threadHandle, fieldSpec, radix, out IntPtr nativeFrames);
            return GetNativeObject(result, nativeFrames, out frames);
        }

        internal int Step(ulong threadHandle, uint stepKind, uint stepUnit)
        {
            return Bridge.IDebugThread2_Step(this.sessionPtr, threadHandle, stepKind, stepUnit);
        }

        internal int GetLogicalThread(ulong threadHandle, IDebugStackFrame2 stackFrame, out IDebugLogicalThread2 logicalThread)
        {
            using ComPointer nativeStackFrame = new ComPointer(stackFrame);
            int result = Bridge.IDebugThread2_GetLogicalThread(this.sessionPtr, threadHandle, nativeStackFrame.Pointer, out IntPtr nativeLogicalThread);
            return GetNativeObject(result, nativeLogicalThread, out logicalThread);
        }

        internal int GetThreadProgram(ulong threadHandle, out IDebugProgram2 program)
        {
            int result = Bridge.IDebugThread2_GetProgram(this.sessionPtr, threadHandle, out IntPtr nativeProgram);
            return GetNativeObject(result, nativeProgram, out program);
        }

        internal int GetThreadId(ulong threadHandle, out uint threadId)
        {
            return Bridge.IDebugThread2_GetThreadId(this.sessionPtr, threadHandle, out threadId);
        }

        internal int GetThreadProperties(ulong threadHandle, uint fields, ref THREADPROPERTIES properties)
        {
            return Bridge.IDebugThread2_GetThreadProperties(this.sessionPtr, threadHandle, fields, ref properties);
        }

        internal int ResumeThread(ulong threadHandle, out uint suspendCount)
        {
            return Bridge.IDebugThread2_Resume(this.sessionPtr, threadHandle, out suspendCount);
        }

        internal int SetNextStatement(ulong threadHandle, IDebugStackFrame2 stackFrame, IDebugCodeContext2 codeContext)
        {
            using ComPointer nativeStackFrame = new ComPointer(stackFrame);
            using ComPointer nativeCodeContext = new ComPointer(codeContext);
            return Bridge.IDebugThread2_SetNextStatement(this.sessionPtr, threadHandle, nativeStackFrame.Pointer, nativeCodeContext.Pointer);
        }

        internal int SetThreadName(ulong threadHandle, string name)
        {
            return Bridge.IDebugThread2_SetThreadName(this.sessionPtr, threadHandle, name);
        }

        internal int SuspendThread(ulong threadHandle, out uint suspendCount)
        {
            return Bridge.IDebugThread2_Suspend(this.sessionPtr, threadHandle, out suspendCount);
        }

        internal int SetInstructionPointer(ulong threadHandle, ulong address)
        {
            return Bridge.IDebugThread2_SetInstructionPointer(this.sessionPtr, threadHandle, address);
        }

        internal int GetMemorySize(ulong processHandle, out ulong size)
        {
            return Bridge.IDebugMemoryBytes2_GetSize(this.sessionPtr, processHandle, out size);
        }

        internal int ReadMemoryAt(ulong processHandle, ulong address, uint byteCount, byte[] buffer, out uint read, out uint unreadable)
        {
            read = 0;
            unreadable = 0;
            if ((ulong)buffer.LongLength < byteCount)
            {
                return VSConstants.E_INVALIDARG;
            }

            GCHandle bufferHandle = GCHandle.Alloc(buffer, GCHandleType.Pinned);
            try
            {
                return Bridge.IDebugMemoryBytes2_ReadAt(this.sessionPtr, processHandle, address, byteCount, bufferHandle.AddrOfPinnedObject(), out read, out unreadable);
            }
            finally
            {
                bufferHandle.Free();
            }
        }

        internal int WriteMemoryAt(ulong processHandle, ulong address, uint byteCount, byte[] buffer)
        {
            if ((ulong)buffer.LongLength < byteCount)
            {
                return VSConstants.E_INVALIDARG;
            }

            GCHandle bufferHandle = GCHandle.Alloc(buffer, GCHandleType.Pinned);
            try
            {
                return Bridge.IDebugMemoryBytes2_WriteAt(this.sessionPtr, processHandle, address, byteCount, bufferHandle.AddrOfPinnedObject());
            }
            finally
            {
                bufferHandle.Free();
            }
        }

        internal int ReadMemory(ulong processHandle, ulong address, byte[] buffer, out ulong size)
        {
            size = 0;
            GCHandle bufferHandle = GCHandle.Alloc(buffer, GCHandleType.Pinned);
            try
            {
                return Bridge.IDebugMemoryBytes2_Read(this.sessionPtr, processHandle, address, bufferHandle.AddrOfPinnedObject(), (ulong)buffer.LongLength, out size);
            }
            finally
            {
                bufferHandle.Free();
            }
        }

        internal int AcknowledgeEvent(ulong sequence)
        {
            return Bridge.IDebugEvent2_Acknowledge(this.sessionPtr, sequence);
        }

        internal int WaitEvent(out RadDbgAd7Event e, out string text)
        {
            text = string.Empty;

            int result = Bridge.IDebugEvent2_Wait(this.sessionPtr, 100, out e, IntPtr.Zero, 0, out ulong textSize);

            if (textSize == 0 || result != HResultInsufficientBuffer)
            {
                return result;
            }
            if (textSize > int.MaxValue)
            {
                return HResultOutOfMemory;
            }

            byte[] textBytes = new byte[(int)textSize];
            GCHandle pinnedBuffer = GCHandle.Alloc(textBytes, GCHandleType.Pinned);
            try
            {
                result = Bridge.IDebugEvent2_Wait(this.sessionPtr, 0, out e, pinnedBuffer.AddrOfPinnedObject(), (ulong)textBytes.LongLength, out textSize);
            }
            finally
            {
                pinnedBuffer.Free();
            }
            if (result == VSConstants.S_OK)
            {
                text = Encoding.UTF8.GetString(textBytes, 0, checked((int)textSize));
            }
            return result;
        }

        private int CopyThreads(out RadDbgThreadDesc[] threads)
        {
            threads = Array.Empty<RadDbgThreadDesc>();
            int result = Bridge.IDebugProgram2_CopyThreads(this.sessionPtr, null, 0, out ulong count);
            while (result == VSConstants.S_OK || result == HResultInsufficientBuffer)
            {
                if (count == 0)
                {
                    return VSConstants.S_OK;
                }
                if (count > int.MaxValue)
                {
                    return HResultOutOfMemory;
                }

                RadDbgThreadDesc[] snapshot = new RadDbgThreadDesc[(int)count];
                result = Bridge.IDebugProgram2_CopyThreads(this.sessionPtr, snapshot, (ulong)snapshot.LongLength, out count);
                if (result == VSConstants.S_OK)
                {
                    if (count != (ulong)snapshot.LongLength)
                    {
                        Array.Resize(ref snapshot, (int)count);
                    }
                    threads = snapshot;
                    return VSConstants.S_OK;
                }
            }
            return result;
        }

        internal int GetThreadWrappers(RadDbgProgram program, out IDebugThread2[] wrappers)
        {
            wrappers = Array.Empty<IDebugThread2>();
            int result = this.CopyThreads(out RadDbgThreadDesc[] nativeThreads);
            if (result != VSConstants.S_OK)
            {
                return result;
            }

            List<IDebugThread2> resultThreads = new List<IDebugThread2>();
            for (int index = 0; index < nativeThreads.Length; index++)
            {
                RadDbgThreadDesc nativeThread = nativeThreads[index];
                if (nativeThread.ProcessHandle != program.ProcessHandle)
                {
                    continue;
                }
                RadDbgThread? thread = this.GetThread(program, nativeThread.ThreadHandle, nativeThread.SystemThreadId, createIfMissing: true);
                if (thread == null)
                {
                    wrappers = Array.Empty<IDebugThread2>();
                    return VSConstants.E_FAIL;
                }
                resultThreads.Add(thread);
            }
            wrappers = resultThreads.ToArray();
            return VSConstants.S_OK;
        }

        internal int GetProgramDescriptor(out ulong processHandle, out uint systemProcessId, out string programName, out string hostName, out string engineName, out string engineId)
        {
            processHandle = 0;
            systemProcessId = 0;
            programName = string.Empty;
            hostName = string.Empty;
            engineName = string.Empty;
            engineId = string.Empty;

            int result = Bridge.IDebugProgram2_GetDescriptor(this.sessionPtr, out RadDbgAd7ProgramDesc desc, IntPtr.Zero, 0, out ulong textSize);
            if (result != VSConstants.S_OK && result != HResultInsufficientBuffer)
            {
                return result;
            }
            if (textSize > int.MaxValue)
            {
                return HResultOutOfMemory;
            }

            byte[] text = new byte[(int)textSize];
            if (textSize != 0)
            {
                GCHandle pinnedBuffer = GCHandle.Alloc(text, GCHandleType.Pinned);
                try
                {
                    result = Bridge.IDebugProgram2_GetDescriptor(this.sessionPtr, out desc, pinnedBuffer.AddrOfPinnedObject(), (ulong)text.LongLength, out textSize);
                }
                finally
                {
                    pinnedBuffer.Free();
                }
                if (result != VSConstants.S_OK)
                {
                    return result;
                }
            }
            else if (result != VSConstants.S_OK)
            {
                return result;
            }

            if (!TryDecodeUtf8(text, desc.ProgramNameOffset, desc.ProgramNameSize, out programName) ||
                !TryDecodeUtf8(text, desc.HostNameOffset, desc.HostNameSize, out hostName) ||
                !TryDecodeUtf8(text, desc.EngineNameOffset, desc.EngineNameSize, out engineName) ||
                !TryDecodeUtf8(text, desc.EngineIdOffset, desc.EngineIdSize, out engineId))
            {
                return VSConstants.E_INVALIDARG;
            }
            processHandle = desc.ProcessHandle;
            systemProcessId = desc.SystemProcessId;
            return VSConstants.S_OK;
        }

        internal int GetProgramDescriptor(ulong processHandle, out uint systemProcessId, out string programName, out string hostName, out string engineName, out string engineId)
        {
            systemProcessId = 0;
            programName = string.Empty;
            hostName = string.Empty;
            engineName = string.Empty;
            engineId = string.Empty;
            if (processHandle == 0)
            {
                return this.GetProgramDescriptor(out _, out systemProcessId, out programName, out hostName, out engineName, out engineId);
            }

            int result = Bridge.IDebugProgram2_GetDescriptorForProcess(this.sessionPtr, processHandle, out RadDbgAd7ProgramDesc desc, IntPtr.Zero, 0, out ulong textSize);
            if (result != VSConstants.S_OK && result != HResultInsufficientBuffer)
            {
                return result;
            }
            if (textSize > int.MaxValue)
            {
                return HResultOutOfMemory;
            }

            byte[] text = new byte[(int)textSize];
            if (textSize != 0)
            {
                GCHandle pinnedBuffer = GCHandle.Alloc(text, GCHandleType.Pinned);
                try
                {
                    result = Bridge.IDebugProgram2_GetDescriptorForProcess(this.sessionPtr, processHandle, out desc, pinnedBuffer.AddrOfPinnedObject(), (ulong)text.LongLength, out textSize);
                }
                finally
                {
                    pinnedBuffer.Free();
                }
                if (result != VSConstants.S_OK)
                {
                    return result;
                }
            }
            else if (result != VSConstants.S_OK)
            {
                return result;
            }

            if (!TryDecodeUtf8(text, desc.ProgramNameOffset, desc.ProgramNameSize, out programName) ||
                !TryDecodeUtf8(text, desc.HostNameOffset, desc.HostNameSize, out hostName) ||
                !TryDecodeUtf8(text, desc.EngineNameOffset, desc.EngineNameSize, out engineName) ||
                !TryDecodeUtf8(text, desc.EngineIdOffset, desc.EngineIdSize, out engineId))
            {
                return VSConstants.E_INVALIDARG;
            }
            systemProcessId = desc.SystemProcessId;
            return VSConstants.S_OK;
        }

        internal int GetProgramEngineInfo(out string engineName, out Guid engineId)
        {
            Guid nativeEngineId = Guid.Empty;
            int result = GetUtf8String((IntPtr buffer, ulong bufferSize, out ulong textSize) => Bridge.IDebugProgram2_GetEngineInfo(this.sessionPtr, buffer, bufferSize, out textSize, out nativeEngineId), out engineName);
            engineId = nativeEngineId;
            return result;
        }

        internal int GetProgramName(out string name)
        {
            return GetUtf8String((IntPtr buffer, ulong bufferSize, out ulong textSize) => Bridge.IDebugProgram2_GetName(this.sessionPtr, buffer, bufferSize, out textSize), out name);
        }

        internal int GetThreadName(ulong threadHandle, out string name)
        {
            return GetUtf8String((IntPtr buffer, ulong bufferSize, out ulong textSize) => Bridge.IDebugThread2_GetName(this.sessionPtr, threadHandle, buffer, bufferSize, out textSize), out name);
        }

        internal int GetTopFrame(ulong threadHandle, out RadDbgFrameInfo frame, out string sourcePath)
        {
            sourcePath = string.Empty;
            int result = Bridge.IDebugThread2_GetTopFrame(this.sessionPtr, threadHandle, out frame, IntPtr.Zero, 0, out ulong sourcePathSize);
            if (result != VSConstants.S_OK || sourcePathSize == 0)
            {
                return result;
            }
            if (sourcePathSize > int.MaxValue)
            {
                return HResultOutOfMemory;
            }

            byte[] sourcePathBytes = new byte[(int)sourcePathSize];
            GCHandle pinnedBuffer = GCHandle.Alloc(sourcePathBytes, GCHandleType.Pinned);
            try
            {
                result = Bridge.IDebugThread2_GetTopFrame(this.sessionPtr, threadHandle, out frame, pinnedBuffer.AddrOfPinnedObject(), (ulong)sourcePathBytes.LongLength, out sourcePathSize);
            }
            finally
            {
                pinnedBuffer.Free();
            }
            if (result == VSConstants.S_OK)
            {
                sourcePath = Encoding.UTF8.GetString(sourcePathBytes, 0, checked((int)sourcePathSize));
            }
            return result;
        }

        private delegate int Utf8BufferCall(IntPtr buffer, ulong bufferSize, out ulong textSize);

        private static int GetUtf8String(Utf8BufferCall call, out string value)
        {
            value = string.Empty;
            int result = call(IntPtr.Zero, 0, out ulong textSize);
            while (result == VSConstants.S_OK || result == HResultInsufficientBuffer)
            {
                if (textSize == 0)
                {
                    return VSConstants.S_OK;
                }
                if (textSize > int.MaxValue)
                {
                    return HResultOutOfMemory;
                }

                byte[] text = new byte[(int)textSize];
                GCHandle pinnedBuffer = GCHandle.Alloc(text, GCHandleType.Pinned);
                try
                {
                    result = call(pinnedBuffer.AddrOfPinnedObject(), (ulong)text.LongLength, out textSize);
                }
                finally
                {
                    pinnedBuffer.Free();
                }
                if (result == VSConstants.S_OK)
                {
                    value = Encoding.UTF8.GetString(text, 0, checked((int)textSize));
                    return VSConstants.S_OK;
                }
            }
            return result;
        }

        private static int GetNativeObject<T>(int result, IntPtr nativeObject, out T value) where T : class
        {
            value = null!;
            if (result != VSConstants.S_OK)
            {
                ReleaseIfNonZero(nativeObject);
                return result;
            }
            if (nativeObject == IntPtr.Zero)
            {
                return VSConstants.E_FAIL;
            }

            try
            {
                object instance = Marshal.GetObjectForIUnknown(nativeObject);
                if (instance is T typed)
                {
                    value = typed;
                    return VSConstants.S_OK;
                }
                return HResultNoInterface;
            }
            finally
            {
                Marshal.Release(nativeObject);
            }
        }

        private static void ReleaseIfNonZero(IntPtr nativeObject)
        {
            if (nativeObject != IntPtr.Zero)
            {
                Marshal.Release(nativeObject);
            }
        }

        private sealed class ComPointer : IDisposable
        {
            internal ComPointer(object? instance)
            {
                this.Pointer = instance == null ? IntPtr.Zero : Marshal.GetIUnknownForObject(instance);
            }

            internal IntPtr Pointer { get; private set; }

            public void Dispose()
            {
                if (this.Pointer != IntPtr.Zero)
                {
                    Marshal.Release(this.Pointer);
                    this.Pointer = IntPtr.Zero;
                }
            }
        }

        private sealed class ComPointerArray : IDisposable
        {
            private readonly IntPtr[] pointers;
            private GCHandle pinnedPointers;

            internal ComPointerArray(object[] instances, uint count)
            {
                this.pointers = count == 0 ? Array.Empty<IntPtr>() : new IntPtr[(int)count];
                try
                {
                    for (int index = 0; index < this.pointers.Length; index++)
                    {
                        if (instances[index] != null)
                        {
                            this.pointers[index] = Marshal.GetIUnknownForObject(instances[index]);
                        }
                    }
                    if (this.pointers.Length != 0)
                    {
                        this.pinnedPointers = GCHandle.Alloc(this.pointers, GCHandleType.Pinned);
                    }
                }
                catch
                {
                    this.Dispose();
                    throw;
                }
            }

            internal IntPtr Pointer => this.pinnedPointers.IsAllocated ? this.pinnedPointers.AddrOfPinnedObject() : IntPtr.Zero;

            public void Dispose()
            {
                if (this.pinnedPointers.IsAllocated)
                {
                    this.pinnedPointers.Free();
                }
                for (int index = 0; index < this.pointers.Length; index++)
                {
                    if (this.pointers[index] != IntPtr.Zero)
                    {
                        Marshal.Release(this.pointers[index]);
                        this.pointers[index] = IntPtr.Zero;
                    }
                }
            }
        }

        private static bool TryDecodeUtf8(byte[] text, ulong offset, ulong size, out string value)
        {
            value = string.Empty;
            if (offset > (ulong)text.LongLength || size > (ulong)text.LongLength - offset)
            {
                return false;
            }
            value = Encoding.UTF8.GetString(text, (int)offset, (int)size);
            return true;
        }

        private bool DispatchNativeEvent(Func<RadDbgAd7Event, RadDbgProgram?> getProgram, Func<ulong, IDebugBoundBreakpoint2?> getBreakpoint, Action<IDebugProgram2?, IDebugEvent2, Guid, IDebugThread2?> dispatch, Action<RadDbgAd7Event> programDestroyed, RadDbgAd7Event @event, string text)
        {
            RadDbgProgram? program = getProgram(@event);
            switch (@event.Kind)
            {
                case RadDbgAd7EventKind.ProgramCreated:
                    if (program != null && !program.IsRegistered)
                    {
                        program.MarkRegistered();
                        dispatch(program, new RadProgramCreateEvent(startsNativeSession: false), typeof(IDebugProgramCreateEvent2).GUID, null);
                    }
                    break;

                case RadDbgAd7EventKind.LoadComplete:
                    if (program != null)
                    {
                        dispatch(program, new RadLoadCompleteEvent(@event.Attributes), typeof(IDebugLoadCompleteEvent2).GUID, null);
                    }
                    break;

                case RadDbgAd7EventKind.ThreadCreated:
                {
                    RadDbgThread? thread = program == null ? null : this.GetThread(program, @event.Event.ThreadHandle, @event.Event.SystemThreadId, createIfMissing: true);
                    if (thread != null)
                    {
                        dispatch(program, new RadThreadCreateEvent(@event.Attributes), typeof(IDebugThreadCreateEvent2).GUID, thread);
                    }
                }
                break;

                case RadDbgAd7EventKind.ThreadExited:
                {
                    RadDbgThread? thread = this.RemoveThread(@event.Event.ThreadHandle);
                    if (thread != null)
                    {
                        dispatch(program, new RadThreadDestroyEvent(@event.Event.Raw.Code, @event.Attributes), typeof(IDebugThreadDestroyEvent2).GUID, thread);
                    }
                }
                break;

                case RadDbgAd7EventKind.Stopped:
                {
                    RadDbgThread? thread = program == null ? null : this.GetThread(program, @event.Event.ThreadHandle, @event.Event.SystemThreadId, createIfMissing: false);
                    if (thread != null)
                    {
                        switch (@event.StopReason)
                        {
                            case RadDbgAd7StopReason.Exception:
                                dispatch(program, new RadExceptionEvent(@event.Event.Raw.Code, @event.Event.Raw.ExceptionRepeated != 0, @event.Attributes), typeof(IDebugExceptionEvent2).GUID, thread);
                                break;
                            case RadDbgAd7StopReason.Generic when getBreakpoint(@event.Event.BreakpointId) is IDebugBoundBreakpoint2 breakpoint:
                                dispatch(program, new RadBreakpointEvent(new IDebugBoundBreakpoint2[] { breakpoint }, @event.Attributes), typeof(IDebugBreakpointEvent2).GUID, thread);
                                break;
                            case RadDbgAd7StopReason.Break:
                                dispatch(program, new RadBreakEvent(@event.Attributes), typeof(IDebugBreakEvent2).GUID, thread);
                                break;
                            default:
                                dispatch(program, new RadStopCompleteEvent(@event.Attributes), typeof(IDebugStopCompleteEvent2).GUID, thread);
                                break;
                        }
                    }
                }
                break;

                case RadDbgAd7EventKind.Output:
                    if (program != null)
                    {
                        dispatch(program, new RadOutputStringEvent(text, @event.Attributes), typeof(IDebugOutputStringEvent2).GUID, this.GetThread(program, @event.Event.ThreadHandle, @event.Event.SystemThreadId, createIfMissing: false));
                    }
                    break;

                case RadDbgAd7EventKind.ProgramDestroyed:
                    if (program != null)
                    {
                        dispatch(program, new RadProgramDestroyEvent(@event.Event.Raw.Code, @event.Sequence, @event.Attributes), typeof(IDebugProgramDestroyEvent2).GUID, null);
                    }
                    programDestroyed(@event);
                    break;
            }

            return false;
        }

        private RadDbgThread? GetThread(RadDbgProgram program, ulong threadHandle, uint systemThreadId, bool createIfMissing)
        {
            lock (this.threadLock)
            {
                if (threadHandle != 0 && this.threads.TryGetValue(threadHandle, out RadDbgThread? thread))
                {
                    return thread;
                }
                if (!createIfMissing || threadHandle == 0)
                {
                    return null;
                }
                if (this.GetThreadName(threadHandle, out string name) != VSConstants.S_OK)
                {
                    return null;
                }
                thread = new RadDbgThread(program, systemThreadId, threadHandle, name);
                this.threads[threadHandle] = thread;
                return thread;
            }
        }

        private RadDbgThread? RemoveThread(ulong threadHandle)
        {
            lock (this.threadLock)
            {
                if (threadHandle != 0 && this.threads.TryGetValue(threadHandle, out RadDbgThread? thread))
                {
                    this.threads.Remove(threadHandle);
                    return thread;
                }
            }
            return null;
        }

        public void Dispose()
        {
            if (this.sessionPtr == IntPtr.Zero)
            {
                return;
            }
            Bridge.IDebugEngine2_DestroySession(this.sessionPtr);
            this.sessionPtr = IntPtr.Zero;
        }

        private static class Bridge
        {
            [DllImport(RadDbgBridgeFileName, CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
            public static extern uint radvs_bridge_abi_version();

            [DllImport(RadDbgBridgeFileName, CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
            public static extern int IDebugEvent2_Wait(IntPtr session, uint timeoutMilliseconds, out RadDbgAd7Event e, IntPtr textBuffer, ulong textBufferSize, out ulong textSize);

            [DllImport(RadDbgBridgeFileName, CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
            public static extern int IDebugEvent2_Acknowledge(IntPtr session, ulong sequence);

            [DllImport(RadDbgBridgeFileName, CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
            public static extern int IDebugEvent2_GetAttributes(IntPtr session, ulong sequence, out uint attributes);

            [DllImport(RadDbgBridgeFileName, CallingConvention = CallingConvention.Cdecl, CharSet = CharSet.Unicode, ExactSpelling = true)]
            public static extern int IDebugEngineLaunch2_LaunchSuspended(string exe, string cmdLine, string workingDirectory, out IntPtr session, out uint systemProcessId);

            [DllImport(RadDbgBridgeFileName, CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
            public static extern int IDebugEngineLaunch2_CanTerminateProcess(IntPtr session, ulong processHandle);

            [DllImport(RadDbgBridgeFileName, CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
            public static extern int IDebugEngineLaunch2_ResumeProcess(IntPtr session, ulong processHandle);

            [DllImport(RadDbgBridgeFileName, CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
            public static extern int IDebugEngineLaunch2_TerminateProcess(IntPtr session, ulong processHandle);

            [DllImport(RadDbgBridgeFileName, CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
            public static extern void IDebugEngine2_DestroySession(IntPtr session);

            [DllImport(RadDbgBridgeFileName, CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
            public static extern int IDebugEngine2_Attach(IntPtr session, IntPtr programs, IntPtr programNodes, uint programCount, IntPtr eventCallback, uint attachReason);

            [DllImport(RadDbgBridgeFileName, CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
            public static extern int IDebugEngine2_CauseBreak(IntPtr session);

            [DllImport(RadDbgBridgeFileName, CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
            public static extern int IDebugEngine2_ContinueFromSynchronousEvent(IntPtr session);

            [DllImport(RadDbgBridgeFileName, CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
            public static extern int IDebugEngine2_CreatePendingBreakpoint(IntPtr session, ref RadDbgBreakpointSpec request, out ulong breakpointId);

            [DllImport(RadDbgBridgeFileName, CallingConvention = CallingConvention.Cdecl, CharSet = CharSet.Unicode, ExactSpelling = true)]
            public static extern int IDebugEngine2_CreateBreakpoint(IntPtr session, string sourcePath, uint line, uint column, out ulong breakpointId);

            [DllImport(RadDbgBridgeFileName, CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
            public static extern int IDebugEngine2_CreateAddressBreakpoint(IntPtr session, ulong address, uint enabled, uint addressMode, out ulong breakpointId);

            [DllImport(RadDbgBridgeFileName, CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
            public static extern int IDebugEngine2_SetBreakpointEnabled(IntPtr session, ulong breakpointId, uint enabled);

            [DllImport(RadDbgBridgeFileName, CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
            public static extern int IDebugEngine2_DeleteBreakpoint(IntPtr session, ulong breakpointId);

            [DllImport(RadDbgBridgeFileName, CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
            public static extern int IDebugEngine2_DestroyProgram(IntPtr session, IntPtr program);

            [DllImport(RadDbgBridgeFileName, CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
            public static extern int IDebugEngine2_EnumPrograms(IntPtr session, [Out] RadDbgAd7ProgramDesc[]? buffer, ulong bufferCount, out ulong count, IntPtr textBuffer, ulong textBufferSize, out ulong textSize);

            [DllImport(RadDbgBridgeFileName, CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
            public static extern int IDebugEngine2_GetEngineId(IntPtr session, out Guid engineId);

            [DllImport(RadDbgBridgeFileName, CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
            public static extern int IDebugEngine2_RemoveAllSetExceptions(IntPtr session, ref Guid exceptionType);

            [DllImport(RadDbgBridgeFileName, CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
            public static extern int IDebugEngine2_RemoveSetException(IntPtr session, ref EXCEPTION_INFO exceptionInfo);

            [DllImport(RadDbgBridgeFileName, CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
            public static extern int IDebugEngine2_SetException(IntPtr session, ref EXCEPTION_INFO exceptionInfo);

            [DllImport(RadDbgBridgeFileName, CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
            public static extern int IDebugEngine2_SetLocale(IntPtr session, ushort languageId);

            [DllImport(RadDbgBridgeFileName, CallingConvention = CallingConvention.Cdecl, CharSet = CharSet.Unicode, ExactSpelling = true)]
            public static extern int IDebugEngine2_SetMetric(IntPtr session, string metric, IntPtr value);

            [DllImport(RadDbgBridgeFileName, CallingConvention = CallingConvention.Cdecl, CharSet = CharSet.Unicode, ExactSpelling = true)]
            public static extern int IDebugEngine2_SetRegistryRoot(IntPtr session, string registryRoot);

            [DllImport(RadDbgBridgeFileName, CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
            public static extern int IDebugProgram2_Attach(IntPtr session, IntPtr eventCallback);

            [DllImport(RadDbgBridgeFileName, CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
            public static extern int IDebugProgram2_CanDetach(IntPtr session);

            [DllImport(RadDbgBridgeFileName, CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
            public static extern int IDebugProgram2_GetDescriptor(IntPtr session, out RadDbgAd7ProgramDesc desc, IntPtr textBuffer, ulong textBufferSize, out ulong textSize);

            [DllImport(RadDbgBridgeFileName, CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
            public static extern int IDebugProgram2_GetDescriptorForProcess(IntPtr session, ulong processHandle, out RadDbgAd7ProgramDesc desc, IntPtr textBuffer, ulong textBufferSize, out ulong textSize);

            [DllImport(RadDbgBridgeFileName, CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
            public static extern int IDebugProgram2_CopyThreads(IntPtr session, [Out] RadDbgThreadDesc[]? buffer, ulong bufferCount, out ulong outCount);

            [DllImport(RadDbgBridgeFileName, CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
            public static extern int IDebugProgram2_CopyModules(IntPtr session, [Out] RadDbgModuleDesc[]? buffer, ulong bufferCount, out ulong outCount);

            [DllImport(RadDbgBridgeFileName, CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
            public static extern int IDebugProgram2_Detach(IntPtr session);

            [DllImport(RadDbgBridgeFileName, CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
            public static extern int IDebugProgram2_EnumCodeContexts(IntPtr session, IntPtr documentPosition, out IntPtr contexts);

            [DllImport(RadDbgBridgeFileName, CallingConvention = CallingConvention.Cdecl, CharSet = CharSet.Unicode, ExactSpelling = true)]
            public static extern int IDebugProgram2_EnumCodePaths(IntPtr session, string hint, IntPtr startContext, IntPtr stackFrame, int source, out IntPtr paths, out IntPtr safetyContext);

            [DllImport(RadDbgBridgeFileName, CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
            public static extern int IDebugProgram2_EnumModules(IntPtr session, [Out] RadDbgModuleDesc[]? buffer, ulong bufferCount, out ulong outCount);

            [DllImport(RadDbgBridgeFileName, CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
            public static extern int IDebugProgram2_EnumThreads(IntPtr session, [Out] RadDbgThreadDesc[]? buffer, ulong bufferCount, out ulong outCount);

            [DllImport(RadDbgBridgeFileName, CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
            public static extern int IDebugProgram2_Execute(IntPtr session);

            [DllImport(RadDbgBridgeFileName, CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
            public static extern int IDebugProgram2_GetDebugProperty(IntPtr session, out IntPtr property);

            [DllImport(RadDbgBridgeFileName, CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
            public static extern int IDebugProgram2_GetDisassemblyStream(IntPtr session, uint scope, IntPtr codeContext, out IntPtr stream);

            [DllImport(RadDbgBridgeFileName, CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
            public static extern int IDebugProgram2_GetENCUpdate(IntPtr session, out IntPtr update);

            [DllImport(RadDbgBridgeFileName, CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
            public static extern int IDebugProgram2_GetEngineInfo(IntPtr session, IntPtr nameBuffer, ulong nameBufferSize, out ulong nameSize, out Guid engineId);

            [DllImport(RadDbgBridgeFileName, CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
            public static extern int IDebugProgram2_GetMemoryBytes(IntPtr session, out IntPtr memoryBytes);

            [DllImport(RadDbgBridgeFileName, CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
            public static extern int IDebugProgram2_GetName(IntPtr session, IntPtr nameBuffer, ulong nameBufferSize, out ulong nameSize);

            [DllImport(RadDbgBridgeFileName, CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
            public static extern int IDebugProgram2_GetProcess(IntPtr session, out ulong processHandle);

            [DllImport(RadDbgBridgeFileName, CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
            public static extern int IDebugProgram2_GetProgramId(IntPtr session, out Guid programId);

            [DllImport(RadDbgBridgeFileName, CallingConvention = CallingConvention.Cdecl, CharSet = CharSet.Unicode, ExactSpelling = true)]
            public static extern int IDebugProgram2_ResolveSourcePosition(IntPtr session, string sourcePath, uint line, uint column);

            [DllImport(RadDbgBridgeFileName, CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
            public static extern int IDebugProgram2_Continue(IntPtr session, ulong threadHandle);

            [DllImport(RadDbgBridgeFileName, CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
            public static extern int IDebugProgram2_CauseBreak(IntPtr session);

            [DllImport(RadDbgBridgeFileName, CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
            public static extern int IDebugProgram2_Step(IntPtr session, ulong threadHandle, uint stepKind, uint stepUnit);

            [DllImport(RadDbgBridgeFileName, CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
            public static extern int IDebugProgram2_Terminate(IntPtr session);

            [DllImport(RadDbgBridgeFileName, CallingConvention = CallingConvention.Cdecl, CharSet = CharSet.Unicode, ExactSpelling = true)]
            public static extern int IDebugProgram2_WriteDump(IntPtr session, uint dumpType, string dumpUrl);

            [DllImport(RadDbgBridgeFileName, CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
            public static extern int IDebugProcess2_Attach(IntPtr session, ulong processHandle, IntPtr eventCallback, IntPtr engineIds, uint engineCount, IntPtr engineAttachResults);

            [DllImport(RadDbgBridgeFileName, CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
            public static extern int IDebugProcess2_CanDetach(IntPtr session, ulong processHandle);

            [DllImport(RadDbgBridgeFileName, CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
            public static extern int IDebugProcess2_CauseBreak(IntPtr session, ulong processHandle);

            [DllImport(RadDbgBridgeFileName, CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
            public static extern int IDebugProcess2_Detach(IntPtr session, ulong processHandle);

            [DllImport(RadDbgBridgeFileName, CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
            public static extern int IDebugProcess2_EnumPrograms(IntPtr session, ulong processHandle, [Out] RadDbgAd7ProgramDesc[]? buffer, ulong bufferCount, out ulong count, IntPtr textBuffer, ulong textBufferSize, out ulong textSize);

            [DllImport(RadDbgBridgeFileName, CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
            public static extern int IDebugProcess2_EnumThreads(IntPtr session, ulong processHandle, [Out] RadDbgThreadDesc[]? buffer, ulong bufferCount, out ulong outCount);

            [DllImport(RadDbgBridgeFileName, CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
            public static extern int IDebugProcess2_GetAttachedSessionName(IntPtr session, ulong processHandle, IntPtr textBuffer, ulong textBufferSize, out ulong textSize);

            [DllImport(RadDbgBridgeFileName, CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
            public static extern int IDebugProcess2_GetInfo(IntPtr session, ulong processHandle, uint fields, IntPtr processInfo);

            [DllImport(RadDbgBridgeFileName, CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
            public static extern int IDebugProcess2_GetName(IntPtr session, ulong processHandle, uint nameKind, IntPtr textBuffer, ulong textBufferSize, out ulong textSize);

            [DllImport(RadDbgBridgeFileName, CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
            public static extern int IDebugProcess2_GetPhysicalProcessId(IntPtr session, ulong processHandle, out uint systemProcessId);

            [DllImport(RadDbgBridgeFileName, CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
            public static extern int IDebugProcess2_GetPort(IntPtr session, ulong processHandle, out IntPtr port);

            [DllImport(RadDbgBridgeFileName, CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
            public static extern int IDebugProcess2_GetProcessId(IntPtr session, ulong processHandle, out Guid processId);

            [DllImport(RadDbgBridgeFileName, CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
            public static extern int IDebugProcess2_GetServer(IntPtr session, ulong processHandle, out IntPtr server);

            [DllImport(RadDbgBridgeFileName, CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
            public static extern int IDebugProcess2_Terminate(IntPtr session, ulong processHandle);

            [DllImport(RadDbgBridgeFileName, CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
            public static extern int IDebugThread2_CanSetNextStatement(IntPtr session, ulong threadHandle, IntPtr stackFrame, IntPtr codeContext);

            [DllImport(RadDbgBridgeFileName, CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
            public static extern int IDebugThread2_EnumFrameInfo(IntPtr session, ulong threadHandle, uint fieldSpec, uint radix, out IntPtr frames);

            [DllImport(RadDbgBridgeFileName, CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
            public static extern int IDebugThread2_GetName(IntPtr session, ulong threadHandle, IntPtr textBuffer, ulong textBufferSize, out ulong textSize);

            [DllImport(RadDbgBridgeFileName, CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
            public static extern int IDebugThread2_GetLogicalThread(IntPtr session, ulong threadHandle, IntPtr stackFrame, out IntPtr logicalThread);

            [DllImport(RadDbgBridgeFileName, CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
            public static extern int IDebugThread2_GetProgram(IntPtr session, ulong threadHandle, out IntPtr program);

            [DllImport(RadDbgBridgeFileName, CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
            public static extern int IDebugThread2_GetThreadId(IntPtr session, ulong threadHandle, out uint threadId);

            [DllImport(RadDbgBridgeFileName, CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
            public static extern int IDebugThread2_GetThreadProperties(IntPtr session, ulong threadHandle, uint fields, ref THREADPROPERTIES properties);

            [DllImport(RadDbgBridgeFileName, CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
            public static extern int IDebugThread2_GetTopFrame(IntPtr session, ulong threadHandle, out RadDbgFrameInfo frame, IntPtr sourcePathBuffer, ulong sourcePathBufferSize, out ulong sourcePathSize);

            [DllImport(RadDbgBridgeFileName, CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
            public static extern int IDebugThread2_Resume(IntPtr session, ulong threadHandle, out uint suspendCount);

            [DllImport(RadDbgBridgeFileName, CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
            public static extern int IDebugThread2_SetNextStatement(IntPtr session, ulong threadHandle, IntPtr stackFrame, IntPtr codeContext);

            [DllImport(RadDbgBridgeFileName, CallingConvention = CallingConvention.Cdecl, CharSet = CharSet.Unicode, ExactSpelling = true)]
            public static extern int IDebugThread2_SetThreadName(IntPtr session, ulong threadHandle, string name);

            [DllImport(RadDbgBridgeFileName, CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
            public static extern int IDebugThread2_Step(IntPtr session, ulong threadHandle, uint stepKind, uint stepUnit);

            [DllImport(RadDbgBridgeFileName, CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
            public static extern int IDebugThread2_Suspend(IntPtr session, ulong threadHandle, out uint suspendCount);

            [DllImport(RadDbgBridgeFileName, CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
            public static extern int IDebugThread2_SetInstructionPointer(IntPtr session, ulong threadHandle, ulong address);

            [DllImport(RadDbgBridgeFileName, CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
            public static extern int IDebugMemoryBytes2_GetSize(IntPtr session, ulong processHandle, out ulong size);

            [DllImport(RadDbgBridgeFileName, CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
            public static extern int IDebugMemoryBytes2_ReadAt(IntPtr session, ulong processHandle, ulong address, uint byteCount, IntPtr buffer, out uint read, out uint unreadable);

            [DllImport(RadDbgBridgeFileName, CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
            public static extern int IDebugMemoryBytes2_WriteAt(IntPtr session, ulong processHandle, ulong address, uint byteCount, IntPtr buffer);

            [DllImport(RadDbgBridgeFileName, CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
            public static extern int IDebugMemoryBytes2_Read(IntPtr session, ulong processHandle, ulong address, IntPtr buffer, ulong bufferSize, out ulong size);
        }

        private static class Kernel32
        {
            [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
            public static extern IntPtr LoadLibrary(string fileName);
        }

        private static class OleAut32
        {
            [DllImport("oleaut32.dll", ExactSpelling = true)]
            public static extern int VariantClear(IntPtr variant);
        }
    }
}
