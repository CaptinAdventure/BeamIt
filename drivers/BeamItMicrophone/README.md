# BeamIt Microphone

Source-built Windows 11+ x64 virtual recording device. This subproject is
independent of the host CMake build and has no third-party virtual-cable
dependency.

**Status:** the PortCls/WaveRT capture driver, local PCM injection interface,
INF/catalog package and deployment/test utility are implemented. The unsigned
package can be built and checked without installation. Loading the signed
package, Driver Verifier/HLK, sleep/resume/removal testing and actual Teams
validation remain required; a successful build is not runtime certification.
The existing host is not yet connected to this device.

## Architecture and scope

```text
Remote microphone -> authenticated audio WebSocket -> Opus / AudioEndpoint
                                                   |
                                  future host bridge: playout/drift/float->PCM16
                                                   |
Test producer ----------------------------> versioned local control interface
                                                   |
                                          bounded PCM frame queue
                                                   |
                                   QPC-clocked WaveRT capture DMA buffer
                                                   |
                                   BeamIt Microphone -> Windows -> Teams
```

The driver exposes one recording endpoint, **BeamIt Microphone**, and no render
endpoint, APO, synthetic production source or default-device override. It
accepts 48 kHz stereo PCM16. The Windows audio engine can convert its stream
for shared-mode applications. One WaveRT system capture pin is supported;
multiple shared-mode recording applications are serviced by the Windows audio
engine, not by multiple consumers draining the producer queue. Exclusive-mode
recording excludes other recording clients as usual.

`driver/AudioMiniport.cpp` implements the topology and capture miniports.
`driver/ControlDevice.cpp` implements the local injection boundary. The code is
original BeamIt implementation against documented WDK interfaces, not an
imported SysVAD snapshot. SysVAD was consulted as architectural reference.
PortCls/WaveRT was chosen to expose a normal recording endpoint; an external
virtual cable violates the source-build requirement, and a user-mode WASAPI
library alone cannot create this endpoint.

Opus, sockets, password authentication, jitter buffering and adaptive
resampling stay out of kernel mode. They belong in the future user-mode bridge,
which must parse the negotiated wire configuration rather than assume this
driver's format or a fixed packet duration.

## Local injection ABI v2

See `shared/BeamItMicIoctl.h`. The previous counter-only skeleton ABI v1 is
superseded; update producer and driver together. Fixed-width Windows types and
layout assertions are shared by both builds. The payload is native
little-endian Windows PCM, not the network Opus framing.

Open `\\.\BeamItMicCtl` with `GENERIC_READ | GENERIC_WRITE` and shared read/write
access, then explicitly acquire a producer lease.

| Operation | Contract |
| --- | --- |
| `ACQUIRE` | Exact `BeamItMicAcquireRequest`, version 2 and advertised format. Only one open file object can own the lease; even another handle in the same process gets busy. `process_id` is advisory/ignored; the kernel records the actual requestor. |
| `WRITE_PCM` | Exact 32-byte header plus one packet of interleaved PCM16. Flags may contain only `BeamItMicPcmDiscontinuity`; reserved must be zero. |
| `RESET` | Owner only, no input; clear queued audio and require a fresh discontinuous packet. |
| `RELEASE` | Owner only, no input; discard audio and relinquish the lease. |
| `GET_STATS` | Return owner PID, leased/capturing state, accepted/rejected packets, accepted frames, underrun/overrun frames and queued frames. Counters accumulate for the loaded driver's lifetime. |

Packet duration is a named constant, initially 10 ms; frames and bytes are
derived from frequency, channels, bit depth and duration. Initial packets
require discontinuity. Sequence numbers must strictly increase, gaps require
discontinuity, and sample timestamps must equal sequence times frames per
packet without overflow. Duplicate or backward packets are rejected even when
marked discontinuous. A reset permits a new sequence epoch.

The queue holds at most three packets (30 ms at present). Overflow discards
the oldest frames and counts them; it never grows. Capture advances on the
local QPC clock, using a high-resolution kernel timer only while RUN.
No producer or insufficient PCM means zero-filled capture, never replayed
audio or a stopped clock. Timer lateness skips overwritten DMA history rather
than doing unbounded catch-up; packet numbers expose missed capture packets.
The queue limit is not a target jitter delay or a latency guarantee.

Packets are accepted only while a recording stream is RUN. Open a recording
application before sending. Capture stop/start clears the queue and requires
discontinuity; power-down, device stop/removal and handle cleanup release
ownership as well. Driver callbacks are drained before DMA pages are unmapped.
Queued PCM and the next DMA update are cleared on lease changes; audio already
delivered into Windows/application buffers cannot be retracted.

Ownership is tied to the file object, not a PID, host mutex or user-supplied
identifier. `IRP_MJ_CLEANUP` releases it automatically on close/process exit.
Duplicated handles share that same lease until the last handle is closed.
The control-device ACL permits SYSTEM, administrators and interactive users
(including remote interactive logons), not anonymous/network principals.
Any permitted local process can acquire the idle device; this is a local-user
trust boundary, not executable identity authentication.

Unavailable, busy, access-denied, malformed input and format/version mismatch
return explicit failing NTSTATUS/Win32 results. No fallback to physical
speakers or another microphone is performed. Local production consumers must
release the lease on audio-session end and report failures to the user.

## Build

