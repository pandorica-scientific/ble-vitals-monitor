# Changelog

All notable changes to this project are documented here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/) and versions follow
[Semantic Versioning](https://semver.org/).

## [Unreleased]

## [0.1.0] - 2026-10-22

First tagged release: the state of the project as of the firmware refactor of 23 September 2026.

### Added

- Standalone ESP32 Cheap Yellow Display firmware (`firmware/cyd_vitals`): live heart rate, SpO₂ and
  skin temperature from the wristband's BLE advertisements, timestamped CSV logging to microSD,
  24-hour touch charts, a full-screen alarm for a sustained critical heart rate, a low heart-rate
  alarm, and a phone report served from the device's logs.
- Heart rate corrected from the beat interval, with recent context used to resolve conflicting rates.
- Helper sketches: `reader_serial`, `bandsniff` and `provision_wifi`.
- Mac command-line tools (`mac/`) for live readings and passive scanning, and `tools/analyze.py` for
  per-byte analysis of raw captures.
- Documentation (`docs/PROTOCOL.md`, `docs/ARCHITECTURE.md`), native C++ and Node.js tests, and CI
  that runs them and compiles every sketch.
- Contributor Licence Agreement and sign-off check (code contributions are paused).
- Project hygiene: this changelog, a dated README status line, Dependabot for GitHub Actions and a
  bug report template.

[Unreleased]: https://github.com/pandorica-scientific/ble-vitals-monitor/compare/v0.1.0...HEAD
[0.1.0]: https://github.com/pandorica-scientific/ble-vitals-monitor/releases/tag/v0.1.0
