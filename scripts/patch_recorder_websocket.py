#!/usr/bin/env python3
"""Patch the pinned WebSocket dependency's reader lifetime before compilation."""
from pathlib import Path
import argparse

OLD = '''WebSocket::~WebSocket() {
    if (connected_) {
        tcp_->Disconnect();
    }'''
NEW = '''WebSocket::~WebSocket() {
    // Stop/join the reader even after a failed HTTP upgrade. Its callbacks own
    // this object's buffers and handshake event until Disconnect completes.
    if (tcp_) {
        tcp_->Disconnect();
    }'''


def patch(path: Path) -> bool:
    source = path.read_text()
    if NEW in source:
        return False
    if source.count(OLD) != 1:
        raise RuntimeError('WebSocket lifetime patch needs review for this dependency version')
    path.write_text(source.replace(OLD, NEW, 1))
    return True


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--source', type=Path, required=True)
    args = parser.parse_args()
    print('WebSocket reader lifetime:', 'patched' if patch(args.source) else 'already patched')
