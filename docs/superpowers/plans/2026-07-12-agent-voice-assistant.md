# Agent Voice Assistant Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Upgrade the ESP32-C6 SD recorder into a push-to-talk AI assistant that exchanges durable voice turns with the Azure Agent platform, binds those turns to the 金山 user and an Opus 4.8 assistant, stores both sides on SD, and automatically plays complete replies.

**Architecture:** The device keeps a TLS WebSocket open and moves WAV files in bounded binary chunks, while its recorder task remains the sole owner of SD, display, and codec operations. Agent authenticates one hardware Endpoint, persists an idempotent turn, runs Azure STT → user-scoped Opus 4.8 prompt processing → Azure TTS, then streams the stored reply back. Azure bootstrap copies provider and speech configuration internally from Xiaolu without exposing secret values.

**Tech Stack:** ESP-IDF 5.5.3, C++17, FreeRTOS, FATFS/SDSPI, LVGL, `esp-ml307` WebSocket, FastAPI, SQLAlchemy, Alembic, SQLite, pytest, Azure Speech, Anthropic-compatible Opus 4.8 provider, Nginx, systemd.

---

## Repository and file map

Agent repository: `/Users/kanayama/Desktop/AI/agent`

- Create `backend/services/device_voice_auth.py`: token hashing and Endpoint principal resolution.
- Create `backend/services/device_voice_protocol.py`: upload/download frame state and file hashing.
- Create `backend/services/device_voice_processor.py`: user-scoped STT/LLM/TTS turn orchestration.
- Create `backend/api/device_voice.py`: authenticated WebSocket transport.
- Create `backend/alembic/versions/e4c7a1d9b205_add_hardware_voice_turns.py`: durable turn table.
- Create `backend/scripts/bootstrap_device_voice.py`: idempotent Agent DB provisioning from Xiaolu.
- Create `backend/scripts/sync_xiaolu_voice_env.py`: secret-preserving STT/TTS env merge.
- Create tests matching each module under `backend/tests/`.
- Modify `backend/models.py`, `backend/config.py`, `backend/core/ai_pipeline.py`, `backend/main.py`, `.env.example`, deploy verification, and adjacent AGENTS/operations docs.

Firmware repository: `/Users/kanayama/xiaozhi-kb`

- Create `main/apps/recorder/agent_voice_state.*`: pure turn/network reducer.
- Create `main/apps/recorder/agent_voice_protocol.*`: JSON controls, hashes, binary transfer bookkeeping.
- Create `main/apps/recorder/agent_turn_store.*`: SD directory, manifest, recovery, and atomic files.
- Create `main/apps/recorder/recorder_network.*`: Wi-Fi and WebSocket lifecycle.
- Create host tests for each pure component.
- Modify `recorder_app.cc`, recorder display/control/file list, `main/CMakeLists.txt`, `Kconfig.projbuild`, README, AGENTS, and verification scripts.

## Task 1: Persist idempotent hardware voice turns

**Files:**
- Modify: `/Users/kanayama/Desktop/AI/agent/backend/models.py`
- Create: `/Users/kanayama/Desktop/AI/agent/backend/alembic/versions/e4c7a1d9b205_add_hardware_voice_turns.py`
- Create: `/Users/kanayama/Desktop/AI/agent/backend/tests/test_device_voice_models.py`

- [ ] **Step 1: Write the failing ownership and uniqueness tests**

```python
def test_hardware_voice_turn_is_owned_and_idempotent(session):
    turn = HardwareVoiceTurn(
        user_id=user.id, endpoint_id=endpoint.id, identity_id=identity.id,
        turn_id="turn-1", status="recorded", input_bytes=48,
        input_sha256="a" * 64, input_audio_path="/tmp/in.wav",
    )
    session.add(turn)
    session.commit()
    session.add(HardwareVoiceTurn(
        user_id=user.id, endpoint_id=endpoint.id, identity_id=identity.id,
        turn_id="turn-1", status="recorded", input_bytes=48,
        input_sha256="a" * 64, input_audio_path="/tmp/in-2.wav",
    ))
    with pytest.raises(IntegrityError):
        session.commit()
```

- [ ] **Step 2: Run the test and verify the model is missing**

Run: `cd /Users/kanayama/Desktop/AI/agent/backend && JWT_SECRET=agent-test-secret PYTHONPATH=. ./venv/bin/pytest -q tests/test_device_voice_models.py`

Expected: FAIL importing `HardwareVoiceTurn`.

- [ ] **Step 3: Add the model and migration**

Define `HardwareVoiceTurn` with `UniqueConstraint("endpoint_id", "turn_id")`, indexed `user_id/status`, request and response byte counts/hashes/paths, transcript, reply text, message IDs, last error, and timestamps. The migration creates the same columns and foreign keys; `down_revision` is the current Alembic head `d1f6a3b9c204`.

```python
class HardwareVoiceTurn(Base):
    __tablename__ = "hardware_voice_turns"
    __table_args__ = (
        UniqueConstraint("endpoint_id", "turn_id", name="uq_hardware_voice_endpoint_turn"),
        Index("ix_hardware_voice_user_status", "user_id", "status"),
    )
    id = Column(Integer, primary_key=True)
    user_id = Column(Integer, ForeignKey("users.id"), nullable=False)
    endpoint_id = Column(Integer, ForeignKey("channel_endpoints.id"), nullable=False)
    identity_id = Column(Integer, ForeignKey("channel_identities.id"), nullable=False)
    turn_id = Column(String(64), nullable=False)
    status = Column(String(24), nullable=False, default="recorded")
    input_bytes = Column(Integer, nullable=False)
    input_sha256 = Column(String(64), nullable=False)
    input_audio_path = Column(String(512), nullable=False)
    transcript = Column(Text, default="")
    reply_text = Column(Text, default="")
    output_bytes = Column(Integer)
    output_sha256 = Column(String(64))
    output_audio_path = Column(String(512))
    inbound_message_id = Column(Integer, ForeignKey("messages.id"))
    outbound_message_id = Column(Integer, ForeignKey("messages.id"))
    last_error = Column(Text, default="")
    created_at = Column(DateTime, default=now_cn, nullable=False)
    updated_at = Column(DateTime, default=now_cn, onupdate=now_cn, nullable=False)
```

