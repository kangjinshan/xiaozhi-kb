import importlib.util
from pathlib import Path
import tempfile

spec=importlib.util.spec_from_file_location('ws_patch',Path(__file__).with_name('patch_recorder_websocket.py'))
module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module)
with tempfile.TemporaryDirectory() as tmp:
    source=Path(tmp)/'web_socket.cc';source.write_text(module.OLD+'\n}\n')
    assert module.patch(source)
    result=source.read_text();assert not module.patch(source) and source.read_text()==result
    source.write_text('unexpected dependency revision')
    try: module.patch(source)
    except RuntimeError: pass
    else: raise AssertionError('unsupported revisions must fail closed')
    assert source.read_text()=='unexpected dependency revision'
print('WebSocket patch idempotency and version guard passed')
