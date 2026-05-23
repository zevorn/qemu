---
name: k230-kpu-modeling
description: Use when developing, debugging, or analyzing the private K230 KPU/GNNE model in this QEMU repo, including remote K230 SDK and RT-Smart image builds, runtime KPU trace captures, .plan Python kmodel/command-stream analysis, nncase simulator correlation, TCG physwatch probes, and gdbstub debugging.
license: GPL-2.0-or-later
---

# K230 KPU Modeling Workflow

This workflow is for the private downstream K230 branch.  Do not present these
AI-assisted source changes as upstream-ready QEMU work; follow
`docs/devel/code-provenance.rst` and the repo `AGENTS.md`.

When this skill leads to QEMU builds or tests, also use the `qemu-build` and
`qemu-testing` skills.  Those require running builds/tests in sub-agents, not
in the main agent context.

## Ground Rules

- Keep analysis scaffolding, captures, dumps, and generated summaries under
  `.plan/k230-gnne-analysis/`.
- Do not commit `.plan/` artifacts.  Source commits should be small,
  bisectable, and evidence-driven.
- Prefer trace-only instrumentation before broad behavior changes.
- For KPU arithmetic/addressing changes, add a focused qtest before relying on
  a full RT-Smart workload.
- Treat unsupported dtypes/modes as diagnostic trace points first.  Avoid
  broad heuristics unless a trace, simulator observation, and small qtest all
  point to the same rule.

## Important Paths

- QEMU repo: `/Users/zevorn/qemu`
- Analysis scratch: `.plan/k230-gnne-analysis`
- Local RT-Smart images:
  - `/Users/zevorn/k230-project/images/rtt-image-classify/fw_payload.bin`
  - `/Users/zevorn/k230-project/images/rtt-big-face/fw_payload.bin`
  - `/Users/zevorn/k230-project/images/rtt-object-yolov8n/fw_payload.bin`
- Remote entry script: `~/remote-wsl.sh`
- Remote endpoint: `ssh -p 23333 zevorn@e17157889b1b4650.natapp.cc`
- Remote SDKs:
  - `/home/zevorn/k230_sdk`
  - `/home/zevorn/k230_sdk-codex-rtt`
- Preferred local GDB: `/opt/homebrew/bin/riscv64-elf-gdb`
  (`gdb-multiarch` / `multiarch-gdb` may not be installed).

## Remote SDK And Image Builds

Use the remote SDK when new RT-Smart workloads, kmodels, or packaged images are
needed.

1. Connect:

```sh
~/remote-wsl.sh
```

2. Build an AI POC app on the remote SDK:

```sh
cd /home/zevorn/k230_sdk-codex-rtt/src/reference/ai_poc
./build_app.sh object_detect_yolov8n
```

`build_app_sub.sh` removes and recreates `k230_bin`, so copy aside anything
important before building another demo.

3. Stage the selected app into the RT-Smart root tree.  For QEMU, prefer local
image-input demos and an `init.sh` command that does not require ISP/camera
hardware, for example:

```sh
./ob_det.elf yolov8n_320.kmodel 0.15 0.2 bus.jpg 0
```

4. If `fw_payload.bin` still contains stale content or `/bin/init.sh` is
missing, regenerate `romfs.c` before relinking the image:

```sh
cd /home/zevorn/k230_sdk-codex-rtt/src/big/rt-smart
python3 tools/mkromfs.py userapps/root kernel/bsp/maix3/applications/romfs.c
cd /home/zevorn/k230_sdk-codex-rtt
make rtt_update_romfs
```

The key lesson from the YOLOv8n bring-up: copying an ELF into the SDK source
tree is not enough.  The binary actually packaged into `fw_payload.bin` must
be present in the RT-Smart image root and `applications/romfs.c` must include
the final `/bin` entries.

5. Copy refreshed images back locally:

```sh
mkdir -p /Users/zevorn/k230-project/images/rtt-object-yolov8n
scp -P 23333 \
  zevorn@e17157889b1b4650.natapp.cc:/home/zevorn/k230_sdk-codex-rtt/output/k230_canmv_rtt_evb_defconfig/images/fw_payload.bin \
  /Users/zevorn/k230-project/images/rtt-object-yolov8n/fw_payload.bin
scp -P 23333 \
  zevorn@e17157889b1b4650.natapp.cc:/home/zevorn/k230_sdk-codex-rtt/output/k230_canmv_rtt_evb_defconfig/images/rtthread.bin \
  /Users/zevorn/k230-project/images/rtt-object-yolov8n/rtthread.bin
```

