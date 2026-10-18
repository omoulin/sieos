# SIEOS: rules for agents

Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only

- SIEOS must stay clean, fast, portable and small: heavy or optional things are add-on packages ([docs/packages.md](docs/packages.md)), not the base system.
- Every change must pass `make budget` (every `make` checks it, both architectures); raising a limit in `tools/budget.txt` needs a reason in the same commit ([docs/budgets.md](docs/budgets.md)).
