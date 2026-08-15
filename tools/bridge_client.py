#!/usr/bin/env python3
"""Authenticated Flipforge Bridge protocol/measurement client."""

from __future__ import annotations

import argparse
import getpass
import hashlib
import hmac
import json
import os
import secrets
import socket
import struct
import time
from pathlib import Path

MAGIC = b"FBRG"
VERSION = 1
PORT = 4242
HEADER = struct.Struct("<4sBBBBIH")
HELLO = 1
AUTHENTICATE = 2
GET_INFO = 3
GET_STATUS = 4
BEGIN_PROXY = 5
PING = 7
OK = 0
AUTH_CONTEXT = b"Flipforge Bridge Auth v1"


def receive_exact(connection: socket.socket, size: int) -> bytes:
    output = bytearray()
    while len(output) < size:
        chunk = connection.recv(size - len(output))
        if not chunk:
            raise ConnectionError("Bridge closed the connection")
        output.extend(chunk)
    return bytes(output)


def send_message(connection: socket.socket, command: int, request_id: int, payload: bytes = b"") -> None:
    if len(payload) > 512:
        raise ValueError("Management payload exceeds 512 bytes")
    connection.sendall(HEADER.pack(MAGIC, VERSION, command, 0, 0, request_id, len(payload)) + payload)


def receive_message(connection: socket.socket, expected_command: int, expected_id: int) -> bytes:
    magic, version, command, flags, status, request_id, size = HEADER.unpack(
        receive_exact(connection, HEADER.size)
    )
    if magic != MAGIC or version != VERSION or command != expected_command or not (flags & 1):
        raise RuntimeError("Invalid Bridge response header")
    payload = receive_exact(connection, size)
    if request_id != expected_id:
        raise RuntimeError("Mismatched Bridge request ID")
    if status != OK:
        raise RuntimeError(f"Bridge returned status {status}")
    return payload


def authenticate(connection: socket.socket, pairing_secret: bytes) -> None:
    client_nonce = secrets.token_bytes(32)
    send_message(connection, HELLO, 1, client_nonce)
    challenge = receive_message(connection, HELLO, 1)
    if len(challenge) != 40:
        raise RuntimeError("Invalid Bridge challenge length")
    material = AUTH_CONTEXT + bytes([VERSION]) + client_nonce + challenge
    proof = hmac.new(pairing_secret, material, hashlib.sha256).digest()
    send_message(connection, AUTHENTICATE, 2, proof)
    receive_message(connection, AUTHENTICATE, 2)


def connect(args: argparse.Namespace) -> socket.socket:
    secret_text = args.secret or os.environ.get("FLIPFORGE_PAIRING_SECRET")
    if not secret_text:
        secret_text = getpass.getpass("Pairing secret (hex): ")
    try:
        secret = bytes.fromhex(secret_text)
    except ValueError as error:
        raise SystemExit("--secret must be hexadecimal") from error
    if len(secret) != 32:
        raise SystemExit("--secret must decode to exactly 32 bytes")
    connection = socket.create_connection((args.host, args.port), timeout=args.timeout)
    connection.settimeout(args.timeout)
    authenticate(connection, secret)
    return connection


def request_json(args: argparse.Namespace, command: int) -> None:
    with connect(args) as connection:
        send_message(connection, command, 3)
        payload = receive_message(connection, command, 3)
    print(json.dumps(json.loads(payload), indent=2, sort_keys=True))


def run_rpc(args: argparse.Namespace) -> None:
    request = args.input.read_bytes()
    with connect(args) as connection:
        send_message(connection, BEGIN_PROXY, 3)
        receive_message(connection, BEGIN_PROXY, 3)
        started = time.monotonic()
        connection.sendall(request)
        response = bytearray()
        connection.settimeout(args.response_seconds)
        while True:
            try:
                chunk = connection.recv(64 * 1024)
            except TimeoutError:
                break
            if not chunk:
                break
            response.extend(chunk)
            if args.expected_response_bytes is not None and len(response) >= args.expected_response_bytes:
                break
        duration = time.monotonic() - started
    if args.expected_response_bytes is not None and len(response) != args.expected_response_bytes:
        raise RuntimeError(
            f"Expected {args.expected_response_bytes} response bytes, received {len(response)}"
        )
    args.output.write_bytes(response)
    total = len(request) + len(response)
    rate = total / duration / 1024 if duration > 0 else 0.0
    print(
        json.dumps(
            {
                "request_bytes": len(request),
                "response_bytes": len(response),
                "duration_seconds": round(duration, 3),
                "effective_kib_per_second": round(rate, 2),
                "response_target_verified": args.expected_response_bytes is not None,
                "output": str(args.output),
            },
            indent=2,
        )
    )


def parser() -> argparse.ArgumentParser:
    result = argparse.ArgumentParser(description=__doc__)
    result.add_argument("--host", default="192.168.4.1")
    result.add_argument("--port", type=int, default=PORT)
    result.add_argument(
        "--secret",
        help="64-character pairing secret hex; prefer FLIPFORGE_PAIRING_SECRET or prompt",
    )
    result.add_argument("--timeout", type=float, default=5.0)
    subcommands = result.add_subparsers(dest="command", required=True)
    info = subcommands.add_parser("info")
    info.set_defaults(handler=lambda args: request_json(args, GET_INFO))
    status = subcommands.add_parser("status")
    status.set_defaults(handler=lambda args: request_json(args, GET_STATUS))
    rpc = subcommands.add_parser("rpc")
    rpc.add_argument("--input", type=Path, required=True, help="Valid protobuf RPC byte stream")
    rpc.add_argument("--output", type=Path, required=True)
    rpc.add_argument("--response-seconds", type=float, default=5.0)
    rpc.add_argument(
        "--expected-response-bytes",
        type=int,
        help="Stop on this exact response size for a bounded throughput measurement",
    )
    rpc.set_defaults(handler=run_rpc)
    return result


def main() -> None:
    args = parser().parse_args()
    args.handler(args)


if __name__ == "__main__":
    main()