- [ ] **Step 4: Run model and migration checks**

Run: `JWT_SECRET=agent-test-secret PYTHONPATH=. ./venv/bin/pytest -q tests/test_device_voice_models.py && JWT_SECRET=agent-test-secret PYTHONPATH=. ./venv/bin/alembic upgrade head && JWT_SECRET=agent-test-secret PYTHONPATH=. ./venv/bin/alembic check`

Expected: test PASS; migration reaches head; Alembic reports no new upgrade operations.

- [ ] **Step 5: Commit the schema**

```bash
git add backend/models.py backend/alembic/versions/e4c7a1d9b205_add_hardware_voice_turns.py backend/tests/test_device_voice_models.py
git commit -m "feat(agent): persist hardware voice turns"
```

## Task 2: Authenticate one device without storing its raw token

**Files:**
- Create: `/Users/kanayama/Desktop/AI/agent/backend/services/device_voice_auth.py`
- Create: `/Users/kanayama/Desktop/AI/agent/backend/tests/test_device_voice_auth.py`
- Modify: `/Users/kanayama/Desktop/AI/agent/backend/services/AGENTS.md`

- [ ] **Step 1: Write failing token and ownership tests**

```python
def test_device_token_hash_never_contains_raw_token():
    encoded = hash_device_token("one-time-device-secret")
    assert "one-time-device-secret" not in encoded
    assert verify_device_token("one-time-device-secret", encoded)
    assert not verify_device_token("wrong", encoded)

def test_resolve_principal_requires_enabled_hardware_connector_and_bound_identity(db):
    principal = resolve_device_principal(db, device_id, token)
    assert principal.user_id == user.id
    assert principal.endpoint_id == endpoint.id
    connector.enabled = False
    assert resolve_device_principal(db, device_id, token) is None
```

- [ ] **Step 2: Run tests and see missing functions**

Run: `JWT_SECRET=agent-test-secret PYTHONPATH=. ./venv/bin/pytest -q tests/test_device_voice_auth.py`

Expected: FAIL importing `device_voice_auth`.

- [ ] **Step 3: Implement scrypt hashing and principal resolution**

```python
@dataclass(frozen=True)
class DeviceVoicePrincipal:
    connector_id: int
    endpoint_id: int
    identity_id: int
    user_id: int
    device_id: str

def hash_device_token(token: str, salt: Optional[bytes] = None) -> str:
    actual_salt = salt or secrets.token_bytes(16)
    digest = hashlib.scrypt(token.encode(), salt=actual_salt, n=2**14, r=8, p=1)
    return "scrypt$16384$8$1${}${}".format(
        base64.urlsafe_b64encode(actual_salt).decode(),
        base64.urlsafe_b64encode(digest).decode(),
    )
```

Store the encoded hash under `ChannelEndpoint.metadata_json["voice_token_hash"]`. Resolution requires connector type `hardware`, connector enabled, endpoint active, `metadata_json.voice_enabled is True`, and exactly one active bound Identity.

- [ ] **Step 4: Run the auth tests**

Run: `JWT_SECRET=agent-test-secret PYTHONPATH=. ./venv/bin/pytest -q tests/test_device_voice_auth.py`

Expected: PASS without reading any runtime `.env` or database.

- [ ] **Step 5: Commit auth**

```bash
git add backend/services/device_voice_auth.py backend/tests/test_device_voice_auth.py backend/services/AGENTS.md
git commit -m "feat(agent): authenticate hardware voice devices"
```

## Task 3: Add configurable 16 kHz TTS without changing Xiaolu output

**Files:**
- Modify: `/Users/kanayama/Desktop/AI/agent/backend/core/ai_pipeline.py`
- Modify: `/Users/kanayama/Desktop/AI/agent/backend/tests/test_ai_pipeline_logging.py`

- [ ] **Step 1: Add a failing output-format test**

```python
def test_tts_accepts_device_output_format(monkeypatch):
    sent = {}
    monkeypatch.setattr(ai_pipeline.requests, "post", lambda url, headers, data, timeout: sent.update(headers=headers) or FakeResponse(b"RIFF"))
    assert ai_pipeline.tts("你好", output_format="riff-16khz-16bit-mono-pcm") == b"RIFF"
    assert sent["headers"]["X-Microsoft-OutputFormat"] == "riff-16khz-16bit-mono-pcm"
```

- [ ] **Step 2: Verify the old signature fails**

Run: `JWT_SECRET=agent-test-secret PYTHONPATH=. ./venv/bin/pytest -q tests/test_ai_pipeline_logging.py::test_tts_accepts_device_output_format`

Expected: FAIL with unexpected keyword `output_format`.

- [ ] **Step 3: Add the optional parameter**

```python
def tts(text: str, voice="zh-CN-Xiaoxiao:DragonHDFlashLatestNeural", rate=1.0,
        output_format="riff-8khz-16bit-mono-pcm") -> bytes:
    # existing SSML construction remains unchanged
    headers["X-Microsoft-OutputFormat"] = output_format
```

Keep the existing 8 kHz default so Xiaolu AMR behavior is unchanged.

- [ ] **Step 4: Run focused and existing voice tests**

Run: `JWT_SECRET=agent-test-secret PYTHONPATH=. ./venv/bin/pytest -q tests/test_ai_pipeline_logging.py tests/test_long_voice_replies.py tests/test_broadcast.py`

Expected: PASS.

- [ ] **Step 5: Commit TTS support**

```bash
git add backend/core/ai_pipeline.py backend/tests/test_ai_pipeline_logging.py
git commit -m "feat(agent): support 16khz device speech"
```

## Task 4: Process a turn through the bound user's Opus assistant

**Files:**
- Create: `/Users/kanayama/Desktop/AI/agent/backend/services/device_voice_processor.py`
- Create: `/Users/kanayama/Desktop/AI/agent/backend/tests/test_device_voice_processor.py`
- Modify: `/Users/kanayama/Desktop/AI/agent/backend/services/AGENTS.md`

- [ ] **Step 1: Write failing isolated pipeline tests**

