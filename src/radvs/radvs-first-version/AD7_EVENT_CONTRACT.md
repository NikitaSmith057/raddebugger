# RADVS AD7 Event Contract

`RADVS_Engine` owns the canonical process, thread, module, breakpoint, and stop
model. `RADVS_AD7` translates engine events into `RADVS_AD7_Event` descriptors.
The managed plugin owns only Visual Studio COM wrappers and callback delivery.

## Format Layer

`radvs_format.h` defines the frontend-neutral IDs, result codes, frame and
entity descriptors, breakpoint formats, and `RADVS_Event` payload. It does not
depend on the engine, AD7, or bridge layers.

## Native Boundary

- `IDebugEngineLaunch2_LaunchSuspended` initializes process-wide services, creates
  the session, launches the target, and returns the native session plus OS PID.
- `RADVS_Event` captures DEMON handles before an exited entity is
  released, so AD7 lifecycle events never require a managed identity map.
  `RADVS_AD7_Event` embeds that event and adds only AD7 delivery
  metadata. Managed code treats pointer fields in the embedded raw `DMN_Event`
  as opaque.
- `IDebugProgram2_GetDescriptor` owns program, host, engine, OS PID, and native
  process-handle metadata. `IDebugProgram2_CopyThreads` returns native thread
  descriptors and process-state epochs.
- `IDebugEvent2_Wait` waits for the native event condition variable. `S_FALSE`
  means its timeout elapsed without an event; managed code must not retry with a
  separate polling delay.
- `IDebugEvent2_Acknowledge` advances the native `ProgramDestroyed` lifecycle.

## Attributes

`RADVS_AD7_Event.attributes` uses the numeric values of AD7
`enum_EVENTATTRIBUTES`. Native owns the choice of `EVENT_ASYNCHRONOUS`,
`EVENT_SYNCHRONOUS`, and `EVENT_ASYNC_STOP`; managed event wrappers return the
descriptor-provided value unchanged.

## Startup And Shutdown

1. The managed COM façade delivers `IDebugProgramCreateEvent2` because the event
   requires managed program identity.
2. `IDebugEngine2_ContinueFromSynchronousEvent` transitions the native session
   into startup and runs DEMON.
3. `HandshakeComplete` produces one asynchronous `IDebugLoadCompleteEvent2`.
4. `ExitProcess` produces a synchronous `IDebugProgramDestroyEvent2`.
5. The callback's `ContinueFromSynchronousEvent` acknowledges that event through
   `IDebugEvent2_Acknowledge` before native state becomes closed.

`HandshakeComplete` is not an entry-point event.

## DEMON Mapping

| DEMON event | AD7 descriptor | Attribute |
| --- | --- | --- |
| `Error` | Output diagnostic string | `EVENT_ASYNCHRONOUS` |
| `HandshakeComplete` | `IDebugLoadCompleteEvent2` | `EVENT_ASYNCHRONOUS` |
| `CreateProcess` | Native model update | N/A |
| `ExitProcess` | `IDebugProgramDestroyEvent2` | `EVENT_SYNCHRONOUS` |
| `CreateThread` | `IDebugThreadCreateEvent2` | `EVENT_ASYNCHRONOUS` |
| `ExitThread` | `IDebugThreadDestroyEvent2` | `EVENT_ASYNCHRONOUS` |
| `Breakpoint`, `Trap` | `IDebugStopCompleteEvent2` | `EVENT_ASYNC_STOP` |
| `SingleStep` | `IDebugStopCompleteEvent2` until native stepping is implemented | `EVENT_ASYNC_STOP` |
| `Exception` | `IDebugExceptionEvent2` | `EVENT_ASYNC_STOP` |
| `Halt` | `IDebugBreakEvent2` | `EVENT_ASYNC_STOP` |
| `DebugString` | `IDebugOutputStringEvent2` | `EVENT_ASYNCHRONOUS` |

`RADVS_AD7` emits a native `ThreadCreated` descriptor before a stop for a thread
first observed at that stop. C# caches the corresponding COM wrapper by native
thread handle only; it does not own target topology or stop validity.
