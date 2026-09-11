# Apple Events and AppleScript — an agents-first control surface

The Mac port has a window, sound and a snapshot, and the only way to drive
any of it from outside is a QMP socket and whatever the agent already knows
about QEMU. This is the design for the surface the app should grow instead:
**Apple Events, with an sdef so AppleScript and JavaScript for Automation
speak it natively — designed for an AI agent as the first and primary
scripter**, with humans as a pleasant side effect. It is also the design for
the two things that surface needs before it can work at all: an app bundle,
and (for the unattended agent to survive it) Gatekeeper signing and
notarization.

> The design principle, landing in a new place: rather than exposing QEMU,
> we should expose the functions needed to control and debug this machine …
> be as curated and typed as possible, and expose raw QEMU only if we are
> desperate. We do need a consent model though — the whole surface is
> running off trust.

Nothing here is started. Everything in §4 and §5 is written against the
code as it stands, with the line numbers checked; everything in §11 is
genuinely open and gates the sprints in §9.

## 1. Why Apple Events at all

What an agent can do to the app today, and what each path costs:

| Path | Has | Lacks |
| --- | --- | --- |
| QMP over TCP (`run-macos.sh` opens 127.0.0.1:4455) | everything QMP has: `stop`, `cont`, `screendump`, `human-monitor-command`, `savevm` | a client that speaks the greeting/capabilities dance; identity (dies with the PID); any consent model |
| HMP through that | `x/i`, `info registers`, the lot, via `human-monitor-command` | same as above, plus parsing prose |
| gdbstub | breakpoints, single-step | a gdb/lldb client per session; no app-level state |
| `-trace`, logs, `ticks.py` | measurement | read-only, host-side files |
| The Metal window | the only screenshot worth looking at, input | a human |

Apple Events close the gaps that matter to an agent host:

- **The client is already installed.** `osascript` is on every Mac, and
  `osascript -l JavaScript` (JXA) speaks JSON-shaped calls to an sdef
  without a line of AppleScript. An agent with a shell needs nothing else.
- **Identity.** The app is addressed by bundle id, which survives restarts
  and reboots; a QMP socket is a port number tied to a PID.
- **Consent.** macOS gates Apple Events between apps with TCC: the *sender*
  is prompted once, keyed to both identities. Signed with a stable
  Developer ID, that consent survives app updates — which is exactly the
  property an unattended agent needs and a bare QEMU port number has never
  had. §3 explains why this puts signing early, not last.
- **Discoverability.** The sdef is a machine-readable dictionary of every
  command, argument and reply. It is the interface *and* the documentation,
  read live by Script Editor and offline by any agent that can open a file.
- **The platform's future comes free**: a Shortcuts action set is the same
  sdef plus one `.intentdefinition`, later, if ever wanted.