```python
def test_process_turn_uses_user_default_agent_and_persists_both_messages(fixture, monkeypatch):
    calls = install_fake_stt_gpt_tts(monkeypatch, transcript="今天天气怎么样", reply="今天晴，适合出门。")
    result = process_hardware_voice_turn(fixture.session_factory, fixture.turn_id)
    assert result.transcript == "今天天气怎么样"
    assert result.reply_text == "今天晴，适合出门。"
    assert calls.gpt_provider["model"] == "claude-opus-4-8"
    with fixture.session_factory() as db:
        rows = db.query(Message).filter_by(user_id=fixture.user.id).order_by(Message.id).all()
        assert [row.direction for row in rows] == ["in", "out"]
        stored = db.query(HardwareVoiceTurn).filter_by(turn_id=fixture.turn_id).one()
        assert stored.status == "complete"
```

Add a second test where another user's default assistant exists; assert it is never selected.

- [ ] **Step 2: Run tests and verify the service is absent**

Run: `JWT_SECRET=agent-test-secret PYTHONPATH=. ./venv/bin/pytest -q tests/test_device_voice_processor.py`

Expected: FAIL importing `process_hardware_voice_turn`.

- [ ] **Step 3: Implement the transaction-safe processor**

The service loads the turn and principal IDs, closes the read transaction before network calls, reads the stored WAV, calls `stt_dispatch`, selects `AIAgent(user_id=user_id, is_default=True, enabled=True)`, builds messages through `build_user_messages`, calls `gpt(messages, provider=provider)`, and calls `tts(reply_text, output_format="riff-16khz-16bit-mono-pcm")`. It writes the reply atomically, calculates SHA-256, and then commits Message, Conversation, LLMTrace, and completed turn fields together.

```python
@dataclass(frozen=True)
class ProcessedVoiceTurn:
    turn_id: str
    transcript: str
    reply_text: str
    output_path: str
    output_bytes: int
    output_sha256: str

def process_hardware_voice_turn(session_factory, turn_pk: int) -> ProcessedVoiceTurn:
    # load immutable ownership/config snapshot
    # perform STT/LLM/TTS without holding a DB transaction
    # atomically write WAV, then commit owned rows and status
```

On exceptions, set status `failed` and a sanitized `last_error`, then re-raise. Never put provider keys or tokens in the error field.

- [ ] **Step 4: Run processor, isolation, prompt, and queue tests**

Run: `JWT_SECRET=agent-test-secret PYTHONPATH=. ./venv/bin/pytest -q tests/test_device_voice_processor.py tests/test_context_disclosure.py tests/test_user_queue.py`

Expected: PASS; mocks prove no network call escapes.

- [ ] **Step 5: Commit the processor**

```bash
git add backend/services/device_voice_processor.py backend/tests/test_device_voice_processor.py backend/services/AGENTS.md
git commit -m "feat(agent): process user-scoped device voice turns"
```

## Task 5: Implement the bounded WebSocket transfer protocol

**Files:**
- Create: `/Users/kanayama/Desktop/AI/agent/backend/services/device_voice_protocol.py`
- Create: `/Users/kanayama/Desktop/AI/agent/backend/tests/test_device_voice_protocol.py`

- [ ] **Step 1: Write failing state, size, and hash tests**

```python
def test_upload_requires_ready_then_exact_binary_size(tmp_path):
    upload = VoiceUpload(tmp_path, max_bytes=4 * 1024 * 1024)
    upload.start(TurnStart(turn_id="t1", bytes=48, sha256=sha256(WAV)))
    upload.append(WAV[:20])
    upload.append(WAV[20:])
    completed = upload.finish("t1")
    assert completed.bytes == 48
    assert completed.path.read_bytes() == WAV

def test_bad_hash_removes_partial_file(tmp_path):
    upload = VoiceUpload(tmp_path, max_bytes=100)
    upload.start(TurnStart(turn_id="t1", bytes=48, sha256="0" * 64))
    upload.append(WAV)
    with pytest.raises(ProtocolError):
        upload.finish("t1")
    assert not list(tmp_path.glob("*.part"))
```

- [ ] **Step 2: Run and verify missing protocol types**

Run: `JWT_SECRET=agent-test-secret PYTHONPATH=. ./venv/bin/pytest -q tests/test_device_voice_protocol.py`

Expected: FAIL importing `VoiceUpload`.

- [ ] **Step 3: Implement typed control parsing and streaming files**

```python
@dataclass(frozen=True)
class TurnStart:
    turn_id: str
    bytes: int
    sha256: str
    sample_rate: int = 16000
    channels: int = 1

class VoiceUpload:
    def start(self, frame: TurnStart) -> None:
        validate_turn_start(frame, self.max_bytes)
        if self.file is not None:
            raise ProtocolError("upload already active")
        self.frame = frame
        self.part_path = self.root / (frame.turn_id + ".wav.part")
        self.file = self.part_path.open("xb")
        self.digest = hashlib.sha256()
        self.received = 0

    def append(self, data: bytes) -> None:
        if self.file is None or not data or len(data) > 4096:
            raise ProtocolError("invalid binary chunk")
        if self.received + len(data) > self.frame.bytes:
            raise ProtocolError("upload exceeds declared size")
        self.file.write(data)
        self.digest.update(data)
        self.received += len(data)

    def finish(self, turn_id: str) -> CompletedUpload:
        if self.file is None or turn_id != self.frame.turn_id:
            raise ProtocolError("turn does not match active upload")
        self.file.flush()
        os.fsync(self.file.fileno())
        self.file.close()
        self.file = None
        if self.received != self.frame.bytes or self.digest.hexdigest() != self.frame.sha256:
            self.abort()
            raise ProtocolError("uploaded WAV failed integrity check")
        final_path = self.root / (turn_id + ".wav")
        os.replace(self.part_path, final_path)
        return CompletedUpload(final_path, self.received, self.digest.hexdigest())

    def abort(self) -> None:
        if self.file is not None:
            self.file.close()
            self.file = None
        if self.part_path is not None:
            self.part_path.unlink(missing_ok=True)
```

Validation accepts only 16 kHz mono PCM WAV, turn IDs matching `[A-Za-z0-9._-]{1,64}`, maximum 4 MiB, binary chunks at most 4096 bytes, and an exact byte count and SHA-256. `finish` uses `os.replace` from `.part` to `.wav`.

- [ ] **Step 4: Run protocol tests**

Run: `JWT_SECRET=agent-test-secret PYTHONPATH=. ./venv/bin/pytest -q tests/test_device_voice_protocol.py`

