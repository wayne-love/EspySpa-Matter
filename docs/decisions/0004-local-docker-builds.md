# ADR 0004: Reusable local Docker development builds

- **Status:** Accepted
- **Date:** 2026-10-22

## Context

Explicit local firmware builds are allowed alongside release-only GitHub firmware
builds. SDK preparation is expensive, while repeated development builds should
reuse generated configuration, objects and compiler results.

## Decision

Use CI's `espressif/esp-matter:release-v1.4.2` base, prepare its ESP-Matter checkout
at `4cedd4ad40821933d6cc237e80ab6604f2953fb0` and verify ESP-IDF v5.4.1 when
creating a reusable local image. Verify both SDK pins at command startup.
Mount the checkout at a stable `/workspace` path. Persist configuration, build
outputs, logs and a 500 MB ccache under ignored `.local-build/`. Run as the
invoking UID/GID with a writable persistent home. Serialize commands with a lock.
Default to four compiler tasks per Ninja process, not four concurrent containers.
Each command starts one container; the outer and nested Ninja limits are
independent rather than a combined task ceiling. Select ESP32-C6 through
IDF_TARGET. Never automatically
run set-target/fullclean. Reconfigure only when the Git identity changes; Ninja
also tracks source and CMake dependencies. Local identities always use the
development channel and include the Git revision and `-dirty` when applicable.

Reuse the existing host suites, configuration/OTA validator, packaging script and
manifest generator. Package copies of validated outputs so the public filename
rewrite cannot disturb the incremental build tree. A local manifest uses a
reserved `.invalid` URL: it is metadata for local files, not a published update.

## Alternatives

- Install SDKs on the host: rejected as the default because it adds host setup
  and makes divergence from CI easier.
- Download and prepare the SDK on every run: rejected for repeated cost.
- Reuse only ccache while cleaning every build: rejected because Ninja outputs
  and configuration are useful incremental state too.
- Run bind-mounted builds as root and recursively chown afterward: rejected;
  matching UID/GID creates usable outputs without an ownership repair pass.

## Compatibility and tests

CI triggers, release publishing, endpoint IDs, firmware behavior and hardware
operations are unchanged. Existing validator CLI defaults remain compatible
with CI; optional paths support the isolated local build directory. Mocked host
tests cover identity changes, incremental command selection and package staging.
Validate with both C++ host suites, Python tests, a first local firmware build,
an unchanged incremental build, package checks, ownership checks and ccache
statistics. Firmware build success is separate from physical hardware acceptance.

See [local build workflow](../BUILD.md#local-docker-builds).
