#!/usr/bin/env python3
"""
Wipe stale ACL entries from a Matter node (e.g. IKEA Kajplats lamp).
 
Bewaart alleen admin-entries (privilege=Administer + targets=null) — dat
zijn de echte controllers zoals Home Assistant. Alle binding-targets
(entries met targets niet leeg, bv. {cluster:6, endpoint:1}) en lege
fabric-entries worden weggegooid.
 
Voorbeeldgebruik:
    python3 wipe_acl.py                          # connect via default URL
    python3 wipe_acl.py --ws ws://192.168.1.10:5580/ws --node 3
    python3 wipe_acl.py --dry-run                # alleen tonen, niet schrijven
 
Dependencies:
    pip install websockets
 
Hostname / poort:
- Home Assistant OS / Supervised met Matter Server add-on:
      ws://192.168.178.2:5580/ws       (standaard)
   of ws://<HA-IP>:5580/ws
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
    # Lees berichten tot we de matching response zien (negeer event-broadcasts).
    while True:
        raw = await ws.recv()
        msg = json.loads(raw)
        if msg.get("message_id") == message_id:
            return msg
 
 
def _field(entry: dict, named: str, tlv_key: str) -> Any:
    """Lees een veld uit een ACL-entry — accepteert zowel named (privilege)
    als integer-TLV (1) sleutels, afhankelijk van hoe matter-server het
    serialiseert in deze versie."""
    if named in entry:
        return entry[named]
    return entry.get(tlv_key)
 
 
def filter_admin_entries(acl: list) -> tuple[list, list]:
    """Splits ACL in (keep, drop). Behoud alleen entries met
    privilege==Administer (5) en targets==None — dat zijn de echte
    controllers. Alle andere entries (binding-targets, halve fabric-
    entries) gaan weg."""
    keep, drop = [], []
    for e in acl:
        priv    = _field(e, "privilege", "1")
        targets = _field(e, "targets",   "4")
        if priv == 5 and targets in (None, []):
            keep.append(e)
        else:
            drop.append(e)
    return keep, drop
 
 
async def main() -> int:
    ap = argparse.ArgumentParser(
        description="Wipe stale ACL entries from a Matter node.",
        formatter_class=argparse.ArgumentDefaultsHelpFormatter,
    )
    ap.add_argument("--ws",   default="ws://192.168.178.2:5580/ws",
                    help="matter-server WebSocket URL")
    ap.add_argument("--node", type=int, default=3,
                    help="lamp node id")
    ap.add_argument("--dry-run", action="store_true",
                    help="alleen tonen wat er zou worden geschreven, niets veranderen")
    args = ap.parse_args()
 
    print(f"[*] Connecting to {args.ws} ...")
    try:
        ws = await websockets.connect(args.ws, max_size=2**22)
    except Exception as exc:
        sys.exit(f"FOUT: kon niet verbinden: {exc}")
 
    async with ws:
        # Server-info pakket dat matter-server direct na connect verstuurt.
        try:
            info = json.loads(await asyncio.wait_for(ws.recv(), timeout=5))
            print(f"[*] matter-server: schema={info.get('schema_version')} "
                  f"sdk={info.get('sdk_version')} "
                  f"fabric_id={info.get('fabric_id')}")
        except asyncio.TimeoutError:
            print("[!] Geen server-info pakket binnen 5s — ga toch door")
 
        # ----- 1) Read huidige ACL ---------------------------------------
        print(f"\n[*] Lezen ACL van node {args.node} (path {ACL_PATH}) ...")
        resp = await send_command(ws, "read_attribute", {
            "node_id": args.node,
            "attribute_path": ACL_PATH,
        }, message_id="read-acl")
 
        if "error_code" in resp or "error" in resp:
            sys.exit(f"FOUT bij read_attribute: {resp}")
 
        # matter-server kan resultaat als list óf als dict ({attribute_path: value})
        # teruggeven, afhankelijk van versie. Normaliseer naar list.
        result = resp.get("result")
        if isinstance(result, dict):
            # bv. {"0/31/0": [...]}
            current = next(iter(result.values()), [])
        elif isinstance(result, list):
            current = result
        else:
            sys.exit(f"FOUT: onverwacht result-formaat: {result!r}")
 
        print(f"[*] Huidige ACL ({len(current)} entries):")
        for i, e in enumerate(current):
            print(f"      [{i}] {e}")
 
        # ----- 2) Filter --------------------------------------------------
        keep, drop = filter_admin_entries(current)
 
        print(f"\n[*] Behouden: {len(keep)} admin-entries")
        for e in keep:
            print(f"      KEEP  {e}")
        print(f"[*] Verwijderen: {len(drop)} entries (binding-targets / lege fabric)")
        for e in drop:
            print(f"      DROP  {e}")
 
        if not keep:
            sys.exit("FOUT: geen admin-entry gevonden. ABORT — anders verlies je HA-toegang!")
 
        if drop == []:
            print("\n[*] Niets te doen — ACL is al schoon.")
            return 0
 
        if args.dry_run:
            print("\n[*] --dry-run: niets geschreven.")
            return 0
 
        # ----- 3) Write nieuwe ACL ---------------------------------------
        print(f"\n[*] Schrijven nieuwe ACL ({len(keep)} entries) ...")
        resp = await send_command(ws, "write_attribute", {
            "node_id": args.node,
            "attribute_path": ACL_PATH,
            "value": keep,
        }, message_id="write-acl")
 
        if "error_code" in resp or "error" in resp:
            sys.exit(f"FOUT bij write_attribute: {resp}")
 
        print(f"[*] OK — response: {resp.get('result', resp)}")
        print("\n[*] Klaar. Probeer nu opnieuw de binding aan te maken in HA.")
        return 0
 
 
if __name__ == "__main__":
    sys.exit(asyncio.run(main()) or 0)