Expected: PASS for happy path, oversize, overflow, hash mismatch, unknown state, and cleanup.

- [ ] **Step 5: Commit protocol core**

```bash
git add backend/services/device_voice_protocol.py backend/tests/test_device_voice_protocol.py
git commit -m "feat(agent): validate device voice transfer frames"
```

## Task 6: Expose the authenticated WebSocket and replay completed turns

**Files:**
- Create: `/Users/kanayama/Desktop/AI/agent/backend/api/device_voice.py`
- Create: `/Users/kanayama/Desktop/AI/agent/backend/tests/test_device_voice_websocket.py`
- Modify: `/Users/kanayama/Desktop/AI/agent/backend/main.py`
- Modify: `/Users/kanayama/Desktop/AI/agent/backend/config.py`
- Modify: `/Users/kanayama/Desktop/AI/agent/backend/.env.example`
- Modify: `/Users/kanayama/Desktop/AI/agent/backend/api/AGENTS.md`

- [ ] **Step 1: Write failing TestClient WebSocket tests**

```python
def test_device_voice_round_trip_and_duplicate_replay(client, provisioned_device, monkeypatch):
    calls = install_fake_processor(monkeypatch)
    headers = {"Authorization": "Bearer test-token", "Device-Id": "device-one", "Protocol-Version": "1"}
    with client.websocket_connect(
        "/api/device/voice/ws",
        headers=headers,
    ) as ws:
        assert ws.receive_json()["type"] == "ready"
        send_wav_turn(ws, "turn-1", WAV)
        reply = receive_reply(ws)
        assert reply.audio == REPLY_WAV
    with client.websocket_connect("/api/device/voice/ws", headers=headers) as ws:
        ws.receive_json()
        send_wav_turn(ws, "turn-1", WAV)
        assert receive_reply(ws).audio == REPLY_WAV
    assert calls.count == 1
```

Add rejection tests for disabled global flag, disabled Endpoint, wrong token, wrong protocol, invalid binary order, and unknown device; assert no Identity, Message, or turn is created on authentication failure.

- [ ] **Step 2: Run and see route-not-found/disconnect failures**

Run: `JWT_SECRET=agent-test-secret HARDWARE_VOICE_ENABLED=true PYTHONPATH=. ./venv/bin/pytest -q tests/test_device_voice_websocket.py`

Expected: FAIL because `/api/device/voice/ws` is absent.

- [ ] **Step 3: Implement WebSocket orchestration**

Register `device_voice.router` in `main.py`. The route accepts only when `config.HARDWARE_VOICE_ENABLED`, authenticates before `websocket.accept()`, sends `ready`, receives one turn at a time, persists or finds the unique turn, calls `await asyncio.to_thread(process_hardware_voice_turn, SessionLocal, turn.id)`, and streams the reply in 4096-byte frames.

```python
HARDWARE_VOICE_ENABLED = os.getenv("HARDWARE_VOICE_ENABLED", "false").lower() == "true"
HARDWARE_VOICE_ROOT = os.getenv("HARDWARE_VOICE_ROOT", os.path.join(AUDIO_CACHE_DIR, "hardware"))
HARDWARE_VOICE_MAX_BYTES = int(os.getenv("HARDWARE_VOICE_MAX_BYTES", str(4 * 1024 * 1024)))
```

The completed-turn branch sends existing `reply_start` metadata and stored WAV without invoking the processor. It waits for `reply_saved`, responds to JSON `ping` with `pong`, and removes partial uploads in `finally`.

- [ ] **Step 4: Run WebSocket, security, and health tests**

Run: `JWT_SECRET=agent-test-secret HARDWARE_VOICE_ENABLED=true PYTHONPATH=. ./venv/bin/pytest -q tests/test_device_voice_websocket.py tests/test_hardware_webhook.py tests/test_webhook_security.py tests/test_health_api.py`

Expected: PASS; `/api/health` exposes only a boolean hardware-voice state, never device IDs or hashes.

- [ ] **Step 5: Commit WebSocket transport**

```bash
git add backend/api/device_voice.py backend/tests/test_device_voice_websocket.py backend/main.py backend/config.py backend/.env.example backend/api/AGENTS.md
git commit -m "feat(agent): serve device voice WebSocket"
```

## Task 7: Add secret-safe Azure bootstrap and deploy checks

**Files:**
- Create: `/Users/kanayama/Desktop/AI/agent/backend/scripts/bootstrap_device_voice.py`
- Create: `/Users/kanayama/Desktop/AI/agent/backend/scripts/sync_xiaolu_voice_env.py`
- Create: `/Users/kanayama/Desktop/AI/agent/backend/tests/test_device_voice_bootstrap.py`
- Modify: `/Users/kanayama/Desktop/AI/agent/deploy/nginx-agent.conf`
- Modify: `/Users/kanayama/Desktop/AI/agent/deploy/verify.sh`
- Modify: `/Users/kanayama/Desktop/AI/agent/docs/operations.md`
- Modify: `/Users/kanayama/Desktop/AI/agent/backend/scripts/AGENTS.md`
- Modify: `/Users/kanayama/Desktop/AI/agent/deploy/AGENTS.md`

- [ ] **Step 1: Write failing bootstrap idempotency and redaction tests**

```python
def test_bootstrap_creates_one_user_agent_endpoint_and_identity(tmp_path, capsys):
    run_bootstrap(agent_db, xiaolu_db, device_id="device-one", token="raw-secret")
    run_bootstrap(agent_db, xiaolu_db, device_id="device-one", token="raw-secret")
    assert count(agent_db, "users", "display_name='金山'") == 1
    assert scalar(agent_db, "select model from llm_providers") == "claude-opus-4-8"
    assert count(agent_db, "hardware_voice_turns", "1=1") == 0
    assert "raw-secret" not in capsys.readouterr().out

def test_env_sync_copies_only_named_speech_keys_and_preserves_mode(tmp_path):
    sync_voice_env(source, target)
    assert read_env(target)["AZURE_STT_KEY"] == "source-stt"
    assert read_env(target)["AZURE_TTS_KEY"] == "source-tts"
    assert read_env(target)["JWT_SECRET"] == "existing-jwt"
    assert stat.S_IMODE(target.stat().st_mode) == 0o600
```

