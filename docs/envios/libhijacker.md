Repo: astrelsky/libhijacker
Title: License clarification: GPL version and "or later"?

Body:
---
Hi, and thanks for libhijacker — it's foundational for a lot of PS5 homebrew.

The repository ships `LICENSE.txt` with the GNU General Public License v2 text,
but neither the file headers nor the README state whether the project is
"GPLv2 only" or "GPLv2 or (at your option) any later version".

This matters for downstream projects: several widely used PS5 tools (etaHEN,
OnionHEN and forks) combine libhijacker with GPLv3 code. If libhijacker is
"GPLv2 only", that combination is technically incompatible; if it's "GPLv2 or
later", it's fine.

Could you clarify the intended license — ideally by adding an SPDX header or a
one-line note in the README (e.g. "GPL-2.0-or-later" vs "GPL-2.0-only")?

Thanks for your time and for the project.