QMP stays — for the developer. It is the deep, verbose, cross-platform
channel the tools already use (`probe.py`, `ticks.py`, `run.py`), and
nothing here replaces it in that persona. The Apple Event surface is the
curated, typed, discoverable façade over the same core — and where QMP
already has the function, this calls the same C entry points in-process
rather than proxying text through a socket. But the two channels have
different perimeters and only one of them is governed: a TCP socket on
127.0.0.1 lets *any* local process drive the machine with no prompt,
while Apple Events carry TCC's per-sender consent. **The shipped app
persona therefore leaves QMP off unless explicitly asked for** — the
Apple Event surface is the only always-on way in — and §8's E5
notarizes that shape. The two personas are the product plan, not a
compromise: the app is for users emulating RISC OS with some
automation, and the developer workbench of this same code embeds into
an IDE with editors and tools (SPRINTS' tier two), where the deep
channels and the shared window live. (For the record of §3:
notarization is a malware scan with no policy against listening
sockets — Chrome's CDP port ships notarized — so this is a design
decision about consent, not an Apple requirement.)

## 2. AGENTS FIRST, and what that dictates

The first consumer of this surface is not a person writing a script; it is
an LLM agent with a shell, driving a machine it has never seen, verifying
what it did, and recovering from what went wrong. Each clause of that
sentence buys a rule:

1. **Discoverable without documentation.** `describe` returns the whole
   command table as JSON — every command, its arguments, types, enums,
   error codes — and the same table generates the sdef and the repo's
   schema file. One source of truth; an agent that can run one command can
   find all of them.
2. **Replies are data.** Every reply is a JSON envelope (`§6`), not prose.
   Agents parse JSON; they tolerate AppleScript records badly and generate
   them worse.
3. **No dialog is ever shown on the scripting path.** An agent cannot
   click. Destructive operations are gated by an explicit `dangerous
   allowed` property (default off), never by a modal.
4. **Counters beat pixel-diffing.** "Did my input land? Did the screen
   change?" is answered by monotonic counters — framebuffer generation,
   frames presented, input events injected — rather than by comparing
   screenshots. The counters already exist inside the front end; this
   surfaces them (`§5`).
5. **Every wait is bounded.** Every command takes `waiting` seconds
   (default 10, max 60). On timeout the reply says so and the operation
   still completes; the next `machine state` reflects it. An agent never
   wedges on us.
6. **Single-flight.** One command executes at a time; concurrent callers
   get `busy`. Agents cannot reason about interleaved mutations, so there
   are none.
7. **Errors are values.** A small taxonomy (`§6`) with stable string codes
   and AE error numbers; the message is for the log, the code is for the
   retry logic.
8. **Causality you can hold.** `capture` takes PC, registers, a
   screendump and the counters in one atomic pause — the agent's
   observation is coherent by construction, not by luck.

JXA is the expected dialect, and it is worth saying why: an agent writes
`app.readMemory({address: 0xFC200000, length: 64})` far more reliably than
AppleScript, and JXA replies arrive as JavaScript values with no parsing at
all. AppleScript remains first-class — the sdef serves both — but examples
in this design are JXA.

## 3. Prerequisites: the bundle, and why signing is not the last sprint

The app today is an unbundled executable — `build-macos/qemu-system-aarch64`
with `-display metal` — and that is why `MACOS.md` could say, of the grab
hook, that this is "an unsigned, unbundled process". Apple Events need
more:

- **A `.app` bundle** with an `Info.plist` carrying
  `OSAScriptingDefinition` (the `.sdef` filename) and
  `NSAppleScriptEnabled`, plus the ordinary plumbing (principal class,
  activation policy — already `Regular` in code, icon, minimum system
  version). `tell application id "…"` and JXA `Application("…")` address
  the bundle id; there is no stable way to target a bare executable.
- **A bundle id, chosen once**: provisionally
  `com.github.albanread.RISCOSQEMU-A72`, revisable until the first signed
  release and frozen from there. Every consent, bookmark and agent habit
  hangs off it.
- **No ROM inside the bundle.** The licence position in `README.md` is
  unchanged: the ROM and card image stay outside, in `riscos-images/` or a
  directory the settings name, and the bundle ships without them.

Signing order is the counter-intuitive part, and it is an agents-first
conclusion, not a deployment afterthought:

> TCC keys the Automation consent on the code identities of both ends. An
> ad-hoc or unsigned build has a different cdhash after every rebuild, so
> the agent host is re-prompted — or, unattended, silently denied — each
> time the app is rebuilt. A Developer ID identity with a notarized ticket
> makes the one prompt the last one, across versions.

So the signing sprint (§9, E5) lands before the surface is called done,
and the dev loop until then carries its workaround on paper: run the app
locally with `xattr -dr com.apple.quarantine`, and expect one Automation
prompt per rebuild.

Hardened runtime needs one decision from the wire, not from theory: TCG
generates code at run time, so the entitlement set is at minimum
`com.apple.security.cs.allow-jit` and possibly
`com.apple.security.cs.allow-unsigned-executable-memory`. Minimal first,
add only what a measured failure demands (§11, Q4). Library validation
stays on — this build `dlopen`s nothing. Notarization is `notarytool
submit` + `stapler`, scripted, with the credentials in the keychain.

## 4. The shape in code

The Metal front end already has the shape this needs — a boundary file
that is the whole contract (`ui/metal.h`), C glue on one side, Cocoa on
the other, and a threading discipline that has held since the input path.
Scripting joins it as one new call across that boundary and one new file
each side:

```
ui/metal_script.m   Cocoa side: NSAppleEventManager registration, Apple
                    Event descriptor <-> JSON marshalling, reply and
                    error descriptors. Registered from
                    metal_backend_init(); handlers fire on the main
                    thread, inside the existing pump.
ui/metal.c          C side: the command table and the executor —
                    metal_glue_script(const char *json, char **reply).
                    fast commands run under bql_lock() exactly as
                    metal_glue_key does; the rest schedule a bottom half
                    exactly as metal_glue_load_snapshot does.
ui/metal.h          + metal_glue_script(), metal_glue_counters()
tools/mksdef.py     parses the C table, writes the .sdef and a JSON
                    schema artifact into the repo — the mkcmos.py
                    precedent of parsing a source file as the single
                    source of truth, applied to our own table.
```

Two implementation routes were considered. Cocoa scripting — `NSScriptCommand`
subclasses bound through the sdef — buys an object graph with KVC, and is
the right answer for a document-based app. This app is one machine and a
set of verbs; there is no object graph worth binding, and the hand-rolled
`NSApplication` (no nib, menus in code, its own event pump at
`ui/metal.m:1413`) would fight the machinery for nothing. So: **manual
`NSAppleEventManager` handlers keyed by the sdef's four-character codes**,
with the sdef supplying terminology only. The one standard event we also
take is `quit`, and it must route to the clean power-off the window close
already uses (`m.lost = true`, which unwinds the loop and lets the disc be
written back) — never `-[NSApp terminate:]`, for the reason the
`quitAction:` comment at `ui/metal.m:1245` records.

**Threading rules**, which are the whole safety story:

- Handlers run on the main/UI thread. The pump drains
  `nextEventMatchingMask` in `NSDefaultRunLoopMode`, which is where
  high-level events are delivered — *to be proven first* (§11, Q1).
- The command table marks each command `fast` or `bh`. `fast` commands run
  synchronously under `bql_lock()` from the handler — reads and counter
  fetches, the same discipline as the input glue at `ui/metal.c:149-218`.
  `bh` commands schedule a bottom half on the main loop (the
  `metal_loadvm_bh` pattern at `ui/metal.c:251-273`) and wait on a
  semaphore for up to `waiting` seconds.
- The waiting UI thread holds no lock while it waits. A bottom half never
  calls back into the UI thread. Those two rules are what keep the
  semaphore wait deadlock-free; they are written here so a reviewer can
  check them against every command added since.
- Commands are serialised by one in-process mutex: single-flight, `busy`
  for the loser.

JSON is QEMU's own (`qobject_from_json` / `qobject_to_json`); no new
dependency.

## 5. The surface

### The envelope

Every reply, success or failure, is one JSON object in a UTF-8 text
descriptor:

```json
{"ok": true,  "data": { … }, "elapsed_ms": 4,
 "machine": {"state": "running", "frame_gen": 10, "frames": 16234}}

{"ok": false, "error": {"code": "wrong-state", "number": 2,
                        "message": "the machine is not running"}}
```

The `machine` block rides on every reply so an agent's next decision needs
no extra round trip. `state` is `runstate_get()` mapped to its QAPI name;
`frame_gen` is the framebuffer generation (`bcm2835_fb_get_config`,
`ui/metal.c:64`) and `frames` the presented-frame count — the two numbers
that answer "did anything change".

### Errors

| code | number | meaning |
| --- | --- | --- |
| `busy` | 1 | another command is in flight |
| `wrong-state` | 2 | e.g. power off while `prelaunch` |
| `timeout` | 3 | `waiting` expired; the operation continues |
| `invalid-argument` | 4 | bad address, length over the cap, … |
| `not-capable` | 5 | no framebuffer yet, no audio backend, headless run |
| `not-found` | 6 | unknown snapshot, breakpoint id |
| `denied` | 7 | path outside the configured directories, `dangerous` unset |

The AE reply carries `number`/`message` in the standard error descriptors,
so `osascript` reports them natively.

### Commands

Four-character codes: the suite is `MQem`; command codes are mixed case as
allocated below and never all-lowercase (Apple reserves those). Class is
`fast` (BQL on the handler thread) or `bh` (bottom half, waited).

**Application**

| AE name | code | class | Arguments → reply |
| --- | --- | --- | --- |
| `describe` | `Desc` | fast | — → the compiled command table as JSON |
| `ping` | `Ping` | fast | — → `{version, qemu, branch, bundle, pid}` |

**Machine state and counters**

| AE name | code | class | Arguments → reply |
| --- | --- | --- | --- |
| `machine state` | `Stat` | fast | — → state, uptime (virtual ns), frame gen, frames, key/mouse/AE counters, display mode (`xres×yres×bpp` from the fb config) |
| `set video options` | `VOpt` | fast | `scaling`, `scanlines`, `vsync` → as applied |

**Lifecycle** (all `bh`, all but pause/resume gated by `dangerous allowed`)

| AE name | code | Notes |
| --- | --- | --- |
| `power off` | `Powr` | the clean path: `metal_backend_request_shutdown` semantics, disc written back |
| `reset` | `Rset` | `qmp_system_reset` |
| `pause` | `Paus` | `qmp_stop` (`monitor/qmp-cmds.c:50`) |
| `resume` | `Resu` | `qmp_cont` (`monitor/qmp-cmds.c:66`); refuses if the guest is mid-shutdown |
| `quit` | `aevt/quit` | the standard event, routed to the clean power-off |

**Snapshots** (`bh`)

| AE name | code | Notes |
| --- | --- | --- |
| `save snapshot` | `SnSv` | `save_snapshot` (`include/migration/snapshot.h:32`); named; `overwrite` needs `dangerous` |
| `load snapshot` | `SnLd` | `load_snapshot` + resume, the menu's bottom half generalised past its hard-coded `"desktop"` |
| `list snapshots` | `SnLs` | parsed `info snapshots` in v1 |

**Screens**

| AE name | code | class | Notes |
| --- | --- | --- | --- |
| `screendump` | `Dump` | bh | guest framebuffer PNG, the truth: `qmp_screendump` (`ui/ui-qmp-cmds.c:324`) |
| `screenshot` | `Shot` | fast | the decoded Metal surface, what a human sees incl. scanlines: the `metal_screenshot` path (`ui/metal.m:883`) generalised to a caller-chosen path |

Both write only inside the configured directories (§7). Reply carries the
path and the `frame_gen` it was taken at.

**Input** (`fast`; same injection points the window uses)

| AE name | code | Notes |
| --- | --- | --- |
| `send key` | `KeyD` | macOS virtual keycode + down/up — `metal_glue_key` verbatim |
| `type text` | `Type` | ASCII v1, via the osx keymap reversed (US layout, documented); unicode is §11 Q7 |
| `move mouse` | `Mous` | guest-pixel absolute — `metal_glue_mouse_abs` |
| `click mouse` | `ClkM` | button 0/1/2 — `metal_glue_mouse_btn` |
| `scroll` | `Whl ` | notches — `metal_glue_mouse_wheel` |

**Debugging** — the part the user asked for by name

| AE name | code | class | Notes |
| --- | --- | --- | --- |
| `do HMP command` | `Hmp ` | bh | `qmp_human_monitor_command` (`monitor/qmp-cmds.c:165`) verbatim: `x/i`, `info registers`, `xp`, `info mtree`, `qom-list`, `gpa2hva`, … the whole HMP on day one |
| `read memory` | `RdMe` | fast | `address`, `length` (≤ 1 MiB), `virtual` bool, `core`. Physical via `cpu_physical_memory_read`; virtual via `cpu_memory_rw_debug` (`include/hw/core/cpu.h:706`). Reply hex words or base64 |
| `read registers` | `RdRg` | fast | v1: the `info registers` text per core; v2: curated JSON from `ARMCPU` |
| `read PC` | `RdPc` | fast | `env.regs[15]` of a core, under BQL |
| `disassemble` | `Disa` | bh | `x /<n>i <addr>` sugar over the HMP bridge, capstone underneath |
| `capture` | `Capr` | bh | pause → PC + registers + screendump + counters → restore prior state: one coherent observation |
| `set breakpoint` | `BrkS` | bh | `cpu_breakpoint_insert` (`include/hw/core/cpu.h:1152`) with `BP_GDB`, core 0 v1; reply an id |
| `list breakpoints` | `BrkL` | fast | walk `cpu->breakpoints` (`include/exec/breakpoint.h`) |
| `remove breakpoint` | `BrkR` | bh | by id or address |
| `step` | `Step` | bh | `cpu_single_step` (`include/hw/core/cpu.h:1139`) + continue-until-stopped, the gdbstub semantics; deepest item, E4's own sprint |

Virtual-address reads of a running machine see the monitor's usual
torn-page semantics; `capture` is the answer when that matters. This is the
same trade the HMP makes, said out loud because agents will lean on it.

### The command table is the contract

The table lives in `ui/metal.c` as a C array — name, code, class, argument
schema, one-line help — exactly as `trace-events` is the contract for trace
points. `describe` serialises it at runtime; `tools/mksdef.py` parses it at
build time into the `.sdef` and a committed JSON schema for agent
documentation. Nothing is hand-written twice; the sdef cannot drift from
the code because it is generated from the thing the code dispatches on.

## 6. Semantics the agent can rely on

- **States**: the QAPI `RunState` names (`prelaunch`, `running`, `paused`,
  `shutdown`, …) via `runstate_get()` (`include/system/runstate.h`). The
  app is single-machine by construction; a second machine is a second app
  instance, with the targeting caveat in §10.
- **Ordering**: single-flight makes commands a total order. A timeout does
  not cancel the operation — it only stops the *waiting*; the next
  `machine state` reports what happened.
- **Destructive gating**: `power off`, `reset`, snapshot `overwrite` set
  `dangerous allowed` first or fail `denied`. The property is per-command,
  never sticky beyond the call.
- **`capture` atomicity**: everything is gathered while the vCPUs are
  stopped inside one bottom half — no DMA, no timer, no input can
  interleave. This is SPRINTS 15's frame-coherence idea delivered where it
  is cheapest, for the debugging observer rather than the compositor.
- **Counters are monotonic per boot** and included in every envelope; an
  agent proves causality by reading one before and after.

## 7. Security: the lines this does not cross

The surface can inject input, read guest memory, pause the machine and
write screenshots and snapshots. It must not be a host escape by another
name, so:

- **No host command execution.** There is no `do shell`, no arbitrary
  QEMU reconfiguration, no `-drive` swapping from events in v1.
- **Writes are confined.** Screenshots and screendumps land inside a
  configured directory list (default: the images directory and
  `~/Library/Application Support/RISCOSQEMU/`); settings may add
  directories; anything else is `denied`.
- **Local session only.** No support for remote Apple Events; §11 Q6 asks
  whether a received event's sender attribute lets us *refuse* network
  senders cheaply, and until then the host's Remote Apple Events setting
  is documented as part of the threat model.
- **A kill switch**: `scripting=off` in the settings file (and a
  `-display metal,scripting=off` suboption), default on.
- **One always-on channel**: the shipped app persona leaves QMP off
  unless asked (§1) — a localhost socket is a consent-free control
  channel, and the point of this surface is that its perimeter is TCC's.
- **An audit trail**: every command logged to `metal-debug.txt`
  (rate-limited), which is where the front end's failures already land.
- **TCC does the perimeter**: the *sender* is prompted by macOS for
  Automation consent against our bundle id; we add nothing to that and
  bypass nothing.
- **Input flood**: `type text` and key events are coalesced per run-loop
  turn, as the window's own auto-repeat filtering already does.

## 8. Signing and notarization, as a plan

1. Assemble the `.app` (meson custom target or a `scripts/` step): binary,
   `Info.plist`, `.sdef`, icon; the bare-binary dev path stays working.
2. Ad-hoc sign for local dev (`codesign -s -`), so the TCC prompt shape is
   testable before Developer ID lands.
3. Minimal hardened-runtime entitlements; add only on measured failure
   (expect `allow-jit`; hope not to need
   `allow-unsigned-executable-memory`).
4. Developer ID Application signing, `notarytool submit`, `stapler`;
   credentials in the keychain, never the repo.
5. Verify the two properties that matter to an agent: a quarantined copy
   (a downloaded zip) opens without interaction; the Automation consent
   from the previous build still holds.

## 9. Sprints

Days are estimates in this project's usual sense — a budget, not a
promise. E0's answers gate E1's shape; nothing else starts before them.

- **E0 — prove delivery on the wire (1–2 d). *Done; see §13.*** One
  handler (`ping`), one minimal bundle, real `osascript`/JXA calls.
  Settle Q1 (pump delivery), Q2 (round-trip latency vs QMP — publish
  the number), Q3 (TCC behaviour across ad-hoc rebuilds). Done when
  `osascript -l JavaScript -e
  'Application("…").ping()'` answers from the real front end, with the
  three measurements written down.
- **E1 — the bundle and the table (2–3 d). *Done; see §14.*** Proper
  `.app`, `Info.plist` keys, the C command table, `mksdef.py`,
  `describe`, the envelope, the error taxonomy, single-flight,
  timeouts. Done when the sdef opens in Script Editor with real
  terminology, and `describe` returns the table an agent can drive the
  machine with.
- **E2 — control (2 d). *Done; see §15.*** Lifecycle, pause/resume,
  snapshots, both screenshots, the input set, counters in the envelope.
  Done when a script — written by an agent from `describe` alone, no
  human — cold boots, waits for the desktop by polling `frame_gen`,
  takes a screendump, saves a snapshot, loads it.
- **E3 — debugging (3 d).** HMP passthrough, `read memory` both
  address spaces, registers, PC, disassemble, `capture`. Done when a
  scripted session pauses a running desktop and its `capture`'s
  disassembly-at-PC matches the same `x/i` over QMP.
- **E4 — breakpoints and step (2–3 d).** Insert/list/remove on core 0,
  `step` with the gdbstub wait semantics, breakpoint-hit visible as a
  state change. Done when a scripted session breaks at a chosen physical
  address (the VCHIQ doorbell write is the test) and steps off it.
- **E5 — signing and notarization (2 d).** §8 through step 5, scripted.
  Done when the two agent-facing properties hold and are written down.
- **E6 — the agent kit (1–2 d).** `tools/ae-cookbook.md` with osascript
  and JXA examples in the project's voice (boot-and-wait, capture-a-bug,
  snapshot round trip); optionally `aerun`, a PID-addressed raw-AppleEvent
  sender for multi-instance labs. Done when the cookbook's longest example
  runs end to end unmodified.

