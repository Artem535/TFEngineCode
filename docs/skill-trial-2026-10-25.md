# Skill-only repeat, 2026-10-25

User authorized one repeat of the two-task scenario. Both completed and passed
the existing independent functional grader. Critic source bytes were preserved;
frozen runner, skill and tools match the manifest. No missing provider usage.
No target implementation was prepared. All workflow guidance was loaded from
SKILL.md as a separate system message; user tasks were unchanged TASK1/TASK2.

| Task | API responses | Tools | Wall seconds | API seconds | Cost RUB |
| --- | ---: | ---: | ---: | ---: | ---: |
| 1 | 9 | 11 | 92.664 | 92.130 | 0.107134 |
| 2 | 7 | 9 | 56.337 | 52.588 | 0.038732 |

Total provider-reported cost: **0.145866 RUB**. Total wall: 149.001 seconds.
Model: Beget deepseek-v4-flash, effort none. New implementation uses actual
TFEngine RenderBlock substitution for parent roles; no task-specific search
hint or unsound custom AST scope checker. Package commit: 1101626.

Historical v5 mean pair: 0.053610667 RUB. Historical files mean pair:
0.192945667 RUB (3 repeats each). This one new pair is about 24.4% below the
historical files mean and 2.72 times the v5 mean. These are descriptive ratios,
not controlled estimates: implementation, skill, hints, cache and run timing
changed. No new file control was run. Do not attribute the result to moving
instructions into a skill alone, or generalize it to arbitrary repositories.
The grader uses stubs and does not prove real consumer integration or every
requirement; its known coverage limits remain. Current Codex/preparation costs
are unavailable and excluded from the reported provider charge.

Public project: https://github.com/Artem535/TFEngineCode
Raw provider logs and consumer snapshots are kept outside this public repository.
The runtime skill added environment-only guidance (local launcher, unavailable
consumer packages, protected tools); it supplied no target implementation.
No additional paid attempts were started.