- [ ] **Step 2: Run tests and verify scripts are absent**

Run: `JWT_SECRET=agent-test-secret PYTHONPATH=. ./venv/bin/pytest -q tests/test_device_voice_bootstrap.py`

Expected: FAIL importing bootstrap modules.

- [ ] **Step 3: Implement idempotent bootstrap**

`bootstrap_device_voice.py` accepts `--xiaolu-db`, `--device-id`, and reads the raw token only from `DEVICE_VOICE_TOKEN_FILE`. It copies the enabled provider named `Opus 4.8`, creates `金山`, `金山AI语音助手`, hardware Connector `XiaoZhi Voice`, trusted/full device Endpoint, and active default Identity. It stores only `hash_device_token(token)` and reports row IDs/model names, never endpoints, keys, tokens, or hashes.

`sync_xiaolu_voice_env.py` copies exactly `AZURE_STT_KEY`, `AZURE_STT_REGION`, `AZURE_TTS_KEY`, and `AZURE_TTS_REGION`, writes a temporary file, sets `0600`, and atomically replaces the target.

- [ ] **Step 4: Add Nginx/verify behavior and run checks**

Set WebSocket read/send timeout to `300s`. Extend `verify.sh` to verify `/api/health`, Alembic head, and that non-voice connectors remain disabled; never print config values.

Run: `JWT_SECRET=agent-test-secret PYTHONPATH=. ./venv/bin/pytest -q tests/test_device_voice_bootstrap.py tests/test_deploy_artifacts.py tests/test_agent_baseline_safety.py && bash -n deploy/*.sh`

Expected: PASS and shell syntax clean.

- [ ] **Step 5: Commit bootstrap and operations**

```bash
git add backend/scripts/bootstrap_device_voice.py backend/scripts/sync_xiaolu_voice_env.py backend/tests/test_device_voice_bootstrap.py deploy/nginx-agent.conf deploy/verify.sh docs/operations.md backend/scripts/AGENTS.md deploy/AGENTS.md
git commit -m "feat(agent): provision Xiaozhi voice endpoint safely"
```

## Task 8: Define the firmware voice-assistant state machine

**Files:**
- Create: `/Users/kanayama/xiaozhi-kb/main/apps/recorder/agent_voice_state.h`
- Create: `/Users/kanayama/xiaozhi-kb/main/apps/recorder/agent_voice_state.cc`
- Create: `/Users/kanayama/xiaozhi-kb/main/apps/recorder/agent_voice_state_test.cc`

- [ ] **Step 1: Write the failing pure reducer test**

```cpp
int main() {
    AgentVoiceState state{};
    assert(AgentVoiceReduce(&state, AgentVoiceEvent::kWifiConnected) == AgentVoiceAction::kConnectSocket);
    assert(AgentVoiceReduce(&state, AgentVoiceEvent::kServerReady) == AgentVoiceAction::kSendQueuedTurn);
    assert(AgentVoiceReduce(&state, AgentVoiceEvent::kTurnAccepted) == AgentVoiceAction::kWaitForReply);
    assert(AgentVoiceReduce(&state, AgentVoiceEvent::kReplyComplete) == AgentVoiceAction::kPlayReply);
    assert(AgentVoiceReduce(&state, AgentVoiceEvent::kDisconnected) == AgentVoiceAction::kScheduleReconnect);
}
```

- [ ] **Step 2: Compile and verify missing types**

Run: `c++ -std=c++17 -Wall -Wextra -Werror -I main/apps/recorder main/apps/recorder/agent_voice_state_test.cc main/apps/recorder/agent_voice_state.cc -o /tmp/agent_voice_state_test`

Expected: compile FAIL because files/types do not exist.

- [ ] **Step 3: Implement explicit phases and actions**

```cpp
enum class AgentVoicePhase { kOffline, kConnecting, kOnline, kSending, kThinking, kReceiving, kReadyToPlay, kError };
struct AgentVoiceState { AgentVoicePhase phase = AgentVoicePhase::kOffline; bool queued_turn = false; };
AgentVoiceAction AgentVoiceReduce(AgentVoiceState* state, AgentVoiceEvent event);
const char* AgentVoicePhaseTitle(AgentVoicePhase phase);
```

Reject a new recording while sending/thinking/receiving; disconnect preserves `queued_turn`; `reply_saved` transitions to ready-to-play only after hash validation.

- [ ] **Step 4: Compile and run the reducer test**

Run: previous compile command followed by `/tmp/agent_voice_state_test`.

Expected: exit 0.

- [ ] **Step 5: Commit state machine**

```bash
git add main/apps/recorder/agent_voice_state.*
git commit -m "feat(recorder): add Agent voice state machine"
```

## Task 9: Store complete turns and recover queued recordings on SD

**Files:**
- Create: `/Users/kanayama/xiaozhi-kb/main/apps/recorder/agent_turn_store.h`
- Create: `/Users/kanayama/xiaozhi-kb/main/apps/recorder/agent_turn_store.cc`
- Create: `/Users/kanayama/xiaozhi-kb/main/apps/recorder/agent_turn_store_test.cc`

- [ ] **Step 1: Write failing atomic-manifest and recovery tests**

```cpp
int main() {
    TempDir root;
    AgentTurnStore store(root.path());
    auto turn = store.Create("20260712", "turn-1");
    WriteBytes(turn.user_wav, kWav);
    assert(store.MarkRecorded(turn, Sha256(kWav), sizeof(kWav)));
    auto pending = store.ListPending();
    assert(pending.size() == 1 && pending[0].turn_id == "turn-1");
    assert(store.BeginReply(turn, sizeof(kReply), Sha256(kReply)));
    assert(store.AppendReply(kReply, sizeof(kReply)));
    assert(store.CommitReply("用户文字", "助手文字"));
    assert(FileExists(turn.assistant_wav));
    assert(store.ListPending().empty());
}
```

- [ ] **Step 2: Compile and verify missing store**

Run: `c++ -std=c++17 -Wall -Wextra -Werror -I main/apps/recorder main/apps/recorder/agent_turn_store_test.cc main/apps/recorder/agent_turn_store.cc -o /tmp/agent_turn_store_test`

Expected: compile FAIL.

- [ ] **Step 3: Implement bounded paths and atomic JSON**

