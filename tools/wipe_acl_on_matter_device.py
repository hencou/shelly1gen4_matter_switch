#!/usr/bin/env python3
"""
Wipe stale ACL entries AND the binding table from a Matter node
(e.g. IKEA Kajplats lamp).

ACL part:
Keeps only admin entries (privilege=Administer + targets=null) — those
are the real controllers such as Home Assistant. All binding-targets
entries (entries with non-empty targets, e.g. {cluster:6, endpoint:1})
and stray/incomplete fabric entries are dropped.

Binding part:
The binding table (cluster 0x001E / 30, attribute Binding 0x0000) is
fully cleared. These are separate device-to-device links (e.g. a switch
bound to this lamp) and have nothing to do with controller access, so
this is safe to clean independently of the ACL cleanup.

Example usage:
    python3 wipe_acl.py                          # connect via default URL
    python3 wipe_acl.py --ws ws://192.168.1.10:5580/ws --node 3
    python3 wipe_acl.py --dry-run                # only show, don't write

Dependencies:
    pip install websockets

Hostname / port:
- Home Assistant OS / Supervised with Matter Server add-on:
      ws://192.168.178.2:5580/ws       (default)
   or ws://<HA-IP>:5580/ws
- Python-matter-server standalone:
      ws://<host>:5580/ws
"""

import argparse
import asyncio
import json
import sys
from typing import Any

try:
    import websockets
except ImportError:
    sys.exit("Missing 'websockets'. Install with: pip install websockets")


# ACL attribute path: endpoint 0, cluster 0x1F (AccessControl), attribute 0 (acl)
ACL_PATH = "0/31/0"

# Binding attribute path template: <endpoint>, cluster 0x1E (Binding, decimal 30),
# attribute 0 (binding). The endpoint differs per device — for most
# lamps/switches this is endpoint 1.
BINDING_PATH_TMPL = "{endpoint}/30/0"


async def send_command(
    ws: "websockets.WebSocketClientProtocol",
    command: str,
    args: dict,
    message_id: str = "msg-1",
) -> dict:
    """Send a JSON command to matter-server and wait for matching response."""
    payload = {
        "message_id": message_id,
        "command": command,
        "args": args,
    }
    await ws.send(json.dumps(payload))
    # Keep reading messages until we see the matching response (ignore event broadcasts).
    while True:
        raw = await ws.recv()
        msg = json.loads(raw)
        if msg.get("message_id") == message_id:
            return msg


def _field(entry: dict, named: str, tlv_key: str) -> Any:
    """Read a field from an ACL entry — accepts both named (privilege)
    and integer-TLV (1) keys, depending on how matter-server serializes
    it in this version."""
    if named in entry:
        return entry[named]
    return entry.get(tlv_key)


def filter_admin_entries(acl: list) -> tuple[list, list]:
    """Split ACL into (keep, drop). Keep only entries with
    privilege==Administer (5) and targets==None — those are the real
    controllers. All other entries (binding-targets, partial fabric
    entries) are dropped."""
    keep, drop = [], []
    for e in acl:
        priv    = _field(e, "privilege", "1")
        targets = _field(e, "targets",   "4")
        if priv == 5 and targets in (None, []):
            keep.append(e)
        else:
            drop.append(e)
    return keep, drop


def _normalize_result(resp: dict, path: str) -> list:
    """matter-server can return the result as a list OR as a dict
    ({attribute_path: value}), depending on the version. Normalize to a list."""
    result = resp.get("result")
    if isinstance(result, dict):
        if path in result:
            return result[path] or []
        return next(iter(result.values()), []) or []
    if isinstance(result, list):
        return result
    sys.exit(f"ERROR: unexpected result format: {result!r}")


async def discover_binding_endpoints(ws, node_id: int) -> list[int]:
    """Find which endpoint(s) actually expose the Binding cluster (0x1E / 30)
    on this node. Not every device has a Binding cluster, and if it does,
    it isn't always on endpoint 1 — so ask the server instead of guessing.
    Uses the 'get_node' command, which returns the full cached node data
    including an 'attributes' dict keyed by 'endpoint/cluster/attribute'."""
    resp = await send_command(ws, "get_node", {
        "node_id": node_id,
    }, message_id="get-node")

    if "error_code" in resp or "error" in resp:
        sys.exit(f"ERROR during get_node: {resp}")

    node_data = resp.get("result", {})
    attributes = node_data.get("attributes", {})

    endpoints = set()
    for key in attributes:
        parts = key.split("/")
        if len(parts) == 3 and parts[1] == "30":
            endpoints.add(int(parts[0]))

    return sorted(endpoints)


