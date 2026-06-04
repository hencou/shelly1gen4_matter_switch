#!/usr/bin/env python3
"""
Maak een Matter-groep aan voor meerdere lampen via python-matter-server.

De volgorde per node (verplicht door de Matter-spec):
  1. KeySetWrite  — schrijf een group keyset (cluster 0x3F, endpoint 0)
  2. GroupKeyMap  — koppel group-ID aan keyset (cluster 0x3F, endpoint 0,
                    via write_attribute met attribuut-pad "0/63/3")
  3. AddGroup     — registreer endpoint als groepslid (cluster 0x04, endpoint 1)

Serialisatie-regels python-matter-server (schema 11):
  - bytes-velden → base64-string in JSON (de server decodeert dit intern)
  - GroupKeyMap (attribuut 0/63/3) is een lijst van structs; de server
    verwacht als "value" een lijst van dicts met camelCase sleutels

Gebruik:
    python3 create_matter_group.py --dry-run
    python3 create_matter_group.py
    python3 create_matter_group.py --group-id 0x0001 --group-name "Kantoor"
    python3 create_matter_group.py --epoch-key fa34333653f7dd864bc05e49a71923a8

Vereisten:
    pip install websockets
"""

import argparse
import asyncio
import base64
import json
import os
import sys
import time

try:
    import websockets
except ImportError:
    sys.exit("Ontbrekende dependency. Installeer met: pip install websockets")

DEFAULT_NODES      = [32, 33, 34, 35]
DEFAULT_GROUP_ID   = 0x0001
DEFAULT_GROUP_NAME = "Kantoor"
DEFAULT_KEYSET_ID  = 42

CLUSTER_GROUP_KEY_MGMT = 63
CLUSTER_GROUPS         = 4


async def send_command(ws, command: str, args: dict, message_id: str) -> dict:
    payload = {"message_id": message_id, "command": command, "args": args}
    await ws.send(json.dumps(payload))
    while True:
        raw = await ws.recv()
        msg = json.loads(raw)
        if msg.get("message_id") == message_id:
            return msg


def check_error(resp: dict, context: str) -> bool:
    if "error_code" in resp or "error" in resp:
        print(f"  [!] Fout bij {context}: {resp}")
        return True
    return False


def hex_to_base64(hex_str: str) -> str:
    """Converteer hex-string naar base64 — python-matter-server verwacht
    bytes-velden als base64-string in het JSON-payload."""
    return base64.b64encode(bytes.fromhex(hex_str)).decode("ascii")


# ---------------------------------------------------------------------------
# Stap 1: KeySetWrite
# ---------------------------------------------------------------------------
async def keyset_write(ws, node_id: int, keyset_id: int, epoch_key_hex: str,
                       dry_run: bool) -> bool:
    # python-matter-server (schema 11) deserialiseert base64 → bytes intern
    epoch_key_b64 = hex_to_base64(epoch_key_hex)
    now_us = int(time.time() * 1_000_000)

    payload = {
        "groupKeySet": {
            "groupKeySetID":          keyset_id,
            "groupKeySecurityPolicy": 0,       # 0 = TrustFirst
            "epochKey0":              epoch_key_b64,
            "epochStartTime0":        now_us,
            "epochKey1":              None,
            "epochStartTime1":        None,
            "epochKey2":              None,
            "epochStartTime2":        None,
        }
    }

    if dry_run:
        print(f"  [dry-run] KeySetWrite keyset_id={keyset_id} "
              f"epochKey0=<base64, {len(epoch_key_hex)//2} bytes>")
        return True

    resp = await send_command(
        ws,
        "device_command",
        {
            "node_id":      node_id,
            "endpoint_id":  0,
            "cluster_id":   CLUSTER_GROUP_KEY_MGMT,
            "command_name": "KeySetWrite",
            "payload":      payload,
        },
        message_id=f"keyset-write-{node_id}",
    )

    if check_error(resp, "KeySetWrite"):
        return False
    print(f"  [OK] KeySetWrite geslaagd (keyset_id={keyset_id})")
    return True


