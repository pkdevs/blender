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

Phase B.5: RIS Baseline Cleanup
[x] Remove dead uv/visible fields from RIS/reservoir
[x] Make ReSTIR final weighting internally consistent
    Note: Temporal reuse is disbaled in runs until sample reconstruction exists!
    Note: Current RIS validation baseline assumes direct light sampling without light tree.
        Light tree integration requires auditing proposal pdf semantics and MIS consistency.



Phase C:
Spatial reuse.

Phase C.0: New reservoir structs
[ ] Add radiance/luminance/total_weight reservoir variant
[ ] Add MIS-aware add_light_sample()
[ ] Add direct-light sampling method awareness
[ ] Add measure/Jacobian audit

Phase C.1: Spatial surface payload
[ ] Store/unpack current surface data
[ ] Reconstruct ShaderData for neighbor/current
[ ] Add neighbor validity checks

Phase C.2: Spatial reuse prototype
[ ] Merge compatible reservoirs
[ ] Pairwise MIS weight
[ ] Visibility option
[ ] Final sample evaluation

Phase C.x:
[ ] Proper compact canonical sample identity
[ ] Proper UV/sample parameterization for current LightSample
[ ] Real light_sample_from_uv replacement for new Cycles

Phase C.x / Post-stability cleanup:
[ ] Compact reservoir payload (avoid storing full LightSample; use `light_id`)
[ ] Cheaper target function for RIS/temporal evaluation
    Note: (`target = luminance(candidate_light_eval) * abs(dot(N,L))`)
    Deferred until DI temporal reuse is stable and spatial reuse is integrated,
    to avoid conflating performance refactors with correctness debugging.


Phase D:
Path replay.

Phase E:
ReSTIR GI

Phase F:
Unsure...

Phase G:
Enhanced paper features.

--- Jun 14th
