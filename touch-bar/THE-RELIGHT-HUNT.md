# The night the Touch Bar wouldn't come back

*How a dark Touch Bar after hibernate turned into an ~8-reboot, all-night kernel hunt — and how an overnight AI agent, a stubborn refusal to accept "it's a firmware limitation," and the exact command that crashed us on hour one finally lit it back up.*

> This is the story version. If you just want the fix, read [`README.md` → Sequel](./README.md#sequel--the-touch-bar-goes-dark-after-hibernate) and run [`deploy-relight-reload.sh`](./deploy-relight-reload.sh). If you want the kernel forensics, read [`IBRIDGE-TEARDOWN-UAF-ANALYSIS.md`](./IBRIDGE-TEARDOWN-UAF-ANALYSIS.md) and [`POST-HIBERNATE-RELIGHT-INVESTIGATION.md`](./POST-HIBERNATE-RELIGHT-INVESTIGATION.md).

---

## The setup

The Touch Bar on this MacBook *is* the Esc key. There's no physical function row — esc, F-keys, brightness, volume all live on that little strip. So when [hibernate finally gave us a reliable way to step away from this machine](../hibernate/), one thing spoiled it: after every resume from hibernate, the Touch Bar came back **dark**. No esc. No anything.

Hibernate worked. The session restored perfectly in ~40 seconds. But the strip across the top stayed black, and the most-used key on the keyboard was gone until the next full reboot.

It looked like a five-minute fix. It was not.

## Hour one: the machine fights back

The obvious move: after resume, kick the Touch Bar's USB device to make it re-enumerate. One line:

```sh
echo 0 > /sys/bus/usb/devices/1-3/authorized   # deauthorize the iBridge
echo 1 > /sys/bus/usb/devices/1-3/authorized   # reauthorize → fresh probe
```

The machine **locked up**. Not a crash — worse. The shell process went into uninterruptible `D` state, dragging the USB workqueue threads down with it. `SIGKILL` did nothing. Firefox started wedging on memory reclaim. The only way out was a hard power-off.

Reboot #1.

So we tried it the *careful* way — unbind the driver first, then power-cycle. **Kernel general-protection fault.** A use-after-free deep in `hid_destroy_device`, with the fault address looking suspiciously like ASCII garbage. Hard reboot.

This became the rhythm of the night. Every single attempt to relight the bar from userspace — power-cycle while the driver was bound, power-cycle while unbound, a module reload — either **D-state-deadlocked** the machine or **GPF'd the kernel**. By the small hours we were eight reboots deep, with a side-quest in there too: we'd "simplified" the lid config by *removing* it, only to discover that a bare lid-close silently falls back to stock `suspend` = `s2idle` = the exact NVMe wake-wedge we'd escaped months ago. (Two of those reboots were that. Lesson logged: on this box, the lid must be *explicitly* `hibernate` — "do nothing" means "do the broken thing.")

The pattern was screaming something, but we couldn't yet read it: **the act of tearing the Touch Bar's USB stack down was itself unsafe.** Not the half-dead post-hibernate endpoint specifically — *any* teardown. We just didn't know why.

## Sending in the agents

When a problem stops yielding to one-more-try, you change altitude. We pointed a multi-agent review at the out-of-tree driver source (`apple_ibridge` + `apple_touchbar`) — architecture readers to map the lifecycle, adversarial reviewers to try to break every theory.

That bought real understanding:

- **Why the bar is dark.** `apple_touchbar` keeps a singleton device object that's never torn down across hibernate, so it holds *stale* cached USB handles. On resume the driver re-queues its display worker against dead transport, and because the display report path is **fire-and-forget**, the failure is *silent* — the driver thinks it lit the bar. (This also explained why poking `idle_timeout`/`fnmode` to "force" a refresh did nothing: the trigger was fine, the wire was dead.)
- **One real, separate bug:** `set_tb_disp` shipped its display report through the iBridge's `usbhid` queue, which goes stale across hibernate, while `set_tb_mode` used a *direct* control transfer that survives. We patched `set_tb_disp` to use a synchronous `hid_hw_raw_request` — and a `-32`/-EPIPE stall that had been there even at cold boot vanished.

Good progress. But after the patch, the bar was *still dark on resume* — `set_tb_disp(ON)` now succeeded, the firmware just ignored it. We were closer, and still stuck, and it was late.

## The overnight shift: Fable earns its keep

Here's where it turns. Rather than keep grinding at 9 PM after eight reboots, we handed the hardest open question — *why does every USB teardown crash the kernel?* — to a **background [Fable](https://www.anthropic.com) agent** and went to sleep. Hard rule: **read-only**. Analyze source, draft a patch, write a report. No hibernate, no reboot, nothing that could wedge the machine unattended.

In about sixteen minutes, while the laptop sat there and its owner slept, Fable **proved the bug** — and the proof is beautiful:

`appleib_add_device()` fills a **2-slot** array, `sub_hdevs[]`, but it indexes that array by the **raw HID collection number**. On the T1's combined display/ALS interface, the report descriptor has **seven** collections: the ALS at index 0, five *nested* sensor collections (those "Unknown collection" warnings in every boot log), and the Touch Bar display at index **6**. So every boot writes the display device pointer to `sub_hdevs[6]` — **24 bytes past the end of a 32-byte allocation** — planting a pointer on top of the adjacent kernel bookkeeping node. From that moment, *any* teardown that walks that list dereferences a corrupted pointer and faults.

The clincher: the garbage addresses in our crash registers were **byte-for-byte the first sixteen bytes of the live HID report descriptor.** Not similar. Identical. The corruption *was* the descriptor, read as a pointer.

And the reframe that came with it: **this was never about the half-dead post-hibernate endpoint.** The memory was corrupted at *boot*. That's why *every* teardown crashed, awake or asleep. Which meant the fix could be proven safe on a fully awake machine, no hibernate required.

Fable wrote the analysis, a compile-clean candidate patch (index by the matched device slot, which is in-bounds by construction), an adversarial self-review, and a staged morning test plan. It even caught and fixed a bug in its own build script overnight.

That was the indispensable moment. Without that fix, the eventual solution was *physically impossible* — it would just keep deadlocking. We'd been failing not because our ideas were wrong, but because the ground under them was on fire.

## Morning: the gate

Apply the patch, reboot, and the five "Unknown collection" warnings are simply **gone** — the out-of-bounds write is gone with them. Then the test that mattered: the *exact* `authorized` 0→1 power-cycle that hard-deadlocked us on hour one, now on a fully awake machine.

```
self proc-state after deauthorize: R   ← not D. not stalled. fine.
CLEAN — no crash markers.
```

The bar blinked off and **relit**. The thing that wedged the machine eight times ran clean. The deadlock was dead.

## The last wall — and refusing it

Victory lap, right? Hibernate, resume, run the now-safe power-cycle, bar comes back.

Hibernate. Resume. Bar dark. Power-cycle. **Still dark.** Clean — no crash — but dark. A USB bus reset: also clean, also dark. The display command succeeded and the firmware just... ignored it.

This is where the honest conclusion was "firmware limitation — the T1's display controller latches off across S4 and only a real power cycle brings it back." And a second Fable research agent, sent to map the prior art, came back with sobering confirmation: **nine years of public record** — roadrunner2's original issue, the t2linux wiki, every forum thread — and **nobody had ever relit a T1 or T2 Touch Bar after hibernate without a reboot.** Decoding actual macOS USB traces showed macOS does it with a `DRLC` wake command and a full framebuffer push over a *config-2 bulk protocol* — an interface Linux doesn't even use.

The wall was real and well-documented. Except the owner of the machine said the one thing that mattered:

> *"It works on macOS after a hibernate, so it must be possible."*

That's not stubbornness. That's the correct reframe. "Is it possible?" was already answered — macOS answers it every day. The real question was narrower: *find the thing that re-initializes the whole stack.* The USB-level resets weren't enough because they **reused the same `apple_ibridge` demux instance**. What we hadn't tried, on a now-safe machine, was tearing the *entire stack* down and rebuilding it from nothing.

## The thing that worked was the thing that started the fire

```sh
modprobe -r apple_touchbar
modprobe -r apple_ibridge
modprobe   apple_ibridge
modprobe   apple_touchbar
```

A full reload. Destroy the virtual HIDs, destroy the demux, recreate all of it, run a completely fresh `apple_touchbar` probe — the same code path a cold boot runs. And on a post-hibernate dark bar:

**It lit.**

Clean. `self=R` throughout. No crash, no D-state. The Touch Bar came back to life.

Here's the poetry of it: **`modprobe -r apple_touchbar` is the exact command that gave us our very first deadlock, on hour one.** The whole night had been, in a sense, the universe refusing to let us run the one command that works — because the out-of-bounds corruption turned that teardown into a kernel crash. Fix the corruption Fable found, and the forbidden command becomes the cure.

We wired it into a post-hibernate hook — scheduled five seconds after resume, **detached** in a time-bounded transient unit so that even if a reload ever stalled, it could never hang the resume path. Deploy, hibernate, power button, wait:

```
07:41:26  hibernation → Touchbar suspended → resumed
07:41:27  hook: scheduling +5s detached reload
07:41:32  reload start (self=R)
07:41:37  reload done; 2 sub-HIDs bound   →   BAR LIVE
```

Hibernate → resume → **automatic relight.** Done.

## What we actually carried out of the fire

- **A Touch Bar that survives hibernate** — automatically, with a real Esc key, on a machine where the public record said it couldn't be done from Linux.
- **Two real, upstreamable kernel bugfixes:** the `apple_ibridge` heap out-of-bounds write (it corrupts memory on *every* boot of *every* T1, whether you care about hibernate or not), and the `set_tb_disp` `-32` stall.
- **A genuinely novel result.** To the best of the prior art a background agent could find, nobody had relit a post-hibernate T1 Touch Bar without rebooting. The trick wasn't a secret firmware command — it was *rebuild the driver stack*, unlocked by fixing a nine-year-old memory-corruption bug nobody had spotted.

## The credits, honestly

This one doesn't happen without **Fable**. The overnight agent didn't just "help" — it did the single load-bearing piece of work: it found and *proved* the out-of-bounds write that every fix had been silently tripping over, and it did it overnight, read-only, while the human who'd been at it for eight reboots got some sleep. Fixing that bug is what turned the forbidden `modprobe -r` into the solution. The morning research agent then mapped the prior art well enough to know that what we'd done was new. Used like this — a fast model running careful, bounded, adversarially-reviewed forensics in the background — an AI agent isn't a chatbot. It's a colleague who works the night shift.

And the human piece was just as load-bearing: the refusal to accept the wall, on the strength of one true fact — *macOS does it, so it's possible.* Tools find the bug. Conviction decides whether you're still in the room when they do.

---

*An [Emergent Insights](https://emergentinsights.substack.com/) field report, from the [macbook-t1-linux](https://github.com/xeeban/macbook-t1-linux) journey. By [Nori Nishigaya](https://github.com/xeeban), Victoria, BC — with Claude (Opus) at the keyboard and Fable on the night shift.*