# ---------------------------------------------------------------------------
# Stap 2: GroupKeyMap schrijven
# ---------------------------------------------------------------------------
async def write_group_key_map(ws, node_id: int, group_id: int,
                              keyset_id: int, dry_run: bool) -> bool:
    """
    Schrijft de GroupKeyMap via write_attribute.
    Pad: 0/63/3  (endpoint 0, cluster GroupKeyManagement=63, attribuut 3)
    
    De server verwacht de value als lijst van dicts. De server-side
    parse_value functie zet de camelCase dict om naar het juiste CHIP-type.
    """
    existing = []
    if not dry_run:
        resp = await send_command(
            ws, "read_attribute",
            {"node_id": node_id, "attribute_path": "0/63/3"},
            message_id=f"read-gkm-{node_id}",
        )
        result = resp.get("result")
        if isinstance(result, dict):
            val = next(iter(result.values()), None)
            existing = val if isinstance(val, list) else []
        elif isinstance(result, list):
            existing = result
        else:
            existing = []

        # Verwijder bestaande entry voor dezelfde group_id (dedup)
        existing = [
            e for e in existing
            if isinstance(e, dict) and
               e.get("groupId", e.get("groupID")) != group_id
        ]

    new_entry = {"groupId": group_id, "groupKeySetID": keyset_id}
    new_map   = existing + [new_entry]

    if dry_run:
        print(f"  [dry-run] GroupKeyMap schrijven: {new_entry}")
        return True

    resp = await send_command(
        ws,
        "write_attribute",
        {
            "node_id":        node_id,
            "attribute_path": "0/63/3",
            "value":          new_map,
        },
        message_id=f"write-gkm-{node_id}",
    )

    if check_error(resp, "GroupKeyMap write"):
        # Alternatief proberen: soms werkt een enkele entry zonder lijst-wrapper
        print(f"  [?] Poging 2: GroupKeyMap als enkelvoudige dict...")
        resp2 = await send_command(
            ws,
            "write_attribute",
            {
                "node_id":        node_id,
                "attribute_path": "0/63/3",
                "value":          new_entry,
            },
            message_id=f"write-gkm2-{node_id}",
        )
        if check_error(resp2, "GroupKeyMap write (poging 2)"):
            return False

    print(f"  [OK] GroupKeyMap geschreven (group_id=0x{group_id:04X} → keyset {keyset_id})")
    return True


