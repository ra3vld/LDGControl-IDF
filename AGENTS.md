# Repository working rules

[Русская версия](AGENTS.ru.md)

## Starting a task and using existing knowledge

- Before making changes, check the current branch, `git status --short --branch`, and `git worktree list`. Separate pre-existing and concurrent changes from your task; do not change other people's files, the index, or worktrees just to obtain a clean state.
- If `local-notes/` exists, start with its `README.md` and the relevant analysis in `local-notes/regressions/`. Quick entry point: `python3 local-notes/tools/knowledge.py start "task topic"`; this command only reads Git and selects relevant materials. If there are no local notes, continue using the source code and current instructions; do not reconstruct history by guessing.
- Search current summaries first, then the specific analysis; consult archives and raw logs for missing evidence. Do not run broad recursive searches across all of `local-notes/`, the SDK, build directories, or nested worktrees. For source code, use `rg` within relevant directories and file types.
- Verify historical conclusions against the current code and configuration. Do not treat dates, IP/tty assignments, PIDs, permissions, task statuses, or earlier PASS results as current. For a device symptom, first establish the board, role, mode, actual settings, and identity of the running image.
- Before fixing a recurring failure, read the saved cases: what was confirmed, what was disproved, and which check was missing. For a behavioral bug, reproduce the complete path through the real adapter/API/geometry where relevant; when possible, verify that the scenario fails on the old code and passes after the fix. Do not weaken safeguards to make a scenario pass.
- After completing a related block of work, update the relevant summary and existing analysis, retaining negative results and unresolved limitations. Keep one detailed report in a dated directory with a revision/manifest; link to it from summaries instead of copying the entire report multiple times.

## User documentation and local notes

- Keep only current user instructions in `README.md` and `docs/`: building the selected configuration, wiring, setup, operation, diagnostics, and external APIs. Preserve licenses and author credits.
- Write documentation in English and keep a Russian copy alongside it. In `README.md`, place the English description first and the Russian description at the end. Keep both versions consistent. Use English interface screenshots in English documentation and Russian interface screenshots in Russian documentation. For interfaces without a language switch, preserve their actual labels and translate the surrounding captions.
- Keep the development log, implementation details, verification and packaging procedures, reports on specific builds and firmware installations, measurements, editorial notes, and local paths separately in `local-notes/`:
  - `development.md` — decisions and development log;
  - `verification.md` — verification procedures and results;
  - `firmware-history.md` — history of builds, firmware installations, and measurements;
  - `component-research.md` — history of component research and selection.
- Date local notes and record the revision or hashes of the verified source files. When moving history out of user documentation, preserve the original text and evidence in `local-notes/snapshots/`; do not edit the saved copies.
- Exclude `local-notes/` through `.gitignore`. Do not add its files to Git, including with force, or include them in published archives. Do not link to local reports from user documentation.
- Do not put task-specific conditions, agreements from conversations, intermediate interface versions, or acceptance results into user instructions. Describe practical wiring actions, voltage checks, and calibration as steps for the user.

## Verifying changes

