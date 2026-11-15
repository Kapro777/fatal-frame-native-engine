# Bring-up tools

Diagnostic tooling for the port, kept in the engine repository so any
engineer (or agent) with repository access can reproduce the analysis. No
game code or assets here — the scripts work on a user-owned XBE copy and
on the generated C, which the build pipeline publishes privately.

| File | What it does | Needs |
|---|---|---|
the title executable (an owner-held copy; CI can extract it from the input store)
| `analyze_gen.py` | Dump a function from the generated C, find every access to a field offset, list callees, locate the function owning a VA | Python 3; the generated C (CI artifact `generated-c-*`) |
| `safe-run.ps1` | Run a built exe constrained (idle priority, one core, bounded by the runtime watchdog, hard kill, game-dir write snapshot) | Windows; the built exe + the game directory next to it |

## The iteration loop, in one place

1. Build: `gh workflow run build -R <owner>/<build-farm> --ref main`
   (or push an engine change — the engine repo is the fingerprint input).
2. Read the result: the job log ends with a **SMOKE_SUMMARY** line
   (`kernel_calls=N crashes=M bytes=...`) and the artifact
   `smoke-run-<run-id>` holds the full stderr/stdout of the bounded
   on-runner smoke run. That is the primary feedback channel — no local
   machine required.
3. Classify the endpoint from the log:
   * `[CRASH] ... Xbox VA of fault: 0x........` — the new frontier.
     `xbe_tools.py disasm` around the innermost return address on the
     guest stack shows the faulting instruction.
   * An unresolved-call storm (`RECOMP_ICALL` failures, "unresolved" in
     the log) — seed convergence: run the toolkit's `tools.seed_from_log`
     against the smoke-run's err.txt, add the entries to
     `engine/config/seed_functions.json`, rebuild.
   * A silent gap (kernel calls stop with no crash) — extend
     `engine/config/trace_functions.json` around the last address seen,
     rebuild, and read the trace from the next smoke run.
4. Engine-level modelling goes in `src/main.c` (the per-title host entry:
   register model, channel ack, VRAM window). Document every constant
   next to where it is used, with the disassembly or probe that proved it.

Full history and the reasoning behind each fix: `docs/bringup-log.md`
in the engine repository.