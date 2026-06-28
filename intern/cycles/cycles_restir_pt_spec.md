# cycles_restir_pt_spec.md

Goal:
Integrate Enhanced ReSTIR PT into Cycles.

Architecture:
- New integrator mode.
- Reuse existing IntegratorState.
- Add reservoir buffers.
- Add temporal/spatial reuse passes.

Phase A:
[x] Proof-of-concept `DEVICE_KERNEL_INTEGRATOR_RESTIR_TEST` branch; 
    Note: ReSTIR DI is not its own kernel stage.
    It runs inside `DEVICE_KERNEL_INTEGRATOR_SHADE_SURFACE`
[x] Direct-light-only RIS candidate gen (No temporal reuse. No spatial reuse, No replay shifts).
[x] (Reservoir selection) Recompute direct-light shading from the reservoir-selected sample
[x] Apply finalized reservoir weight to direct-light throughput
[x] Remove redundant classic NEE sampling in ReSTIR mode
[x] Add GPU per-pixel reservoir buffers and surface-shading kernel plumbing
[x] Add first-hit reservoir write-back for temporal reuse
[x] Prepare visibility-aware validation hook

Phase B:
[x] Initial temporal reuse merge from previous per-pixel reservoir
[x] Add current/previous reservoir ping-pong buffers
[x] Validate temporal reuse effectiveness with debug visualizations
[ ] Reprojection using motion vectors (Cross-pixel reuse when camera moves)
[x] Shadow-visibility-aware temporal validation
    Note:
    Current shadow visibility updates are wired for the wavefront ReSTIR DI path.
    CPU megakernel currently passes null reservoir buffers, so CPU visibility mutation is not yet active.

Phase C:
Spatial reuse.

Phase D:
Path replay.

Phase E:
ReSTIR GI

Phase F:
Unsure...

Phase G:
Enhanced paper features.

--- Jun 14th
