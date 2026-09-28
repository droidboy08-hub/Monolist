---
name: user-working-style
description: "How the user likes to work on this project: terse replies, keep momentum, free to rewrite code and scripts"
metadata: 
  node_type: memory
  type: feedback
  originSessionId: ef2a6719-0eac-48fc-a636-2c9ddb95cb0e
  modified: 2026-09-28T22:13:14.490Z
---

The user writes short, typo-heavy messages and approves plans with one word ("go"). They interrupt mid-task with side questions, e.g. "is InnerTube faster?", then say "keep doing the work". They told me explicitly that existing scripts and code can be rewritten, since they have learned new approaches on a similar project.

**Why:** they want steady progress without re-asking about things already agreed. They value measured evidence; benchmarks and screenshots landed well.
**How to apply:**
- Keep executing an agreed plan through to working, verified results.
- Answer interjections briefly, with numbers, then continue.
- Refactor freely where it improves things.
- Still ask before downloads or installs, stating filename, source and size. Never hard-delete files on the network share; tell them to delete in Finder so it goes to the Trash.
- 2026-09-27: the owner said multi-agent workflows "take too much time" — after engine batch B5, do the work directly myself instead of launching agent workflows (even though ultracode is on; the owner's instruction wins). Keep verification (build, self-tests, targeted re-measurement) but lighter than the agent batches.
- 2026-09-28 feedback: "could have told me this sooner" — the owner only learned after ~2 days that the ARM Windows VM (x64 emulation) + the \\psf network share + heavy 4-reviewer agent batches made builds 5-10 min and batches ~15 h; they would have used their own Windows PC. **Why:** they choose tools and hardware based on such costs. **How to apply:** surface environment/process costs that make work slow or expensive as soon as they are visible, with a concrete alternative, before committing to long runs. The owner is moving development to a native Windows PC (hand-off folder Desktop\Monolist-handoff with CLAUDE.md, memory, research, bench, toolchain zip).
- The Parallels VM can pause itself when idle (clock jumps forward on resume); a long "stuck" build is usually a paused VM, not a hang.

Related: [[monolist-project-goal]].