# ---------------------------------------------------------------------------
# Stap 3: AddGroup
# ---------------------------------------------------------------------------
async def add_group(ws, node_id: int, group_id: int, group_name: str,
                    dry_run: bool) -> bool:
    payload = {"groupId": group_id, "groupName": group_name}

    if dry_run:
        print(f"  [dry-run] AddGroup {payload}")
        return True

    resp = await send_command(
        ws,
        "device_command",
        {
            "node_id":      node_id,
            "endpoint_id":  1,
            "cluster_id":   CLUSTER_GROUPS,
            "command_name": "AddGroup",
            "payload":      payload,
        },
        message_id=f"addgroup-{node_id}",
    )

    if check_error(resp, "AddGroup"):
        return False

    result = resp.get("result") or {}
    status = result.get("status", result.get("0"))

    if status in (0, None):
        print(f"  [OK] AddGroup geslaagd (group_id=0x{group_id:04X})")
        return True
    elif status == 137:
        print(f"  [OK] Node was al lid van de groep (DUPLICATE — normaal)")
        return True
    else:
        print(f"  [!] AddGroup status={status} (0x{status:02X}). Antwoord: {resp}")
        return False


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------
async def main() -> int:
    ap = argparse.ArgumentParser(
        description="Voeg Matter-lampen toe aan een groep.",
        formatter_class=argparse.ArgumentDefaultsHelpFormatter,
    )
    ap.add_argument("--ws",         default="ws://192.168.178.2:5580/ws")
    ap.add_argument("--nodes",      type=int, nargs="+", default=DEFAULT_NODES)
    ap.add_argument("--group-id",   type=lambda x: int(x, 0), default=DEFAULT_GROUP_ID,
                    help="groep-ID (hex of dec, bv. 0x0001)")
    ap.add_argument("--group-name", default=DEFAULT_GROUP_NAME,
                    help="naam (max 16 tekens)")
    ap.add_argument("--keyset-id",  type=int, default=DEFAULT_KEYSET_ID,
                    help="keyset-ID (1-65535)")
    ap.add_argument("--epoch-key",  default=None,
                    help="16-byte epoch key als 32 hex-tekens. Leeglaten = automatisch genereren.")
    ap.add_argument("--dry-run",    action="store_true")
    args = ap.parse_args()

    if len(args.group_name) > 16:
        sys.exit(f"FOUT: group-name mag maximaal 16 tekens zijn (nu {len(args.group_name)})")

    if args.epoch_key:
        epoch_key = args.epoch_key.lower().replace("0x", "").replace(" ", "")
        if len(epoch_key) != 32 or not all(c in "0123456789abcdef" for c in epoch_key):
            sys.exit("FOUT: epoch-key moet exact 32 hex-tekens zijn (16 bytes)")
    else:
        epoch_key = os.urandom(16).hex()
        print(f"[*] Automatisch gegenereerde epoch key: {epoch_key}")
        print(f"    Bewaar deze als je de drukknop aan dezelfde groep wilt toevoegen.\n")

    print(f"[*] Verbinding maken met {args.ws} ...")
    try:
        ws = await websockets.connect(args.ws, max_size=2**22)
    except Exception as exc:
        sys.exit(f"FOUT: kan niet verbinden: {exc}")

    async with ws:
        try:
            info = json.loads(await asyncio.wait_for(ws.recv(), timeout=5))
            print(f"[*] matter-server: schema={info.get('schema_version')} "
                  f"sdk={info.get('sdk_version')} "
                  f"fabric_id={info.get('fabric_id')}")
        except asyncio.TimeoutError:
            print("[!] Geen server-info pakket binnen 5s — ga door")

        print(f"\n[*] Groep: ID=0x{args.group_id:04X} ({args.group_id}), "
              f"naam='{args.group_name}', keyset_id={args.keyset_id}")
        print(f"[*] Epoch key (base64): {hex_to_base64(epoch_key)}")
        print(f"[*] Nodes: {args.nodes}")
        if args.dry_run:
            print("[*] DRY-RUN — niets wordt geschreven\n")

        results = {}
        for node_id in args.nodes:
            print(f"\n--- Node {node_id} ---")
            ok  = await keyset_write(ws, node_id, args.keyset_id, epoch_key, args.dry_run)
            ok &= await write_group_key_map(ws, node_id, args.group_id, args.keyset_id, args.dry_run)
            ok &= await add_group(ws, node_id, args.group_id, args.group_name, args.dry_run)
            results[node_id] = ok

        print("\n" + "=" * 50)
        print("Samenvatting:")
        all_ok = True
        for node_id, ok in results.items():
            print(f"  Node {node_id}: {'OK' if ok else 'MISLUKT'}")
            if not ok:
                all_ok = False

        if all_ok and not args.dry_run:
            print(f"\n[*] Klaar. Alle nodes zijn lid van groep 0x{args.group_id:04X}.")
            print(f"\n--- Waarden voor het binding-script (drukknop) ---")
            print(f"  --group-id  0x{args.group_id:04X}")
            print(f"  --keyset-id {args.keyset_id}")
            print(f"  --epoch-key {epoch_key}")

        return 0 if all_ok else 1


if __name__ == "__main__":
    sys.exit(asyncio.run(main()) or 0)