## 10. What we deliberately do not do

- **No Cocoa object-model scripting.** Commands, not a KVC graph; the
  manual-handler route is smaller, reviewable beside the rest of the front
  end, and loses nothing a single-machine app would use.
- **No AppleScript records as replies.** JSON in a string; JXA and
  AppleScript both read it, and every agent host parses it.
- **No remote Apple Events.** Local session only, §7.
- **No multi-instance targeting in v1.** `tell application id` selects one
  instance per bundle id; running several machines means several app
  copies with distinct bundle ids, or E6's `aerun` addressing a PID.
  Stated rather than solved.
- **No async tickets.** Synchronous with `waiting`; `osascript` is
  synchronous, and an agent's retry loop is a fine poller.
- **No Shortcuts actions in v1.** The sdef makes them cheap later; nothing
  now.
- **No screen capture of the host.** The guest framebuffer and the decoded
  surface are read out of memory the app already owns — no
  screen-recording permission, no CGWindowList, nothing to prompt about.

## 11. Open questions, to be settled on the wire

Questions 1–3 were settled by Sprint E0; §13 records the answers. The
rest stand.

1. **Does the manual pump deliver Apple Events?** Handlers fire on the
   main thread when `sendEvent:` dispatches the high-level event; the pump
   (`nextEventMatchingMask … inMode:NSDefaultRunLoopMode`,
   `ui/metal.m:1417`) is not `-[NSApp run]`. Expected yes; if no, the
   fallbacks are a run-loop source serviced per frame or, last resort,
   `-[NSApp run]` with the render on a `CADisplayLink`-shaped timer —
   cost measured before choosing.