async def main() -> int:
    ap = argparse.ArgumentParser(
        description="Wipe stale ACL entries from a Matter node.",
        formatter_class=argparse.ArgumentDefaultsHelpFormatter,
    )
    ap.add_argument("--ws",   default="ws://192.168.178.2:5580/ws",
                    help="matter-server WebSocket URL")
    ap.add_argument("--node", type=int, default=3,
                    help="lamp node id")
    ap.add_argument("--endpoint", type=int, default=None,
                    help="endpoint that holds the binding table. "
                         "If omitted, it is auto-detected via get_node.")
    ap.add_argument("--dry-run", action="store_true",
                    help="only show what would be written, change nothing")
    args = ap.parse_args()

    print(f"[*] Connecting to {args.ws} ...")
    try:
        ws = await websockets.connect(args.ws, max_size=2**22)
    except Exception as exc:
        sys.exit(f"ERROR: could not connect: {exc}")

    async with ws:
        # Server-info packet that matter-server sends right after connecting.
        try:
            info = json.loads(await asyncio.wait_for(ws.recv(), timeout=5))
            print(f"[*] matter-server: schema={info.get('schema_version')} "
                  f"sdk={info.get('sdk_version')} "
                  f"fabric_id={info.get('fabric_id')}")
        except asyncio.TimeoutError:
            print("[!] No server-info packet within 5s — continuing anyway")

        # ----- 1) Read current ACL ---------------------------------------
        print(f"\n[*] Reading ACL of node {args.node} (path {ACL_PATH}) ...")
        resp = await send_command(ws, "read_attribute", {
            "node_id": args.node,
            "attribute_path": ACL_PATH,
        }, message_id="read-acl")

        if "error_code" in resp or "error" in resp:
            sys.exit(f"ERROR during read_attribute: {resp}")

        current = _normalize_result(resp, ACL_PATH)

        print(f"[*] Current ACL ({len(current)} entries):")
        for i, e in enumerate(current):
            print(f"      [{i}] {e}")

        # ----- 2) Filter --------------------------------------------------
        keep, drop = filter_admin_entries(current)

        print(f"\n[*] Keeping: {len(keep)} admin entries")
        for e in keep:
            print(f"      KEEP  {e}")
        print(f"[*] Removing: {len(drop)} entries (binding-targets / stray fabric)")
        for e in drop:
            print(f"      DROP  {e}")

        if not keep:
            sys.exit("ERROR: no admin entry found. ABORT — otherwise you lose HA access!")

        if drop == []:
            print("\n[*] ACL: nothing to do — already clean.")
        elif args.dry_run:
            print("\n[*] --dry-run: nothing written to ACL.")
        else:
            # ----- 3) Write new ACL -----------------------------------
            print(f"\n[*] Writing new ACL ({len(keep)} entries) ...")
            resp = await send_command(ws, "write_attribute", {
                "node_id": args.node,
                "attribute_path": ACL_PATH,
                "value": keep,
            }, message_id="write-acl")

            if "error_code" in resp or "error" in resp:
                sys.exit(f"ERROR during write_attribute (acl): {resp}")

            print(f"[*] OK — response: {resp.get('result', resp)}")

        # ----- 4) Find endpoint(s) with a Binding cluster ---------------
        if args.endpoint is not None:
            binding_endpoints = [args.endpoint]
        else:
            print(f"\n[*] Auto-detecting Binding cluster endpoint(s) on node {args.node} ...")
            binding_endpoints = await discover_binding_endpoints(ws, args.node)

        if not binding_endpoints:
            print("[*] No Binding cluster found on this node — nothing to clean up.")
        else:
            print(f"[*] Binding cluster found on endpoint(s): {binding_endpoints}")

        # ----- 5) Read and clear the binding table on each endpoint -----
        for endpoint in binding_endpoints:
            binding_path = BINDING_PATH_TMPL.format(endpoint=endpoint)
            print(f"\n[*] Reading binding table of node {args.node} "
                  f"endpoint {endpoint} (path {binding_path}) ...")
            resp = await send_command(ws, "read_attribute", {
                "node_id": args.node,
                "attribute_path": binding_path,
            }, message_id=f"read-binding-{endpoint}")

            if "error_code" in resp or "error" in resp:
                sys.exit(f"ERROR during read_attribute (binding, endpoint {endpoint}): {resp}")

            current_bindings = _normalize_result(resp, binding_path)

            print(f"[*] Current binding table ({len(current_bindings)} entries):")
            for i, e in enumerate(current_bindings):
                print(f"      [{i}] {e}")

            if not current_bindings:
                print("[*] Binding table: nothing to do — already empty.")
                continue

            if args.dry_run:
                print("[*] --dry-run: nothing written to binding table.")
                continue

            print("[*] Clearing binding table (writing empty list) ...")
            resp = await send_command(ws, "write_attribute", {
                "node_id": args.node,
                "attribute_path": binding_path,
                "value": [],
            }, message_id=f"write-binding-{endpoint}")

            if "error_code" in resp or "error" in resp:
                sys.exit(f"ERROR during write_attribute (binding, endpoint {endpoint}): {resp}")

            print(f"[*] OK — response: {resp.get('result', resp)}")

        print("\n[*] Done. Try setting up the binding in HA again.")
        return 0


if __name__ == "__main__":
    sys.exit(asyncio.run(main()) or 0)