```cpp
struct AgentTurnPaths {
    std::string turn_id, directory, user_wav, assistant_wav, manifest;
};
class AgentTurnStore {
 public:
    explicit AgentTurnStore(std::string root);
    AgentTurnPaths Create(const std::string& date, const std::string& turn_id);
    bool MarkRecorded(const AgentTurnPaths&, const std::string& sha256, uint32_t bytes);
    std::vector<AgentPendingTurn> ListPending() const;
    bool BeginReply(const AgentTurnPaths&, uint32_t bytes, const std::string& sha256);
    bool AppendReply(const uint8_t*, size_t);
    bool CommitReply(const std::string& transcript, const std::string& reply_text);
    void AbortReply();
};
```

Use `turn.json.part` + `rename`, `assistant.wav.part` + hash + `rename`, and append a compact entry to `/sdcard/agent/turns.jsonl`. Reject traversal characters and paths over the fixed buffer limit. A `.part` file is never playable.

- [ ] **Step 4: Run store test including simulated interrupted reply**

Run: compile and run test; add a second invocation that constructs a new store after `AbortReply()` and confirms the user WAV remains pending and assistant `.part` is removed.

Expected: exit 0.

- [ ] **Step 5: Commit SD turn store**

```bash
git add main/apps/recorder/agent_turn_store.*
git commit -m "feat(recorder): persist Agent turns on SD"
```

## Task 10: Implement firmware control-frame and chunk bookkeeping

**Files:**
- Create: `/Users/kanayama/xiaozhi-kb/main/apps/recorder/agent_voice_protocol.h`
- Create: `/Users/kanayama/xiaozhi-kb/main/apps/recorder/agent_voice_protocol.cc`
- Create: `/Users/kanayama/xiaozhi-kb/main/apps/recorder/agent_voice_protocol_test.cc`

- [ ] **Step 1: Write failing frame tests**

```cpp
int main() {
    auto start = AgentVoiceBuildTurnStart("turn-1", 48, std::string(64, 'a'));
    assert(start.find("\"type\":\"turn_start\"") != std::string::npos);
    AgentReplyStart reply{};
    assert(AgentVoiceParseReplyStart(
        R"({"type":"reply_start","turn_id":"turn-1","bytes":52,"sha256":"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb","transcript":"你好","reply_text":"你好呀"})",
        &reply));
    assert(reply.bytes == 52 && reply.reply_text == "你好呀");
    assert(!AgentVoiceParseReplyStart(R"({"type":"reply_start","bytes":5000000})", &reply));
}
```

- [ ] **Step 2: Compile and see missing protocol API**

Run: `c++ -std=c++17 -Wall -Wextra -Werror -I main/apps/recorder main/apps/recorder/agent_voice_protocol_test.cc main/apps/recorder/agent_voice_protocol.cc -o /tmp/agent_voice_protocol_test`

Expected: compile FAIL.

- [ ] **Step 3: Implement strict builders/parsers**

Define exact frame types `ready`, `turn_ready`, `turn_accepted`, `reply_start`, `reply_end`, `error`, `ping`, and `pong`. Enforce protocol 1, 4 MiB maximum, 64-character SHA-256 hex, matching turn IDs, and UTF-8 strings. ESP build uses cJSON; the host test compiles the field validation helpers without ESP headers.

- [ ] **Step 4: Run protocol tests**

Run: compile and run `/tmp/agent_voice_protocol_test`.

Expected: exit 0 for valid Unicode, invalid sizes, invalid hashes, unknown frames, and mismatched turn IDs.

- [ ] **Step 5: Commit firmware protocol**

```bash
git add main/apps/recorder/agent_voice_protocol.*
git commit -m "feat(recorder): encode Agent voice protocol"
```

## Task 11: Connect Wi-Fi and WSS without duplicating board peripherals

**Files:**
- Create: `/Users/kanayama/xiaozhi-kb/main/apps/recorder/recorder_network.h`
- Create: `/Users/kanayama/xiaozhi-kb/main/apps/recorder/recorder_network.cc`
- Create: `/Users/kanayama/xiaozhi-kb/main/apps/recorder/recorder_network_events_test.cc`
- Modify: `/Users/kanayama/xiaozhi-kb/main/Kconfig.projbuild`
- Modify: `/Users/kanayama/xiaozhi-kb/main/CMakeLists.txt`

- [ ] **Step 1: Write the failing reconnect reducer test**

```cpp
int main() {
    RecorderReconnectPolicy policy;
    assert(policy.NextDelayMs() >= 1000 && policy.NextDelayMs() < 2000);
    assert(policy.NextDelayMs() >= 2000);
    for (int i = 0; i < 10; ++i) policy.NextDelayMs();
    assert(policy.PeekBaseDelayMs() == 30000);
    policy.Reset();
    assert(policy.PeekBaseDelayMs() == 1000);
}
```

- [ ] **Step 2: Compile and verify the network policy is absent**

Run: `c++ -std=c++17 -Wall -Wextra -Werror -I main/apps/recorder main/apps/recorder/recorder_network_events_test.cc main/apps/recorder/recorder_network.cc -DRECORDER_NETWORK_HOST_TEST -o /tmp/recorder_network_events_test`

Expected: compile FAIL.

- [ ] **Step 3: Implement network ownership and build-time provisioning**

Add Kconfig strings `AGENT_VOICE_URL` defaulting to the public WSS URL and `AGENT_VOICE_TOKEN` defaulting empty. `sdkconfig` is ignored and is the only local file containing the raw token.

```cpp
class RecorderNetwork {
 public:
    void Start(EventCallback callback);
    void Poll();
    bool SendText(const std::string& text);
    bool SendBinary(const void* data, size_t size);
    void Stop();
};
```

Initialize `WifiManager`/`SsidManager` directly so Recorder does not construct `Board` and duplicate I2C/display/audio. Reuse the existing Xiaozhi SSID list; if none exists, publish `kWifiNeedsProvisioning` and instruct the user to configure Wi-Fi in XiaoZhi mode. Create `EspNetwork` and `WebSocket`, set Authorization, Device-Id, and Protocol-Version headers, cap receive frames at 8192 bytes, and copy callback data into a bounded queue. Callbacks never write FATFS or LVGL.

- [ ] **Step 4: Run host test and ESP build smoke test**