2. **Round-trip latency**, `ping` over Apple Events vs `query-status`
   over the QMP socket. If AE is ≥ 50 ms, the cookbook says batch; either
   way the number goes in `MACOS.md`.
3. **TCC stability across rebuilds**: ad-hoc re-prompt per cdhash
   (expected) and Developer ID consent surviving an update (expected) —
   two builds, measured, because the sprint order leans on it.
4. **Hardened runtime vs TCG**: is `allow-jit` alone enough on this
   build? Minimal-first, add on failure only.
5. **Reply size ceiling** for the day PNG bytes ride in the reply
   descriptor; v1 uses paths, but the bound should be known, not found.
6. **Remote sender detection**: can the received event's address attribute
   distinguish a network sender cheaply enough to refuse? If not, §7's
   documentation stand is the answer.
7. **Unicode `type text`**: does a `CGEvent` carrying
   `CGEventKeyboardSetUnicodeString`, posted to our own window, arrive
   layout-independently — or does the guest need a unicode-aware path of
   its own? ASCII first either way.
8. **Breakpoints across processor modes**: RISC OS remaps the vector
   table and runs supervisor and user code at the same virtual addresses;
   verify a `BP_GDB` at an address stops where an agent expects, or
   whether the command needs a mode qualifier in v2.

