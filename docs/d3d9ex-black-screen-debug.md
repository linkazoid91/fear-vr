# D3D9Ex black-screen investigation

Last updated: 2026-07-30

This file is the working ledger for the FEAR D3D9Ex black-screen issue. An
experiment is not repeated unless new evidence changes one of its assumptions.

## Current fault boundary

The failure is upstream of capture and transport. FEAR's D3D9Ex render target is
already black when the bridge samples it.

The current fault is narrower than ordinary managed-resource reset survival.
The standalone converter test proves that a classic client can create managed
vertex/index buffers, issue `DrawIndexedPrimitive`, reset, reuse the same
resources, and produce verified pixels before and after reset.

FEAR's specific managed index-buffer lifecycle remains under investigation:

1. FEAR creates a `D3DPOOL_MANAGED` index buffer.
2. The compatibility layer must translate it to `D3DPOOL_DEFAULT`, because
   D3D9Ex does not support managed-pool resources.
3. FEAR locks a 49,152-byte buffer during startup.
4. At the later reset boundary, the registry reports no locked buffer to
   prepare and no shadowed buffer to restore.
5. After reset, no indexed UI draw is observed and the render targets remain
   black apart from the independently drawn FPS overlay.

The next checkpoint is to record the exact object lifecycle between steps 3
and 4: wrapper ID, Lock, Unlock, SetIndices, Release/destruction, and reset.

The previous assumption that FEAR necessarily leaves this particular lock
outstanding across reset has been rejected by the first pre-reset snapshot run:
it reported `prepared=0`.

## Experiment ledger

| Experiment | Result | Conclusion |
| --- | --- | --- |
| Run unmodified FEAR without the proxy | Menus render normally | Game installation and base renderer are healthy |
| Classic D3D9 CPU capture | FEAR menu reaches both host eyes | Capture consumer, stereo submission, and basic hook timing work |
| D3D9Ex through SteamVR | Black game image | Failure is present on the Ex path |
| D3D9Ex through VDXR | Same black image | Runtime selection is not the cause |
| Direct-shared D3D9Ex magenta fill | Magenta reaches both host eyes | D3D9Ex shared transport and D3D11 receiver work |
| DirectShared versus CpuViaD3D9Ex | Same black game source | Transport mode is not selecting the black pixels |
| Capture exact presenting swap-chain backbuffer before Present | Black | Not caused by always using device swap-chain zero |
| Probe current render target and presenting backbuffer together | Both black | Not merely an off-screen/current-RT selection mismatch |
| Track and probe slot-zero render targets | All candidate surfaces black | No completed image found among observed render targets |
| Hook device and swap-chain Present paths | Same result | No missed ordinary Present path identified |
| Disable GPU HUD compositor | Same black game image | HUD state restoration is not the initiating cause |
| Borderless D3D9Ex | Black | Window style does not fix rendering |
| Exclusive D3D9Ex | Black | Borderless translation is not the core cause |
| Small 960x540 desktop-only D3D9Ex window, no OpenXR host | Black reproduced | Headset and OpenXR can be removed from the reproduction |
| Advance past intro videos with Escape | States advance, image remains black | Not just an intro-video capture/timing issue |
| Log draw activity | Non-indexed overlay activity exists; expected indexed UI activity is absent | Failure occurs before final presentation |
| Preserve managed index usage flags and report original managed descriptor | Still black | Descriptor compatibility alone is insufficient |
| Log managed index-buffer lifetime | Lock of 49,152 bytes before reset; no pre-reset Unlock; restore reports zero buffers ready | Identifies a specific reset-boundary data-loss path |
| Standalone classic-D3D9-to-D3D9Ex clear/primitive test | Ex interface confirmed; animated frame, minimize/restore, and Reset all succeed | Basic compatibility device creation and desktop presentation work |
| Standalone managed indexed-draw/reset pixel test | White indexed marker verified before and after Reset using the same managed resources | Ordinary managed vertex/index translation and Reset survival work; FEAR uses a narrower failing lifecycle |

## Implemented diagnostics and safeguards

- Capture occurs before the original `Present`/`PresentEx`.
- Device and swap-chain presentation paths pass the exact presenting surface.
- Every capture/readback stage validates its `HRESULT`.
- System-memory probes use a `0xCD` sentinel and distinguish failed copies from
  genuinely black sources.
- Multisampled sources are resolved before `GetRenderTargetData`.
- `SetRenderTarget`, `StretchRect`, render-target textures, swap chains, and
  presenting windows are tracked.
- A one-frame magenta source test can validate the complete downstream path.
- `-DesktopD3D9ExTest` reproduces the issue in a centered 960x540 desktop
  window without starting the VR host.

## Managed index reset safeguard

The managed index-buffer wrapper now:

- snapshots an outstanding locked range before reset;
- unlocks the real default-pool buffer so reset can proceed;
- restores the complete shadow copy after a successful reset;
- absorbs FEAR's delayed Unlock for the pre-reset lock;
- ensures that stale delayed-Unlock suppression cannot consume a later,
  unrelated lock/unlock pair.

This change is wired into classic `Reset`, `ResetEx`, and the app-local
classic-to-Ex reset redirection. It passes the standalone indexed reset test,
but FEAR's first run did not have an outstanding registered lock at reset, so
the exact FEAR object lifecycle must be established before changing this logic
again.