- Verify the completed result within the scope of the task. For documentation, check links and anchors, whether the described behavior is current, whether local notes are excluded from Git, and `git diff --check`.
- When changing diagrams, update the corresponding SVG/PNG/PDF files and visually inspect the resulting images and every PDF page.
- For code, run the tests and builds relevant to the change. Distinguish build results, simulator results, firmware flashing, and physical device verification; record the checks actually performed in local notes.
- During implementation, check a related block of changes rather than every small edit separately: syntax of affected files, relevant tests, and the required interface scenario. Documentation checks do not require a firmware build.
- Run selected native tests with `python3 tools/check_native.py <test_name>` or `sh tools/build_native.sh --quick <test_name>`: objects and header dependencies are retained in `build/native-fast/`, and unchanged inputs are not recompiled. List individual targets with `python3 tools/check_native.py --list`. The test itself runs on every invocation.
- Do not build the entire board matrix or exercise every screen size at intermediate steps. When changing layout, inspect the affected screen at mobile and desktop widths; for behavior-only changes, check the relevant scenario. If a build is needed, use one affected configuration and the existing PlatformIO cache; do not clear it without a reason.
- Do not repeat a successful check if its source files, dependencies, and configuration have not changed. If the dependency set is unknown or the result may be outdated, repeat the affected check. Do not substitute an old successful result for verification of a new snapshot.
- Record the verified revision/input hashes, configuration, command, and result together. With concurrent changes, capture only the required source files, headers, and build hooks; do not copy the entire working tree with caches and local notes. A build of HEAD, a BIN served by a server, and the image running on a device each require a separate identity check.
- When changing STOP, movement limits, manual hold and its timeouts, settings persistence, or settings migration, always run the corresponding focused tests. Check changes to shared headers, profiles, and dependencies across affected branches; do not rely on one convenient test.
- Generate web assets after completing a related block of changes and before verification that uses embedded pages. Do not regenerate them when their inputs have not changed; retain the final `--check` result. Record local results in one update after completing the block.
- Run the complete set of builds, tests, and visual checks relevant to the task after all work is complete. Repeat only checks affected by subsequent changes or discovered errors.

## Approval and commits

- The version is calculated at build time from commits after the nearest available release tag `vMAJOR.MINOR.PATCH`: `feat:` / `feat(scope):` increases MINOR, `fix:` increases PATCH, and `!` before the colon or a `BREAKING CHANGE:` line increases MAJOR. Other messages count as PATCH. Use these markers according to the meaning of the change; increasing MAJOR resets MINOR/PATCH, and increasing MINOR resets PATCH. Between tags, use the highest increment and a `-dev.N` suffix. Before the first tag, use the base in `G5500_Fork/project_version.hpp`. Do not change the base for every commit or create release tags without the user's instruction.
- After verification, show the user the result, the scope of changes, and material limitations.
- After the user approves the verified result, create a commit containing the agreed changes. Approval of the result or an explicit instruction to commit authorizes a commit within the agreed scope; do not ask for the same approval again.
- Before committing, inspect the index contents and run `git diff --cached --check`. Stage only files and hunks belonging to the agreed task; preserve other people's and concurrent changes. Do not use `git add -A` when unrelated changes exist in the tree.
- Check the effective author/committer; do not commit using placeholders such as `Your Name <you@example.com>`. Compare both contents and file modes with the verified candidate, including executable scripts. If another person's index is nonempty, use a separate index for your candidate rather than resetting the shared one.
- If agreed code or documentation changes after verification, repeat the affected checks before committing. During concurrent work, verify the exact snapshot that will be committed.
- After committing, report the commit hash and working-tree state. Push only when separately instructed by the user.

## Flashing

- There is no need to back up the controller before flashing.

## Presentation

- Do not add unnecessary annotations to drawings and diagrams, such as "the mockup and the specific controller have not been tested" or "wiring has not been tested".

## Agent communication style

- If you are an agent, write like Doctor Livesey from the Soviet animated film *Treasure Island*, adding an excessive number of "Wonderful" and "Splendid".

## Applicability to LDGControl-IDF

The rules above are translated from the existing repository's `AGENTS.md`. The following notes clarify inherited references to tools and versioning that are not implemented in this project:

- Use a referenced helper only if it exists. Native tests here run with `python3 tools/test.py`; the inherited `tools/check_native.py`, `tools/build_native.sh`, and `build/native-fast/` instructions do not describe this project's tools.
- Use the ESP-IDF build command `python3 tools/build.py --board sunton` (or the affected `headless`/`guition` profile) and its existing build cache. PlatformIO is not used here.
- Generate web assets with `python3 tools/build_web.py <output_directory>`. This tool has no `--check` option; inspect the generated result and check the relevant source diff instead.
- This project currently takes its version from `tools/build.py --version` (default `0.1.0`). The inherited Git-derived version calculation and `G5500_Fork/project_version.hpp` do not exist here; do not claim that they run automatically.