Prerequisites: Visual Studio 2022 v143 x64 tools with Spectre-mitigated
libraries, Windows SDK `10.0.26100.0` and WDK 26100.6584 with VS integration.
Kernel sources use C++17; the standalone utility uses C++20. No WDK requirement
is imposed on normal BeamIt host builds.

From a VS x64 Native Tools terminal:

```powershell
msbuild .\drivers\BeamItMicrophone\BeamItMicrophone.sln /m /p:Configuration=Release /p:Platform=x64
```

Or open `BeamItMicrophone.sln` in Visual Studio. Outputs are deterministic
regardless of solution/project invocation:

```text
drivers/BeamItMicrophone/bin/x64/Release/
    BeamItMicProducer.exe
    BeamItMicDriver/
        BeamItMicrophone.inf
        BeamItMicDriver.sys
        beamitmicrophone.cat
```

The driver target stages the INF and SYS and runs WDK Inf2Cat. `SignMode=Off`
deliberately avoids certificate creation, machine-trust changes or loading
kernel code. **Sign/distribute the package subdirectory as a unit**, not just
the flat `.sys` build output. Building again regenerates package artifacts;
do not rebuild after signing unless you intend to sign the new package.

Optional static analysis and explicit INF verification:

```powershell
msbuild .\drivers\BeamItMicrophone\driver\BeamItMicDriver.vcxproj /t:Rebuild /p:Configuration=Release /p:Platform=x64 /p:RunCodeAnalysis=true
& "${env:ProgramFiles(x86)}\Windows Kits\10\Tools\10.0.26100.0\x64\InfVerif.exe" /w .\drivers\BeamItMicrophone\bin\x64\Release\BeamItMicDriver\BeamItMicrophone.inf
```

## Signing and isolated deployment

Use an isolated Windows test machine/VM with a snapshot, kernel debugging and
a recovery path. Do not install an unvalidated driver on the development or
production host.

A normal code-signing certificate alone does **not** establish acceptance by
Windows kernel signing policy. Development test signing and production
Microsoft Hardware Developer Center submission are distinct workflows.
Production deployment must satisfy Windows 11/Secure Boot/HVCI requirements;
do not disable these on users' machines as an installation workaround.

For a prepared, explicitly test-signing-enabled lab system, sign the generated
catalog with the selected trusted test certificate. For example:

```powershell
signtool sign /fd SHA256 /sha1 <certificate-thumbprint> <package>\beamitmicrophone.cat
signtool verify /kp /v <package>\beamitmicrophone.cat
```

These commands select a certificate already in the appropriate signing store;
they do not provision trust or enable test signing. If you additionally embed
a signature in the SYS, do so **before regenerating the catalog**, then sign
the regenerated catalog. Never edit packaged INF/SYS bytes after catalog
signing. A production package must use the Microsoft-approved signing path.

After signing, from an **administrator terminal on the isolated test system**:

```powershell
.\BeamItMicProducer.exe --install C:\Package\BeamItMicrophone.inf
```

The utility creates the matching root device only if absent, then uses Windows
SetupAPI/newdev to install/update the package. On failed installation it tries
to roll back a newly created node and reports rollback errors. It does not
bypass signature enforcement or install a certificate.

Close all recording applications and producer handles before update/removal:

```powershell
.\BeamItMicProducer.exe --remove
```

This removes the matching root node and driver-store INF. If the store package
is still in use, it reports the exact `oemNN.inf` and the explicit `pnputil`
cleanup command. Reboot when requested. Only one adapter is supported;
duplicate root devices are rejected rather than creating multiple microphones.

## Test

Hardware-independent tests do not install/load/open the driver:

```powershell
.\BeamItMicProducer.exe --self-test
```

These exercise the actual queue/sequence/timestamp code used by the driver:
partial reads, ring wrap, silence, overload, discontinuity, stale-data reset,
invalid metadata, timestamp overflow and QPC conversion.

After installing a signed package on the isolated system:

```powershell
.\BeamItMicProducer.exe --verify-device
```

This finds the BeamIt recording endpoint (no physical-microphone fallback),
opens event-driven WASAPI capture and checks silence, injected PCM,
non-owner rejection, malformed packets, cleanup-based lease transfer, no stale
audio, and capture restart. It requires an otherwise idle endpoint/producer.

For a manual recording test, open **BeamIt Microphone** in a recording app
first, then run `BeamItMicProducer.exe 5` to send a quiet 997 Hz tone for five
seconds. The utility uses a high-resolution waitable timer with QPC deadlines
and prints queue/overrun/underrun counters. It has no playback route.

Before calling this production-ready, complete Driver Verifier and applicable
Windows audio HLK testing, competing-process/crash tests, long-run timing,
sleep/resume, disable/re-enable, removal with open handles, reinstall/update,
multi-client shared-mode recording and Teams-call testing. Those require a
loaded signed driver and are not replaced by the deterministic self-tests.

## References

- [Audio driver samples / SysVAD](https://learn.microsoft.com/en-us/windows-hardware/drivers/samples/audio-driver-samples)
- [WaveRT miniport driver](https://learn.microsoft.com/en-us/windows-hardware/drivers/audio/developing-a-wavert-miniport-driver)
- [GetReadPacket contract](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/portcls/nf-portcls-iminiportwavertinputstream-getreadpacket)
- [Driver signing](https://learn.microsoft.com/en-us/windows-hardware/drivers/install/driver-signing)
