# Quality checks for incremental changes

GitHub Actions runs on pull requests, pushes to **main**, and manual workflow runs. Feature-branch pushes run automatically once a PR is open, through the PR event; before that, use **Run workflow** to build a branch on demand. This avoids duplicate push/PR builds. No local ESP SDK is required. Older runs for the same PR or branch are cancelled when a new commit arrives.

The **quality-gate** requires successful change classification, quality checks and both host compiler jobs. Code, configuration, workflow, test and unknown file changes also require a successful firmware build and package validation. A PR containing only `README.md`, `LICENSE`, `LICENSE.md`, or Markdown/PNG/JPEG files under `docs/` deliberately skips firmware. The gate accepts this explicit documentation-only skip, but rejects failures, cancellations, missing classification and unexpected skips. Classification compares the whole checked-out PR merge result with its base SHA, including deleted paths and both sides of renames. Pushes to main and manual runs always build firmware.

Firmware compilation uses a persistent **ccache**, bounded to 500 MB per saved cache. The key includes the SDK baseline, target, relevant configuration and workflow, with a new cache entry for each built commit. Only compiler results are cached: build directories and flashing artifacts are regenerated and validated. Cache misses still perform a complete build; hits reduce repeated compilation but do not remove container startup, SDK setup or linking. Statistics in the build log show the actual hit rate. PR cache access follows GitHub's branch scope; a successful main build seeds a cache that subsequent PRs can restore.

## Automated checks

| Gate | What it catches | Limits |
|---|---|---|
| Quality | Whitespace errors, actionlint workflow/shell validation, ten packaging-validator tests | Does not simulate hardware |
| CI policy | Conservative documentation classification and rejection of failed/cancelled/unexpectedly skipped dependencies | Workflow execution and cache effectiveness are checked on GitHub |
| Protocol (g++ and clang++) | Compiler warnings as errors, AddressSanitizer and UndefinedBehaviorSanitizer, recorded SV3 fixture, malformed input, all five pump registers × 32 capability masks × two readiness states | Fixture is evidence for one controller snapshot |
| Factory reset gesture | Debounce, startup-held button, release requirement, exact five-second boundary, timeout and one-shot trigger | GPIO and SDK erase/reboot require hardware acceptance |
| LED policy | Commissioning states, Thread/spa loss and recovery, pulse boundaries and dark gaps under both compiler/sanitizer jobs | Does not verify LED wiring or colour order |
| Parser stress | Every fixture truncation plus 3,000 reproducible byte mutations; failed parsing must preserve all previous state | Bounded regression stress, not exhaustive fuzzing |
| Firmware | Actual pinned ESP-Matter/ESP-IDF compilation for ESP32-C6 | Compilation does not establish controller interoperability |
| Build package | Generated target/Thread/Wi-Fi/USB console/project config, required images and metadata, nonempty files, safe paths, no flash-image overlap, application offset and fit in both OTA slots | Uses this repository's partition layout |
| OTA headroom | At least 64 KiB free in each OTA application slot | An explicit growth reserve, not a guarantee for future features |
| Control policy | First-valid-RF configuration freezes per boot, sparse stable pump IDs, native mode/slider mapping, staged blower writes/failures and last-fabric recovery | Host policy tests; SDK callbacks and hardware still require firmware/physical validation |
| Startup guard | Permanent controls keep 1/2/8, reserved pump identities 3–7 and aggregator 9 remain stable | Reservations are removed before Matter starts; installed pumps are enabled after RF |

Documentation-only PR runs produce no firmware artifacts. Artifacts are uploaded only after the host checks and firmware/package checks pass. A separate **espyspa-matter-quality-esp32c6** artifact contains commit SHA, image SHA-256 hashes, image sizes/offsets, checked configuration and remaining OTA space. Keep it with the flashing images and matching debug ELF. These hashes identify an artifact; they are not firmware signing.

## Protect main

The workflow alone does not prevent a failing commit from being pushed to main. A repository administrator should configure a branch rule/ruleset for **main** in GitHub Settings:

1. Require the **quality-gate** status check and an up-to-date branch.
2. Disallow bypassing the rule and force pushes.
3. For a review-based workflow, require pull requests and a reviewer before merge.

Wait for the check to run once so GitHub can offer it as a required status check. Repository administration is not available through the connector used for this change; branch protection has not been enabled by this commit. Pull requests are opened when requested; merging remains a user action.

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

Host CI currently does not exercise the FreeRTOS queue, the Matter callbacks, diagnostic JSON construction or Thread reconnection. These remain hardware acceptance checks. Control mapping, per-boot configuration, blower stage sequencing and commissioning recovery policy are exercised in host tests; their SDK/FreeRTOS integration still needs firmware and hardware acceptance.

## Run host checks locally (optional)

```sh
tests/run.sh
CXX=clang++ tests/run.sh
python3 -m unittest discover -s tests -p '*_test.py' -v
```

Requires the selected C++ compiler and Python 3.9 or later. `scripts/check_build.py` runs after an SDK build and requires the generated `sdkconfig`, build images and flashing metadata.