Run host compile/test, then `source ~/esp/esp-idf/export.sh && idf.py reconfigure`.

Expected: host exit 0; ESP component configuration accepts the new sources and Kconfig values.

- [ ] **Step 5: Commit network client**

```bash
git add main/apps/recorder/recorder_network.* main/apps/recorder/recorder_network_events_test.cc main/Kconfig.projbuild main/CMakeLists.txt
git commit -m "feat(recorder): connect to Agent voice WebSocket"
```

## Task 12: Integrate recording, queued upload, reply storage, and automatic playback

**Files:**
- Modify: `/Users/kanayama/xiaozhi-kb/main/apps/recorder/recorder_app.cc`
- Modify: `/Users/kanayama/xiaozhi-kb/main/apps/recorder/recorder_display.h`
- Modify: `/Users/kanayama/xiaozhi-kb/main/apps/recorder/recorder_display.cc`
- Modify: `/Users/kanayama/xiaozhi-kb/main/apps/recorder/recorder_control_state.h`
- Modify: `/Users/kanayama/xiaozhi-kb/main/apps/recorder/recorder_control_state.cc`
- Modify: `/Users/kanayama/xiaozhi-kb/main/apps/recorder/recorder_control_state_test.cc`
- Modify: `/Users/kanayama/xiaozhi-kb/main/apps/recorder/recorder_file_list.*`
- Modify: `/Users/kanayama/xiaozhi-kb/main/apps/recorder/recorder_playback_menu_test.cc`

- [ ] **Step 1: Extend failing control and menu tests**

Add assertions that recording remains allowed offline, a second recording is rejected during sending/thinking/receiving, a completed reply emits `kStartPlayback`, and assistant WAV entries sort next to their user WAV by turn time.

```cpp
state.voice_phase = AgentVoicePhase::kThinking;
assert(RecorderControlReduce(&state, RecorderControlEvent::kTouchRecord) == RecorderControlAction::kNone);
state.voice_phase = AgentVoicePhase::kReadyToPlay;
assert(RecorderControlReduce(&state, RecorderControlEvent::kAgentReplyReady) == RecorderControlAction::kStartAgentReplyPlayback);
```

- [ ] **Step 2: Run current recorder tests and observe new expectations fail**

Run the recorder control and playback menu compile commands from README.

Expected: compile/test FAIL until Agent state is integrated.

- [ ] **Step 3: Integrate one-task SD/codec orchestration**

Replace `recN.wav` creation with a turn directory and `user.wav`; after `FinishRecording`, calculate SHA-256, write `recorded` manifest, and queue it. In the main loop:

1. poll copied network events;
2. send `turn_start`, wait for `turn_ready`, pause display, stream 4096-byte SD reads, resume display, and send `turn_end`;
3. on `reply_start`, pause display only around each SD append, never inside WebSocket callbacks;
4. after `reply_end`, validate/rename through `AgentTurnStore`, send `reply_saved`, and call existing `PlayWavFile` on `assistant.wav`;
5. reconnect/re-enter scans `ListPending()` and resends oldest first.

Expose status text `OFFLINE`, `ONLINE`, `QUEUED`, `SENDING`, `THINKING`, `RECEIVING`, and `PLAYING`. Keep pause/resume and volume behavior during automatic playback. `MENU` closes network after safely finishing any active file.

- [ ] **Step 4: Run all recorder host tests**

Run every `main/apps/recorder/*_test.cc` command listed in README plus the three new test binaries.

Expected: all exit 0; no test needs ESP hardware or network.

- [ ] **Step 5: Commit recorder integration**

```bash
git add main/apps/recorder
git commit -m "feat(recorder): exchange durable Agent voice turns"
```

## Task 13: Document and fully verify both repositories locally

**Files:**
- Modify: `/Users/kanayama/xiaozhi-kb/README.md`
- Modify: `/Users/kanayama/xiaozhi-kb/AGENTS.md`
- Modify: `/Users/kanayama/xiaozhi-kb/main/boards/waveshare/esp32-c6-touch-amoled-2.16/AGENTS.md`
- Create: `/Users/kanayama/xiaozhi-kb/scripts/verify_agent_voice_runtime.py`
- Modify: `/Users/kanayama/xiaozhi-kb/scripts/AGENTS.md`
- Modify: `/Users/kanayama/Desktop/AI/agent/README.md`
- Modify: `/Users/kanayama/Desktop/AI/agent/AGENTS.md`
- Modify: `/Users/kanayama/Desktop/AI/agent/docs/acceptance-report.md`

- [ ] **Step 1: Add a serial verifier before documenting success**

The script uses safe `dtr=True/rts=False`, observes at least 30 seconds, and requires logs for Wi-Fi connected, WSS ready, recorded user path, accepted turn, stored assistant path, and playback start. It returns 1 for SPI2 assertion, stack protection fault, reboot loop, hash failure, or missing expected milestones.

- [ ] **Step 2: Run all Agent local checks**

```bash
cd /Users/kanayama/Desktop/AI/agent/backend
JWT_SECRET=agent-test-secret HARDWARE_VOICE_ENABLED=true PYTHONPATH=. ./venv/bin/alembic upgrade head
JWT_SECRET=agent-test-secret HARDWARE_VOICE_ENABLED=true PYTHONPATH=. ./venv/bin/alembic check
JWT_SECRET=agent-test-secret HARDWARE_VOICE_ENABLED=true PYTHONPATH=. ./venv/bin/pytest -q
cd ../frontend && npm test && npm run build
cd .. && bash -n deploy/*.sh
```

Expected: Alembic current; full pytest PASS; frontend tests/build PASS; scripts parse.

- [ ] **Step 3: Run firmware tests and full build**

Run all README host commands, `python3 -m py_compile scripts/verify_agent_voice_runtime.py`, then:

```bash
source ~/esp/esp-idf/export.sh
idf.py fullclean
idf.py set-target esp32c6
idf.py build
```

Expected: all host tests pass and `build/xiaozhi.bin` is produced within the configured app partition.

- [ ] **Step 4: Update docs with only verified behavior**

Document WSS/SD directory/state behavior, build-time secret provisioning, Azure bootstrap and rollback, and SPI2/network callback constraints. Do not include token, device identity values, provider endpoint, or keys.

- [ ] **Step 5: Commit documentation in each repository**

