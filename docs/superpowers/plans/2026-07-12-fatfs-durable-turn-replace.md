# FATFS Durable Turn Replacement Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make repeated Agent turn-state and assistant-audio publication work on ESP-IDF FATFS while preserving recovery after power loss.

**Architecture:** Keep all behavior inside `AgentTurnStore`. A test-only rename shim reproduces FatFs `FR_EXIST` semantics on the host; production uses a `.part → primary` publication with a temporary `.bak` swap and manifest fallback recovery.

**Tech Stack:** C++17, ESP-IDF FATFS/POSIX VFS, host C++ assertions, ESP-IDF 5.5.3.

---

### Task 1: Reproduce FATFS no-overwrite behavior

**Files:**
- Create: `main/apps/recorder/agent_turn_store_fatfs_test.cc`
- Test: `main/apps/recorder/agent_turn_store.cc`

- [x] **Step 1: Write the failing host test**

Create a test rename shim named `AgentTurnStoreFatRename`. It returns `-1` with
`errno=EEXIST` whenever the destination already exists and otherwise calls
`renameat`. The test must create a turn, call `MarkRecorded`, then require
`UpdateState(..., kSending)` to succeed and persist `"status":"sending"`.

- [x] **Step 2: Run the test under FATFS rename semantics**

Run:

```bash
c++ -std=c++17 -Wall -Wextra -Werror \
  -Drename=AgentTurnStoreFatRename \
  -I main/apps/recorder \
  main/apps/recorder/agent_turn_store_fatfs_test.cc \
  main/apps/recorder/agent_turn_store.cc \
  -o /tmp/agent_turn_store_fatfs_test && \
  /tmp/agent_turn_store_fatfs_test
```

Expected: FAIL at the `UpdateState` assertion because the current direct rename
cannot replace `turn.json`.

### Task 2: Publish through a recoverable backup swap

**Files:**
- Modify: `main/apps/recorder/agent_turn_store.cc`
- Modify: `main/apps/recorder/agent_turn_store_fatfs_test.cc`

- [x] **Step 1: Implement the minimal replacement helper**

Add one internal helper that removes a stale backup only while a valid primary
exists, renames primary to `<path>.bak`, renames the fully flushed `.part` to the
primary path, restores the backup if publication fails, and removes the backup
after success. Use it from both `WriteRecord` and `CommitReply`.

- [x] **Step 2: Verify manifest replacement turns green**

Run the Task 1 command. Expected: PASS with the simulated no-overwrite rename.

- [x] **Step 3: Extend the test for assistant WAV replacement**

Pre-create `assistant.wav`, then require `BeginReply`, `AppendReply`, and
`CommitReply` to replace it under the same rename shim. Confirm the resulting
file contains the new reply and neither `.part` nor `.bak` remains.

- [x] **Step 4: Verify the extended test**

Run the Task 1 command. Expected: PASS.

### Task 3: Recover an interrupted manifest swap

**Files:**
- Modify: `main/apps/recorder/agent_turn_store_fatfs_test.cc`
- Modify: `main/apps/recorder/agent_turn_store.cc`

- [x] **Step 1: Write the failing recovery assertion**

After a valid manifest exists, move it to `turn.json.bak` with no primary file.
Construct a new `AgentTurnStore` and require `ListPending()` to return the turn.

- [x] **Step 2: Run the test and verify RED**

Run the Task 1 command. Expected: FAIL because `LoadRecord` currently reads only
`turn.json`.

- [x] **Step 3: Add backup fallback**

Make `LoadRecord` read `turn.json.bak` only when the primary manifest is absent
or empty. A subsequent `UpdateState` must republish the primary and remove the
backup.

- [x] **Step 4: Run FATFS and ordinary store tests**

Run the Task 1 command and the existing `agent_turn_store_test` command from
`docs/recorder-design-guardrails.md`. Expected: both PASS.

### Task 4: Rebuild, flash, and verify the pending real turn

**Files:**
- Modify: `docs/recorder-design-guardrails.md`
- Modify: `AGENTS.md`

- [x] **Step 1: Run the complete firmware host gate**

Run every recorder command in `docs/recorder-design-guardrails.md`, the new
FATFS test, verifier script tests, and the `nm` check. Expected: all PASS and only
`ns_create` is unresolved from the noise-suppression API.

- [x] **Step 2: Build and flash without NVS erase**

Run `idf.py build`, confirm `firmware_voice_config=verified`, then flash the
normal manifest to `/dev/cu.usbmodem1101`. Expected: all images hash-verify and
the flash manifest still excludes NVS.

- [ ] **Step 3: Verify recovery and the complete physical loop**

Cold boot with USB and battery disconnected for 20 seconds, enter Recorder, and
capture with `dtr=True,rts=False`. The already queued `user.wav` must upload once;
then `turn_accepted`, `assistant.wav` storage, and automatic playback must appear.
Record one fresh turn and require all six runtime-verifier milestones.

- [ ] **Step 4: Update acceptance evidence and commit**

Record only sanitized counts, hashes, revisions, timestamps, and status. Run
secret scans, repository status checks, Agent/Azure verification, and commit the
firmware fix plus final documentation without `sdkconfig`, tokens, audio, or logs.
