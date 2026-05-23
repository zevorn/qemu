---
name: k230-kpu-issue-analysis
description: Use when diagnosing K230 KPU/GNNE bugs, regressions, hangs, skip events, numeric mismatches, bad RT-Smart AI outputs, UART faults after KPU, or trace divergences in this private QEMU K230 branch. Provides a structured evidence-first triage workflow using qtests, runtime traces, .plan analysis scripts, gdbstub probes, physwatch, remote SDK artifacts, and nncase simulator comparisons.
license: GPL-2.0-or-later
---

# K230 KPU Issue Analysis

This skill is for problem triage, not for general implementation.  For command
recipes and environment paths, also use `k230-kpu-modeling`.  For any QEMU
build or test execution, also use `qemu-build` / `qemu-testing`; those require
sub-agents for builds/tests.

Keep all scratch artifacts under `.plan/k230-gnne-analysis/`.  Do not commit
`.plan/` output.  Do not present AI-assisted patches as upstream-ready QEMU
work; follow `docs/devel/code-provenance.rst` and `AGENTS.md`.

## First Classify The Symptom

Assign the issue to one primary layer before changing code:

- `packaging`: wrong ELF/kmodel/image in ROMFS, stale `fw_payload.bin`,
  missing `/bin/init.sh`, wrong app banner.
- `guest-runtime`: RT-Smart ioctl, argument table, CPU copy, allocator crash,
  postprocess fault after KPU completion.
- `qemu-device`: MMIO, START/CLEAR/STATUS/IRQ, command range, DMA failure.
- `decode`: wrong opcode length, register state, shape/stride, MMU/rdata
  translation.
- `data-move`: L2_LOAD, L2_LOAD_W, L2_STORE, runtime DDR/rdata aliases.
- `compute`: PU, PDP0/PDP1, MFU_ACT1, MFU_TRANSPOSE, quant/dequant, Act0.
- `reference-mapping`: comparing the wrong output slice, stale simulator
  output, wrong physical/logical range.

Do not treat a UART postprocess crash as a KPU compute bug until the KPU event
gate and final source provenance have been checked.

## Minimum Evidence Bundle

For every nontrivial KPU issue, collect or identify:

- git commit and `git status --short`.
- exact QEMU binary and BIOS path.
- UART log.
- KPU trace log.
- command stream dump.
- GLB/DDR pmem dump range if data quality is involved.
- L2 store summary and extracted final-visible segments.
- reference compare markdown/json if an nncase output exists.
- qtest/build/checkpatch logs for source changes.

Use stable artifact names that include the experiment label, for example
`runtime-object-yolov8n-after-<change>-kpu-trace.log`.

## Event Gate

Before investigating numeric output, verify the structural gate:

```sh
for ev in \
  k230_kpu_start k230_kpu_gnne_summary \
  k230_kpu_gnne_compute_summary k230_kpu_l2_store \
  k230_kpu_pu_compute; do
  printf '%s ' "$ev"
  rg -c "^$ev" .plan/k230-gnne-analysis/<trace>.log || true
done

for ev in \
  k230_kpu_pu_compute_skip k230_kpu_l2_load_w_skip \
  k230_kpu_l2_store_skip k230_kpu_dma_error \
  k230_kpu_unsupported; do
  printf '%s ' "$ev"
  rg -c "^$ev" .plan/k230-gnne-analysis/<trace>.log || true
done
```

For the current YOLOv8n object-detect case, a healthy structural run has:

- 54 starts
- 54 GNNE summaries
- 54 compute summaries
- 64 L2 stores
- 5220 PU computes
- no skip/error/unsupported events

If this gate fails, diagnose decode/data movement before numeric quality.

## Data Quality Triage

Generate L2 store summaries:

```sh
python3 .plan/k230-gnne-analysis/summarize_l2_store_outputs.py \
  --trace .plan/k230-gnne-analysis/<trace>.log \
  --dump .plan/k230-gnne-analysis/<glb-dump>.bin \
  --json .plan/k230-gnne-analysis/<label>-l2-store-summary.json \
  --markdown .plan/k230-gnne-analysis/<label>-l2-store-summary.md \
  --extract-dir .plan/k230-gnne-analysis/<label>-l2-store-segments
```

Compare against reference only after confirming the physical range and output
slice.  For the current YOLOv8n class-score part, store 64 is
`80 * 2100 * float32` and compares against `nncase-output-0.bin` at offset
`4 * 2100 * float32` bytes.

When a change alters output, compare against both the reference and the prior
capture:

- exact store hash changes
- first changed final-visible store
- first changed compute head if traces include compute heads
- byte-equal count
- mean absolute error and RMSE
- first mismatch index and raw bytes