```bash
# firmware
git add README.md AGENTS.md main/boards/waveshare/esp32-c6-touch-amoled-2.16/AGENTS.md scripts/verify_agent_voice_runtime.py scripts/AGENTS.md
git commit -m "docs: describe Agent voice assistant operations"

# Agent
git add README.md AGENTS.md docs/acceptance-report.md
git commit -m "docs: record hardware voice assistant verification"
```

## Task 14: Deploy and provision Azure without exposing Xiaolu secrets

**Files:**
- Runtime only: `/home/azureuser/agent/backend/.env`
- Runtime only: `/home/azureuser/agent/backend/data/agent.db`
- Runtime only: `/home/azureuser/agent/backend/audio_cache/hardware/`
- Runtime only: `/etc/nginx/sites-enabled/agent-platform.conf`

- [ ] **Step 1: Run deployment dry-run**

Run: `cd /Users/kanayama/Desktop/AI/agent && ./deploy/deploy.sh --dry-run`

Expected: only `/home/azureuser/agent` changes; runtime env/database/audio/log files excluded; Xiaolu paths untouched.

- [ ] **Step 2: Back up Agent and apply code/migration**

Run: `./deploy/deploy.sh --apply`

Expected: source deployed, Alembic upgraded, only `agent-platform.service` restarted, service active on `127.0.0.1:8021`.

- [ ] **Step 3: Sync speech config and bootstrap the device internally**

Create a local mode-0600 temporary token file without printing it, copy it to a mode-0600 Azure temporary file, then run the two scripts with source paths under `/home/azureuser/xiaolu` and target paths under `/home/azureuser/agent`. Delete both token temp files immediately after successful bootstrap.

Expected output contains only created/reused row IDs and `claude-opus-4-8`; it contains no URLs, keys, token, token hash, phone, or user external IDs.

- [ ] **Step 4: Enable only hardware voice and restart Agent**

Set `HARDWARE_VOICE_ENABLED=true`, `HARDWARE_VOICE_ROOT=/home/azureuser/agent/backend/audio_cache/hardware`, retain `ENVIRONMENT=precutover` and `PRODUCTION_SENDS_ENABLED=false`, run `nginx -t`, install/reload the exact Agent vhost if changed, and restart only `agent-platform.service`.

- [ ] **Step 5: Verify remote state**

Run `./deploy/verify.sh`, `curl -fsS https://agent.jinshanweb.com/api/health`, a bad-token WSS probe, and a valid-token scripted sample turn using a synthetic WAV and mocked/real services as appropriate.

Expected: service and TLS healthy; bad token rejected without rows; valid turn reaches complete; database query proves 金山 → default assistant → Opus 4.8 and hardware Endpoint binding; all unrelated connectors disabled.

## Task 15: Provision, flash, and prove the physical device loop

**Files:**
- Ignored runtime config: `/Users/kanayama/xiaozhi-kb/sdkconfig`
- Build output: `/Users/kanayama/xiaozhi-kb/build/`
- Device storage: `/sdcard/agent/`

- [ ] **Step 1: Put the same raw device token into ignored sdkconfig**

Use a local script reading the mode-0600 token file to set `CONFIG_AGENT_VOICE_TOKEN` without printing it. Set `CONFIG_AGENT_VOICE_URL="wss://agent.jinshanweb.com/api/device/voice/ws"`. Confirm `git status` does not show sdkconfig or any secret file.

- [ ] **Step 2: Rebuild and flash the complete image set**

Build with ESP-IDF 5.5.3. Flash bootloader, partition table, OTA data, app, and assets at the repository's verified offsets; do not erase NVS or the full-device backup. Explicitly select the intended OTA slot.

Expected: flash verification succeeds and existing Wi-Fi credentials remain.

- [ ] **Step 3: Perform the required physical cold boot**

Disconnect USB and battery for 20 seconds, reconnect without BOOT, then use only safe serial monitoring. Enter Recorder/Agent mode from the selector.

Expected: display reaches `ONLINE`; no ROM download mode, SPI2 assertion, stack protection fault, or reboot loop.

- [ ] **Step 4: Verify real record → AI → store → autoplay**

Record a spoken question and stop. Confirm serial milestones and audible automatic playback. Inspect SD through the firmware/serial tooling and prove the turn directory contains `user.wav`, `assistant.wav`, `turn.json`, and an index line. Query Agent with a secret-free script to prove the turn's user is 金山 and provider model is `claude-opus-4-8`.

- [ ] **Step 5: Verify offline queue and interrupted reply recovery**

Disable Wi-Fi, record one turn, restore Wi-Fi, and confirm one reply only. Interrupt one reply download, reconnect, and confirm `.part` is never played and the final validated WAV is. Return to selector and re-enter; run `verify_agent_voice_runtime.py --duration 30`.

Expected: queued turn completes exactly once, interrupted turn resumes/replays safely, runtime verifier exits 0.

- [ ] **Step 6: Record final evidence and commit no secrets**

Update acceptance documents with command results, timestamps, model name, turn IDs redacted to prefixes, and paths without raw device ID. Run `git status`, secret scans, and final repository tests before the final evidence commit.

## Task 16: Requirement-by-requirement completion audit

**Files:**
- Modify only if evidence changed: both repositories' acceptance/operations docs.

- [ ] **Step 1: Audit explicit goal requirements**

Create an evidence table for Azure connection, recording, Agent processing, automatic playback, all SD artifacts, 金山 user, dedicated assistant, Opus 4.8, Xiaolu-derived configuration, offline recovery, deployment, and physical-device behavior. Each row must cite a current command result, database query, SD artifact, or serial/audible test.

- [ ] **Step 2: Re-run final local and remote verifiers**

Run both full test suites/builds, `deploy/verify.sh`, health/TLS checks, secret scans, and the physical runtime verifier. Do not use earlier output if code or runtime state changed afterward.

- [ ] **Step 3: Check clean, intentional Git state**

Run `git status --short` and `git log -8 --oneline` in both repositories. Any remaining changes must be explained and committed if they belong to this work; no runtime DB, env, audio, build, or token file may be tracked.

- [ ] **Step 4: Mark the goal complete only when every row is proven**

If any requirement lacks authoritative evidence, continue implementation or verification. Only after all rows are proven should the active goal be completed and handed off.
