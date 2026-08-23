<!--
  Please read CONTRIBUTING.md first.

  Code and documentation pull requests are currently PAUSED while the Contributor
  Licence Agreement is finalised. Open an issue instead and your work will be picked
  up once it reopens.
-->

## What this changes

<!-- One or two sentences. What behaviour is different after this? -->

## Why

<!-- What problem does it solve? Link the issue if there is one. -->

## How it was tested

<!-- Which board, which panel, how long it ran. "Compiles" is not testing. -->

---

## Required checks

- [ ] I have read [`CLA.md`](../blob/main/CLA.md) and have posted the signature block from
      section 8 as a comment on this pull request.
- [ ] Every commit is signed off (`git commit -s`) with my real name.
- [ ] This changes **one** behaviour. Unrelated changes are in separate pull requests.
- [ ] The native firmware-logic tests pass (`tests/firmware_logic_test.cpp`).
- [ ] No WiFi credentials, phone numbers, or captured CSV data are included.
- [ ] This contains **no** optical geometry, wavelength handling, PPG pipeline design,
      sensor-fusion design, schematics, or wearable mechanical design.
      (See "Hardware, optics, and sensor work" in `CONTRIBUTING.md` — that material must not
      be published here.)
- [ ] This does not present the project as a medical device or a safety monitor.
