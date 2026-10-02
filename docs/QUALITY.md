# Quality checks for incremental changes

GitHub Actions runs on every push, pull request and manual workflow run. No local ESP SDK is required. The **quality-gate** job succeeds only when all required jobs succeed; a failed, cancelled or skipped dependency fails that gate. Older runs on the same branch are cancelled when a new commit arrives.

## Automated checks

| Gate | What it catches | Limits |
|---|---|---|
| Quality | Whitespace errors, actionlint workflow/shell validation, ten packaging-validator tests | Does not simulate hardware |
| Protocol (g++ and clang++) | Compiler warnings as errors, AddressSanitizer and UndefinedBehaviorSanitizer, recorded SV3 fixture, malformed input, all five pump registers × 32 capability masks × two readiness states | Fixture is evidence for one controller snapshot |
| LED policy | Commissioning states, Thread/spa loss and recovery, pulse boundaries and dark gaps under both compiler/sanitizer jobs | Does not verify LED wiring or colour order |
| Parser stress | Every fixture truncation plus 3,000 reproducible byte mutations; failed parsing must preserve all previous state | Bounded regression stress, not exhaustive fuzzing |
| Firmware | Actual pinned ESP-Matter/ESP-IDF compilation for ESP32-C6 | Compilation does not establish controller interoperability |
| Build package | Generated target/Thread/Wi-Fi/USB console/project config, required images and metadata, nonempty files, safe paths, no flash-image overlap, application offset and fit in both OTA slots | Uses this repository's partition layout |
| OTA headroom | At least 64 KiB free in each OTA application slot | An explicit growth reserve, not a guarantee for future features |
| Startup guard | Control endpoint IDs 1–8 and aggregator 9 remain stable | Runs on the ESP at startup, not in host CI |

Artifacts are uploaded only after the host checks and firmware/package checks pass. A separate **espyspa-matter-quality-esp32c6** artifact contains commit SHA, image SHA-256 hashes, image sizes/offsets, checked configuration and remaining OTA space. Keep it with the flashing images and matching debug ELF. These hashes identify an artifact; they are not firmware signing.

## Protect main

The workflow alone does not prevent a failing commit from being pushed to main. A repository administrator should configure a branch rule/ruleset for **main** in GitHub Settings:

1. Require the **quality-gate** status check and an up-to-date branch.
2. Disallow bypassing the rule and force pushes.
3. For a review-based workflow, require pull requests and a reviewer before merge.

Wait for the check to run once so GitHub can offer it as a required status check. Repository administration is not available through the connector used for this change; branch protection has not been enabled by this commit. No pull request is created or merged on your behalf.

See [GitHub protected branches](https://docs.github.com/en/repositories/configuring-branches-and-merges-in-your-repository/managing-protected-branches/about-protected-branches).

## Rules for each incremental change

- Add a regression case that fails before fixing a protocol bug. Use recorded RF responses for new register formats; redact sensitive data and record the controller/firmware origin.
- Preserve endpoint IDs and control meaning across updates. An intentional topology change needs a migration plan; do not remove the startup guard merely to make the device boot.
- Keep writes queued, fresh-state guarded and confirmed by readback. Never retry a toggle automatically or block the Matter callback on UART.
- Document the behaviour and limits of new controls. A passing build does not validate a new SpaNET command on hardware.
- Download images only from a successful quality-gate run and retain the previous known-working package.

## Hardware acceptance before relying on a changed control

| Test | Expected observation |
|---|---|
| First pairing from clean commissioning storage | Joins Thread; names and controls appear correctly in Apple Home and the intended controller |
| Reboot/application update retaining NVS | Pairing and endpoint IDs persist; normal control resumes |
| Network diagnostics | Page and JSON open over IPv6 without authentication; fresh/stale state and error information are accurate |
| UART absent or invalid response | Device remains reachable on Thread; spa state becomes stale and commands are rejected |
| Each installed pump/light/blower | Correct wire command and confirmed state; absent/unready pump requests are rejected |
| Temperature boundaries | Valid step sizes accepted; invalid/out-of-range setpoints rejected; no unsupported global Off command |
| Burst commands | Bounded queue; rejected requests visible; no unintended repeats |
| Thread outage and restoration | Diagnosed locally during outage; reconnection and diagnostics recover |

Record firmware commit, spa model/firmware, controller and phone OS version, result and a diagnostic snapshot. Controller names can be cached; verify naming on a newly added device as well as an existing pairing. Do not erase an in-service device merely to perform a naming test.

Host CI currently does not exercise the FreeRTOS queue, the Matter callbacks, diagnostic JSON construction or Thread reconnection. These remain hardware acceptance checks. Extracting those policies behind host-testable interfaces is a subsequent quality milestone if changes begin touching them frequently.

## Run host checks locally (optional)

```sh
tests/run.sh
CXX=clang++ tests/run.sh
python3 -m unittest discover -s tests -p '*_test.py' -v
```

Requires the selected C++ compiler and Python 3.9 or later. `scripts/check_build.py` runs after an SDK build and requires the generated `sdkconfig`, build images and flashing metadata.