## 12. Where it meets the rest of the plan

This surface is **Sprint 8's substrate on macOS** — the RISC OS-aware
debugger wants module lists, Wimp task lists and symbol-aware breakpoints,
and this is its transport; the curated register/JSON work lands here first
and the RISC OS awareness layers on top. The **settings file** is Sprint
17's, shared: the scripting section and the writable-directory list live
in the one JSON document, generated help and all. **`read console`**
arrives with FSDESIGN 6C's console capture, over this surface as well as
the portal's. And **`capture`** is SPRINTS 15's frame-coherence sprint
delivering its first consumer early, at the observer rather than the
compositor, where it costs one pause instead of a rendering pipeline.

## 13. E0, as built

One command is on the wire: `ping`, answered from inside the real front
end with the full envelope — `{"ok":true,"data":{"app":"RISCOSQEMU",
"qemu":"11.1.0","pid":…,"sprint":"E0"},"elapsed_ms":0,"machine":
{"state":"running"}}` — and the machine block already rides it. The
shape is §4's: `ui/metal_script.m` registers one `NSAppleEventManager`
handler at backend init and marshals; `metal_glue_script()` in
`ui/metal.c` dispatches and builds the envelope, taking the BQL for the
state read exactly as the fast class is specified to. The bundle is
assembled by `riscos-pi4/tools/make-bundle.sh` from `riscos-pi4/app/`
(Info.plist with `NSAppleScriptEnabled` + `OSAScriptingDefinition`, and
the hand-written E0 sdef; E1's `mksdef.py` replaces the sdef with the
generated one). The three measurements:

