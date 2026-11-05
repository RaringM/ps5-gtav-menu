# Native nexHEN module

The native nexHEN delivery is implemented for GTA V Enhanced **PPSA04264,
01.010.002**. It is a hardware candidate for the corrected nexHEN v0.90 loader
bridge, not a compatibility claim for stock nexHEN. Stock v0.90 retains the
attach-based lifecycle path that has repeatedly damaged GTA streaming.

## Stock nexHEN v1.00 (target of the native work)

nexHEN v1.00 build 5 loads plugins GoldHEN-style: `/data/nexHEN/plugins.ini` binds
plugin paths to `[TITLEID]` or `[DEFAULT]` sections, a `?autoload` suffix loads the
plugin when the title starts, and the daemon resolves `plugin_load` and
`plugin_unload` by name (their return values are only logged). The `g_plugin*`
metadata is not read. For a native PPSA title the daemon still ptrace-attaches to
the running game, remote-calls `sceKernelLoadStartModule` and then `plugin_load`,
and detaches; unload remote-calls `plugin_unload` and then
`sceKernelStopUnloadModule`. This is the same attach path as v0.90, and an attach on
a steady-state 01.010.002 process permanently jams the streaming I/O completion
ring. An autoload at title start is not survivable either: on hardware, nexHEN
v1.00 loading even a do-nothing plugin about 11 s after launch makes GTA hang at
the "Updating" step after "Press X to Continue", before its save profile mounts,
while the same boot without a plugin loads normally. The cause inside the load
(stop, elevation, module load or thread hijack) is not yet isolated, so stock
nexHEN is not a supported delivery for this menu.

`NEXHEN_MODE` selects the module flavour. `broker` (the default, described below)
needs the corrected loader. `probe` is the stock-loader feasibility gate: a
lifecycle-only module (`src/module/nexhen_probe.c`) that installs no hooks and never
writes game memory. It logs how long after launch `plugin_load` ran, then samples
the streaming ring head/tail/in-flight once per second and scores 15-sample windows
HEALTHY/JAMMED with the `probe_streaming_state.py --verdict` rule:

```sh
make package-nexhen-probe   # stages build/pkg/ppsa04264-01.010.002/nexhen-probe/
```

The staged README carries the install and run procedure. A self-installing stock
module is built only if autoload proves HEALTHY under driving load.

## Build and package

Use PS5 payload SDK v0.44 and LLVM 20, as for the other production builds:

```sh
make package-nexhen
```

The command refuses other title/build profiles. It compiles the production menu
with custom packs disabled for this first native lane, links an `ET_SCE_DYNAMIC`
PRX, packages it as a fake SELF, validates its complete import/export and segment
layout, and stages:

```text
build/pkg/ppsa04264-01.010.002/nexhen/
  gtav_menu_nexhen.sprx
  native-validation.json
  package-manifest.json
  README.md
```

The package manifest records the exact artifact, target manifest and provider
hashes and states `hardwareValidated=false`. Version 1.0.2 validates with zero
undefined weak imports and exact `_init`/`_fini` agreement. Copy only the SPRX to
`/user/data/nexHEN/plugins/gtav_menu_nexhen.sprx` when running an approved test.

## Loader contract

Module startup is quiet. The module exports nexHEN's lifecycle metadata and
callbacks plus `gtav_nexhen_contract`, a versioned 312-byte broker contract. The
corrected ShellCore broker loads the native module, resolves and validates the
contract, verifies the exact game build, discovers the render-phase object twice,
and arms the operation. It then invokes `plugin_load` on the natural game-thread
runner, validates the joinable worker and patch broker, installs the exact frame
hook, and requires both the frame and render callbacks to advance before it
reports success.

The worker cannot self-install the external frame hook. This keeps module code
separate from loader correction and gives the broker exact ownership of the
entry patch. The module owns its worker, menu state, render wrapper, and all
ordinary feature cleanup.

For unload, the broker restores the frame-hook entry first and waits for its
in-flight counter to drain. `plugin_unload` then requests cooperative shutdown,
joins the exact worker, restores the render-phase slot and internal state, and
waits for both callback counters to remain idle. Only after it verifies the
original hook bytes, original render slot, stopped worker and restored broker
state does it remove the native module.

Every module-owned restore now contributes to the shutdown result. The pad GOT
and native-handler slots are restored only from the exact owned replacement and
are read back as original. Render, frame-hook and detour failures also propagate.
A failed stop latches the module lifecycle as `FAILED`, retains callback
passthrough pointers and logging, and rejects later unload retries in that game
process. This gives the broker a reliable nonzero callback result before it
asks firmware to remove the module.

An activation or restoration ambiguity quarantines the operation and keeps the
module mapped. A lost reply never causes automatic replay. The firmware's module
stop result is not used as an unload veto; the broker enforces the lifecycle
policy before asking the firmware to remove the module.

## Current boundary

Host harnesses cover activation, contract mutation, callback failure, cleanup,
ledger outcomes, fragmented transport, two load bases, manual/autoload callers,
the historical unload shortcut and ASan/UBSan execution. The linked PRX validator
checks FreeBSD ABI2, SCE program headers, W^X load segments, exact exports,
bounded provider imports, zero undefined weak imports, exact dynamic
initializer/finalizer values, and its embedded fake-SELF header.

The first hardware attempt completed the no-module runner probe, then reached
the prior two-second loader observation limit with known module absence before
any callback or activation was observed. The second attempt's longer wait
allowed firmware to map the module at `0x806a0000`. The broker then rejected the
otherwise matching contract because it assumed module pointers were at least
four GiB and all counter pointers were eight-byte aligned. It stopped before
`plugin_load`; gameplay remained normal and GTA exited cleanly.

The paired v9 service/v7 payload corrected those broker assumptions and loaded
the real version 1.0.1 menu on hardware. The contract reached `ACTIVE`, the
worker reached `RUNNING`, frame/render callbacks advanced, the menu worked,
gameplay remained healthy and GTA exited normally. The operator's UI off state
never reached `plugin_unload`: v0.90 ShellUI sent load-only command 5 again, so
the module correctly remained active.

The current v10 payload patches that exact ShellUI branch to send command 32
with an exact boolean. The strengthened version 1.0.2 module's SPRX SHA-256 is
`5198dda9ea3a4e3b49b11e2da11a50b9f36f03294f8df3102f44f5fa44510777`.

That exact pair subsequently completed one hardware UI load/unload on PS5 10.01,
GTA `PPSA04264` `01.010.002`. Load reached `ACTIVE` with a `RUNNING` worker and
advancing frame/render/draw counters. The off row reached the bridge as action
`UNLOAD`; callback, module, activation and recovery results were zero; cleanup
was verified; presence was known absent; and no quarantine was set. Read-only
post-unload checks found the module mapping absent, the original 16-byte frame
prologue restored, and the original render callback in its slot. The menu chord
no longer opened the menu, brief moving gameplay stayed healthy, and GTA exited
normally.

This establishes the scheduler, loader/TLS and quiescence path for one short
cycle on the pinned tuple. It does not establish sustained or repeated safety.
Three load/unload cycles, moving/new-scenery gameplay after each action, a clean
exit/relaunch and one cycle in the relaunched process remain required before
calling unload dependable. Any future uncertain operation still stops the run
without replay in that process.