If those exact remote image paths differ, locate `fw_payload.bin` and
`rtthread.bin` under the same `output/k230_canmv_rtt_evb_defconfig/images`
tree and copy the matching pair.

6. Verify the packaged app through QEMU UART, not only file mtimes.  The
YOLOv8n checkpoint was considered valid only after UART showed the new app
banner and no longer printed `/bin/init.sh: command not found`.

## Static Kmodel And Command Analysis

Use `.plan/k230-gnne-analysis` scripts to understand the model before changing
device behavior.

Extract kmodel sections:

```sh
.plan/k230-gnne-analysis/inspect_kmodel.py \
  .plan/k230-gnne-analysis/complex-kmodels/yolov8n_320.kmodel \
  --extract-dir .plan/k230-gnne-analysis/complex-dumps/yolov8n_320
```

Decode command text:

```sh
.plan/k230-gnne-analysis/decode_gnne_text.py --histogram
.plan/k230-gnne-analysis/decode_gnne_text.py \
  --fields --filter DM_LOAD_W --limit 20
```

Trace frontend state over a small PC window:

```sh
.plan/k230-gnne-analysis/trace_gnne_state.py \
  --memory .plan/k230-gnne-analysis/runtime-10000000-4m.bin \
  --glb-offset 0x20 --start 0x330 --end 0x3b0 --limit 80
```

Run the recovered frontend model with compute snapshots:

```sh
.plan/k230-gnne-analysis/execute_gnne_frontend.py \
  --memory .plan/k230-gnne-analysis/runtime-10000000-4m.bin \
  --glb-offset 0x20 --compute-detail --filter PU_COMPUTE --limit 4
```

Summarize whole K230 functions and complex corpora:

```sh
.plan/k230-gnne-analysis/summarize_k230_functions.py --frontend \
  .plan/k230-gnne-analysis/complex-kmodels/*.kmodel
```

Use `extract_k230_layouts.py` and `dump_k230_dll_enums.py` against
`Nncase.Modules.K230.dll` when operand bit layouts or enum values are unclear.
Those helpers need optional `dnfile` / `dncil` packages; install them in a
temporary venv outside the repo.

## Runtime Capture And Trace Debugging

Use `capture_gnne_run.py` for repeatable KPU captures.  It boots RT-Smart,
polls KPU command registers through HMP, saves the command stream, extracts
KPU trace lines, and can `pmemsave` GLB/DDR ranges.

Typical YOLOv8n capture:

```sh
python3 .plan/k230-gnne-analysis/capture_gnne_run.py \
  --qemu build/qemu-system-riscv64 \
  --bios /Users/zevorn/k230-project/images/rtt-object-yolov8n/fw_payload.bin \
  --timeout 70 --post-command-delay 40 --serial-index 3 \
  --dump .plan/k230-gnne-analysis/runtime-object-yolov8n-command.bin \
  --stdout .plan/k230-gnne-analysis/runtime-object-yolov8n-qemu.log \
  --trace .plan/k230-gnne-analysis/runtime-object-yolov8n-kpu-trace.log \
  --serial-file .plan/k230-gnne-analysis/runtime-object-yolov8n-uart3.log \
  --pmemsave 0x10000000:0x1000000:.plan/k230-gnne-analysis/runtime-object-yolov8n-glb-16m.bin
```

Event gate checklist:

- `k230_kpu_start` count equals `k230_kpu_gnne_summary` count.
- `k230_kpu_gnne_compute_summary` count matches submissions.
- `unknown 0` in `k230_kpu_gnne_summary`.
- No `*_skip` events unless the current change intentionally introduces a
  diagnostic skip.
- Runtime aggregate matches offline `summarize_k230_functions.py --frontend`
  for the same kmodel.

Summarize L2 stores and final-visible segments:

