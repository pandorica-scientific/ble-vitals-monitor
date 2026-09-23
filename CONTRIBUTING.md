# Contributing

Thank you for looking. Please read this before opening a pull request — the rules here are
unusual for a hobby project, and they exist for a specific reason.

---

## Current status: code contributions are paused

**No pull request containing code or documentation will be merged yet.** The Contributor
Licence Agreement in [`CLA.md`](CLA.md) is complete and drafted for Polish law, but has not
yet been signed off by a lawyer. Merging a contribution before that is done would create
exactly the problem the CLA is meant to prevent.

This is not a rejection of your work. It is a queue. If you have something ready, open an
issue describing it and it will be picked up once the agreement is final.

### What is welcome right now

- **Bug reports** — especially reception problems, panel variants, and decoding mismatches
  against the official app.
- **Protocol observations** — byte-level findings from your own hardware. Please describe
  what you observed rather than attaching code.
- **Questions and build reports** — what worked, what did not, on which board.

None of these require an agreement, because none of them are copyrightable contributions to
the codebase.

---

## Why a CLA and not just a sign-off

Today one person holds the copyright in every line of this project. That means the licence
can still be changed later — for example to offer a commercial version alongside the free one
that parents use.

The moment a contribution from someone else is merged without an agreement, that stops being
true. The contributor keeps their copyright, their code is licensed to the project under
[PolyForm Noncommercial 1.0.0](LICENSE) and nothing more, and any future licence change needs
their individual permission. Contributors move on, change email addresses, and occasionally
say no. It only takes one to freeze the project's licence permanently.

A **Developer Certificate of Origin alone does not solve this.** The DCO certifies where code
came from — that you wrote it and may submit it. It grants the project nothing beyond the
licence already stated in the repository. It is a provenance tool, not a licensing tool.
(There is a second mismatch: the DCO's text refers to "the open source license indicated in
the file", and this project is deliberately *source available*, not OSI open source.)

So this project uses both, for the two different jobs:

| Instrument | Job |
|---|---|
| [`CLA.md`](CLA.md) | Grants the owner a sublicensable licence, so the project's licence can change later. You keep your copyright. |
| `Signed-off-by` on each commit | Per-commit record that the work is yours and you may submit it. |

The CLA does **not** ask you to give up your copyright, and does not restrict what you do with
your own code elsewhere.

---

## Once contributions reopen

1. Read [`CLA.md`](CLA.md).
2. Sign off every commit: `git commit -s` (adds `Signed-off-by: Your Name <you@example.com>`).
   If you forget, `git rebase --signoff main` fixes a branch.
3. Post the signature block from `CLA.md` section 8 as a comment on your pull request.
4. Fill in the pull request template.

Automated checks verify the sign-off. The CLA comment is checked by hand.

---

## Hardware, optics, and sensor work — do not send it here

This repository is a **receive-only BLE decoder and display**. It is deliberately scoped to
software that listens to advertisements a wristband already broadcasts.

Do not open pull requests or issues here containing:

- optical geometry — emitter/detector placement, spacing, aperture, or housing design;
- wavelength selection or drive schemes;
- photoplethysmography signal-processing pipelines beyond what is needed to decode the
  existing broadcast;
- sensor-fusion designs;
- schematics, board layouts, or mechanical design for a wearable.

Any such material published here becomes prior art on the day it is posted, against the
publisher as much as anyone else. That work belongs in a private repository until it has been
through patent advice. If you have something in this area, say so in an issue **without
technical detail** and it can be discussed privately.

---

## Scope and style

- Keep pull requests focused. One behaviour change per pull request.
- Firmware logic that can be tested natively should be, in `tests/firmware_logic_test.cpp` —
  the pure headers under `firmware/cyd_vitals/` are the pattern to follow. Drawing goes in a
  `*_render.h` header; tunables go in `config.h`. See [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md).
- Run the native tests before opening a pull request, and make sure every sketch still compiles:
  ```bash
  sh tests/run.sh
  arduino-cli compile -b esp32:esp32:esp32:PartitionScheme=no_fs firmware/cyd_vitals
  ```
- Formatting follows the repository's `.clang-format` (Google style, 100 columns).
- Never commit WiFi credentials, phone numbers, or captured CSV data. `.gitignore` covers the
  usual paths, but check `git diff --staged` anyway.

---

## Not a medical device

This project is not a medical device and must not be presented as one. Contributions that
frame it as a safety or monitoring device for infants will not be accepted, however well
intentioned.