- **Q1, delivery: positive.** A ping sent from a user Terminal — after
  one Allow click — reached the handler through the hand-run
  `nextEventMatchingMask` pump and the reply came back; the log shows
  `apple event: command (ping)` firing inside the front end. No run-loop
  fallback needed; §11's first and sharpest risk is closed.
- **Q2, latency: 16.7 ms per warm Apple Event round trip, against
  0.1 ms for `query-status` over the QMP socket** (50 warm round trips
  each, single connection, JXA holding the application object). A
  factor of ~160: comfortably under the 50 ms bar that would force
  batching for interactive use, and the definitive statement of the
  division of labour — one-command control and inspection over Apple
  Events, chatty loops (input streams, polling) over QMP.
- **Q3, consent across rebuilds: survived, measured once.** After a
  real rebuild and re-sign of the ad-hoc bundle, an already-granted
  sender pinged the new binary with no re-prompt. The design expected
  cdhash-keyed fragility; this host (an unsigned-but-valid ad-hoc seal)
  kept the grant across a changed binary. E5's Developer ID remains the
  durable guarantee and the notarization work stands.

Two lessons from the wire, both already folded into the tree:

- **An sdef is XML first**: a double hyphen inside a comment — the
  file's own title line, `RISCOSQEMU.sdef -- the scripting dictionary`
  — made osascript reject the dictionary as corrupt (`-2705`) before
  any event moved. The dictionary's comments now say why they are
  written the way they are.