```sh
python3 .plan/k230-gnne-analysis/summarize_l2_store_outputs.py \
  --trace .plan/k230-gnne-analysis/runtime-object-yolov8n-kpu-trace.log \
  --dump .plan/k230-gnne-analysis/runtime-object-yolov8n-glb-16m.bin \
  --json .plan/k230-gnne-analysis/runtime-object-yolov8n-l2-store-summary.json \
  --markdown .plan/k230-gnne-analysis/runtime-object-yolov8n-l2-store-summary.md \
  --extract-dir .plan/k230-gnne-analysis/runtime-object-yolov8n-l2-store-segments
```

Compare against nncase reference outputs when available:

```sh
python3 .plan/k230-gnne-analysis/compare_yolov8n_reference.py \
  --segment-dir .plan/k230-gnne-analysis/runtime-object-yolov8n-l2-store-segments \
  --markdown .plan/k230-gnne-analysis/runtime-object-yolov8n-vs-nncase-output0-compare.md
```

For PU source-addressing work, keep `k230_kpu_pu_input_source` enabled and
summarize it:

```sh
python3 .plan/k230-gnne-analysis/summarize_pu_input_sources.py \
  .plan/k230-gnne-analysis/runtime-object-yolov8n-kpu-trace.log \
  --json .plan/k230-gnne-analysis/runtime-object-yolov8n-pu-input-source-summary.json \
  --markdown .plan/k230-gnne-analysis/runtime-object-yolov8n-pu-input-source-summary.md
```

Use store hashes to distinguish behavior changes from trace-only changes.  A
trace-only checkpoint should keep final-visible store hashes identical.

## GDB Stub Debugging

Start QEMU stopped at reset:

```sh
BIOS=/Users/zevorn/k230-project/images/rtt-object-yolov8n/fw_payload.bin \
TRACE_FILE=.plan/k230-gnne-analysis/k230-gdbstub-trace.log \
.plan/k230-gnne-analysis/run_qemu_gdbstub.sh
```

Connect with the local RISC-V GDB:

```sh
/opt/homebrew/bin/riscv64-elf-gdb -q \
  -x .plan/k230-gnne-analysis/gnne_probe.gdb
```

Useful GDB commands from `gnne_probe.gdb`:

```gdb
connect_k230
kpu_regs
b_known_k230_runtime
b_kpu_start_store
continue
```

Use `monitor xp` / `monitor pmemsave` through GDB when guest virtual mappings
are uncertain.  The `b_kpu_start_store` watchpoint depends on guest address
translation for `0x80400190`; if it fails, rely on QEMU trace events and
`kpu_regs`, which use physical monitor access.

Use specialized probe scripts when the question is about runtime data
provenance:

- `probe_runtime_arg_table.gdb`: dumps the runtime input/output argument table
  before `gnne_enable`.
- `probe_final_runtime_source.gdb`: stops on the final YOLOv8n runtime table
  and dumps source/output physical ranges.
- `probe_final_source_writer.gdb` and
  `probe_final_source_writer_blocks.gdb`: identify the guest CPU copy that
  fills the final source buffer.
- `probe_final_tensor_structs.gdb`: inspects tensor metadata around the final
  runtime call.

For physical write provenance, use the local TCG plugin:

```sh
python3 .plan/k230-gnne-analysis/capture_gnne_run.py \
  --plugin file=.plan/k230-gnne-analysis/physwatch.dylib,start=0x10944020,size=0x52000 \
  --pmemsave 0x10944020:0x52000:.plan/k230-gnne-analysis/final-source.bin
```

## Implementation Loop

1. Decode or trace the smallest observed missing behavior.
2. Add a trace if the data/source domain is ambiguous.
3. Add a tiny qtest with auditable expected bytes.
4. Implement the narrow rule in `hw/misc/k230_kpu.c` and trace-events as
   needed.
5. Build/test through sub-agents using `qemu-build` / `qemu-testing`.
6. Run a full RT-Smart capture and compare event gate, store hashes, and
   nncase/reference metrics.
7. Update `.plan/k230-gnne-analysis/findings.md` and `next-actions.md` with
   concrete artifact paths and numeric results.
8. Commit only the source files for the verified checkpoint, with the user's
   `Signed-off-by` trailer when requested.