Do not claim improvement based on one store hash alone.  Use slice metrics and
the relevant reference mapping.

## Find The First Divergence

Localize regressions by comparing two captures from nearest checkpoints:

1. Compare event totals; if totals differ, inspect decode/control first.
2. Compare final-visible L2 store hashes to find changed stores.
3. Compare `All Stores` rows to find earlier overwritten producers.
4. Search trace events around the first changed store's command range.
5. For PU problems, correlate:
   - `k230_kpu_pu_input_source`
   - `k230_kpu_pu_compute`
   - latest `DM_LOAD_L1`
   - fetch value
   - quant/load-psum/dest-target flags
6. Record the earliest PC/submission where data diverges.

For PU source-addressing issues, summarize input sources:

```sh
python3 .plan/k230-gnne-analysis/summarize_pu_input_sources.py \
  .plan/k230-gnne-analysis/<trace>.log \
  --json .plan/k230-gnne-analysis/<label>-pu-input-source-summary.json \
  --markdown .plan/k230-gnne-analysis/<label>-pu-input-source-summary.md \
  --limit 120
```

Treat low nonzero `PU_FETCHIF_CONF3` values carefully.  Current evidence shows
`fetch=0x200` is likely an IF-internal view/bank offset from `TCU::FillIf`,
not automatically a guest-memory byte offset into the latest `DM_LOAD_L1`
source.

## Use GDB And Physwatch For Provenance

Use gdbstub when trace proves KPU finished but the producer or guest-side
consumer is unclear:

```sh
BIOS=/Users/zevorn/k230-project/images/rtt-object-yolov8n/fw_payload.bin \
.plan/k230-gnne-analysis/run_qemu_gdbstub.sh

/opt/homebrew/bin/riscv64-elf-gdb -q \
  -x .plan/k230-gnne-analysis/gnne_probe.gdb
```

Useful commands:

```gdb
connect_k230
kpu_regs
b_known_k230_runtime
b_kpu_start_store
continue
```

Prefer QEMU monitor physical access (`monitor xp`, `monitor pmemsave`) when
guest mappings are uncertain.

Use specialized probes for runtime provenance:

- `probe_runtime_arg_table.gdb`: runtime input/output argument table.
- `probe_final_runtime_source.gdb`: final YOLOv8n source/output ranges.
- `probe_final_source_writer.gdb`: guest CPU writer for final source buffer.
- `probe_final_source_writer_blocks.gdb`: block-level source mapping.
- `probe_final_tensor_structs.gdb`: tensor metadata around final runtime call.

Use `physwatch.dylib` to catch guest CPU stores into a physical range:

```sh
python3 .plan/k230-gnne-analysis/capture_gnne_run.py \
  --plugin file=.plan/k230-gnne-analysis/physwatch.dylib,start=0x10944020,size=0x52000 \
  --pmemsave 0x10944020:0x52000:.plan/k230-gnne-analysis/final-source.bin
```

## Simulator And SDK Cross-Checks

Use simulator evidence when the trace identifies a semantic gap:

- `nncase.simulator.k230.sc`: native simulator binary.
- `Nncase.Modules.K230.dll`: enum and instruction layout metadata.
- `extract_k230_layouts.py`: recover instruction bit ranges from CIL
  `Serialize()` methods.
- `dump_k230_dll_enums.py`: recover enum constants.
- `summarize_k230_functions.py --frontend`: compare static function totals
  against runtime totals.

Only translate a simulator observation into QEMU behavior after identifying:

- exact function/operator path, such as `TCU::FillIf` or `MeshNet::MfuAct1`.
- source/destination address formula.
- dtype/quant/dequant path.
- one observed runtime PC/function that needs the rule.
- one small qtest that would fail without the rule.

## Decision Rules

- If only trace observability is missing, add trace-only instrumentation and
  prove store hashes do not change.
- If a qtest fails but runtime gate is stable, fix the narrow qtest-covered
  behavior first.
- If runtime output worsens while qtests pass, do not broaden the heuristic.
  Recover a discriminator from trace/simulator evidence.
- If UART faults after KPU but trace gate is closed, inspect guest postprocess
  and packaged binary provenance before changing KPU arithmetic.
- If a compare gets worse, keep the result as evidence; do not hide it by
  redefining the reference slice.

## Reporting Template

Use this shape for findings:

```text
Symptom:
Layer:
Reproducer:
Structural gate:
First divergence:
Affected stores/ranges:
Reference comparison:
Likely cause:
Rejected hypotheses:
Next action:
Artifacts:
```

For code changes, finish with:

- exact files changed.
- qtests and subtest counts.
- runtime gate result.
- store/reference metric movement.
- whether the change is behavior-changing or trace-only.
- whether it is committed or still staged/uncommitted.