- **A stale seal kills at launch.** Re-copying a changed executable
  into an existing bundle without forcing a fresh signature gave
  `SIGKILL (Code Signature Invalid, "Taskgated Invalid Signature")` —
  and a swallowed `codesign … || true` hid it. `make-bundle.sh` now
  removes the old seal, clears extended attributes (`xattr -cr`; Finder
  info breaks codesign), signs with `-f` and *verifies*, failing loudly
  on any of it. The first symptom of a bundle that will not start is a
  crash report named after the binary, not an error from the app.

Also observed: a first send from a background agent's shell can time out
(`-1712`) with no visible prompt, while a user Terminal's identical send
prompts and grants cleanly — the TCC ambiguity §7 already carries, now
with its shape known: agents get consent through a host the user can see
prompting, which is the Apple Events consent model working as designed,
and E5's stable identity is what makes that one click last.

## 14. E1, as built

The command table is the single source of truth and everything is
generated from it: `tools/mksdef.py` parses the marked array in
`ui/metal.c` and writes the sdef and `commands.json` (the mkcmos.py
precedent; `make-bundle.sh` re-runs it so a stale dictionary cannot
ship), `describe` serialises the same array at runtime, and the
dispatch resolves both by event id and by name. `describe` answers with
the table through real terminology — `tell application id "…" to
describe` — and the raw-text form (`ping '{"cmd":"describe"}'`) and JXA
(`JSON.parse(app.describe())`) both work. The envelope, the error
taxonomy, the JSON escaping and the waiting parameter's clamps are all
in; single-flight holds by construction while commands are fast.

Four things the wire taught, each costing a build:

- **One handler per event id.** NSAppleEventManager dispatches exact
  (class, id) pairs: a handler for `Ping` does not serve `Desc` in the
  same suite, so E0's single registration meant `describe` compiled,
  was sent, and came back as an unhandled empty reply. Registration now
  walks the table's codes (`metal_glue_script_events()`), is
  idempotent, and runs a second time after the pump's
  `finishLaunching` in case AppKit's own scripting initialisation
  pre-empts ours.
- **The event id is the command.** E0's "omitted parameter means ping"
  shortcut could not survive a second command — bare `describe`
  answered ping's envelope until the handler passed the addressed
  event id through and the C side resolved by it. A `"cmd"` key in a
  JSON direct parameter still overrides, so the raw-text form keeps
  working for agents that prefer it.
- **Deploy as a proper application.** Launching the bundle's executable
  directly left the app half-registered, and terminology, TCC and event
  routing behaved inconsistently mid-session (fast empty replies with
  no handler fired, until a relaunch). The canonical app launch is
  `open -n RISCOSQEMU.app --args …`, which is `tools/run-app.sh` — and
  that script carries the persona split of §1: **no QMP socket unless
  `WITH_QMP=1`**, Apple Events the only always-on channel in the app
  persona. A properly-launched app also has `cwd=/`, so
  `metal-debug.txt` silently does not appear — the log's proper home is
  Application Support, which arrives with Sprint 17's settings file
  and is noted here so the silence is not mysterious.
- **Pipe every reply through a JSON parser in the test, always.**
  describe's first envelope was invalid by one pair of braces — object
  contents where the envelope promised an object — invisible in a
  glance at the string and impossible to miss in `python3 -m
  json.tool`.

## 15. E2, as built

