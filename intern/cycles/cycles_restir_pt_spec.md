# cycles_restir_pt_spec.md

Goal:
Integrate Enhanced ReSTIR PT into Cycles.

Architecture:
- New integrator mode.
- Reuse existing IntegratorState.
- Add reservoir buffers.
- Add temporal/spatial reuse passes.

Phase A:
[x] Proof-of-concept `DEVICE_KERNEL_INTEGRATOR_RESTIR_TEST` branch
[x] Direct-light-only RIS (No temporal reuse. No spatial reuse, No replay shifts).
[x] Recompute direct-light shading from the reservoir-selected sample
[x] Apply finalized reservoir weight to direct-light throughput
[ ] Add visibility-aware reservoir validation

Phase B:
Temporal reuse.

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