Eighteen commands answer on the wire: the E1 pair, `state`, `video`,
the lifecycle four (`pause`/`resume` fast-path-gated none,
`reset`/`poweroff` behind `dangerous`), snapshots three
(`savevm`/`loadvm`/`listvm`), screens two (`screendump`/`screenshot`),
and input five (`key`/`type`/`mouse`/`click`/`wheel`). The envelope's
machine block grew `uptime_ns`, `frame_gen`, `frames` and `mode` — the
wait-for-desktop poll of the acceptance test is two fields in every
reply. The acceptance script ran end to end: boot, `frame_gen` polling,
both screenshots, snapshot save, list and load, the load proven by the
uptime winding back to the save point and continuing.

Measured on the wire (full Apple Event round trips): fast commands
0–1 ms, `pause` 39 ms (it takes the BQL and stops the vCPUs), `resume`
30 ms, `screendump` 40 ms, `screenshot` 72 ms, `savevm` 749 ms,
`loadvm` 228 ms, `listvm` 0 ms. The bottom-half class is the E1
semaphore design: schedule, wait up to `waiting` seconds (1–60,
default 10), `busy` for a second command while one is in flight, and a
timeout that reports while the operation continues.

Four more things the wire taught, each costing a build or an evening:

- **Initialise before you drain.** The first `pause` aborted the app:
  `script_run_bh` drained the semaphore *before* `qemu_sem_init()` had
  ever run, and macOS's pthread mutex is not a zeroed mutex. The
  init-once block now comes first; the drain only makes sense once
  something can post.
- **One instance, or the wires cross.** A relaunch while an instance
  still ran (a quit that silently failed its QMP handshake) left two
  apps with one bundle id: `tell application id` delivered events to
  one process while QMP answered from the other, and the surface
  looked hung (`-1712` timeouts on `ping`) with both machines healthy.
  §10's duplicate-instance caveat is now a tested failure mode, not a
  footnote: check `pgrep` before blaming the surface.
- **Never walk the object tree per frame.** The crash sample that
  chased the phantom hang showed `bcm2835_vchiq_get_cursor` resolving
  the VCHIQ device through the full QOM tree on *every* frame —
  milliseconds per frame, and a lock-free read of a tree other threads
  own. The device pointer is cached once now, the way the framebuffer
  view always did it.
- **The standard `quit` event was a land mine.** AppleScript `quit`
  previously fell through to NSApplication's default terminate — no
  disc write-back. `aevt/quit` is registered beside the table's
  handlers and routes to the same clean power-off as the window close;
  the reply is an envelope, and the process exits after the disc is
  written.

As-built notes against §5: `type` goes straight to linux key codes —
the same entry point the osx map feeds, so reversing the map would
land on the same codes (US spelling, shifted punctuation, 256-char
cap). `screenshot` is the decoded surface at the guest's native
resolution, with the pointer sprite blended in — the window's scaling
and scanlines ride the drawable, not the texture. Both screens write
only under `~/Library/Application Support/RISCOSQEMU` (§7's configured
directory, v1: one), a caller `name` is a leaf or `denied`, and the
periodic `METAL_SHOT_EVERY` path moved there too — a properly-launched
app has `cwd=/`, so the old bare filename was writing nowhere.
`listvm` reads `bdrv_snapshot_list` on the vmstate device (the same
data `info snapshots` prints). `video` applies scaling/scanlines by
dropping the specialised pipeline for one frame, and `vsync` sets the
machine's generator or reports `not-capable`. Counters (`keys`,
`mouse_moves`, `mouse_buttons`, `ae_events`) are taken where input
enters QEMU, so the window's events and the surface's count together
— an agent proves causality with one `state` before and after.

## Sources

Written against the tree, with these checked: `ui/metal.c` (input glue
149–218, snapshot bottom half 251–273, fb generation 64, the `qemu_main`
hand-off 321–326), `ui/metal.m` (pump 1394–1472, `quitAction` 1245–1251,
`screenshot` 883, menus 1280–1312), `monitor/qmp-cmds.c` (`qmp_stop` 50,
`qmp_cont` 66, `qmp_human_monitor_command` 165), `ui/ui-qmp-cmds.c:324`
(`qmp_screendump`), `include/hw/core/cpu.h` (`cpu_memory_rw_debug` 706,
`cpu_single_step` 1139, `cpu_breakpoint_insert` 1152),
`include/migration/snapshot.h` (32, 47), `include/system/runstate.h`,
`system/main.c` (the `qemu_main` hand-off, 58–91), `MACOS.md` (the
unsigned-unbundled status, §2; what is not done, §6), `SPRINTS.md`
(Sprints 8, 15, 17), `FSDESIGN.md` (6C). Nothing here is built yet.
