#!/usr/bin/env python3
"""
=============================================================================
HOWTO: DIT SCRIPT EXTERN UITVOEREN VANAF EEN ANDERE MACHINE
=============================================================================

1. VOORBEREIDING OP DE EXTERNE MACHINE:
   Zorg ervoor dat Python 3 op de externe machine is geïnstalleerd.
   Installeer de benodigde 'websockets' bibliotheek via de terminal/opdrachtprompt:
   
   pip install websockets

2. CONTROLEER NETWERK & FIREWALL:
   - Dit script stuurt opdrachten naar de Matter Server op IP: 192.168.178.2
   - Zorg ervoor dat poort 5580 op de doelmachine open staat en bereikbaar is
     binnen je lokale netwerk (LAN).

3. HET SCRIPT OPSLAAN EN UITVOEREN:
   Sla deze code op als 'remote_groups.py' op de externe machine.
   
   Voer het script uit via de terminal:
   python3 remote_groups.py --nodes 32 33 34 35 --group-id 0x0001

   Optionele parameters die je kunt meegeven:
   --group-name "MijnGroep"  (Standaard: "Kantoor")
   --keyset-id 42            (Standaard: 42)
   --epoch-key "HEX_STRING"  (Als je een specifieke 16-byte sleutel wilt forceren)

=============================================================================
"""

import argparse
import asyncio
import json
import os
import sys
import time
import websockets

DEFAULT_NODES      = [32, 33, 34, 35]
DEFAULT_GROUP_ID   = 0x0001
DEFAULT_GROUP_NAME = "Kantoor"
DEFAULT_KEYSET_ID  = 42
SERVER_IP          = "192.168.178.2"
SERVER_PORT        = 5580

class MatterRemoteClient:
    def __init__(self, ip, port):
        self.url = f"ws://{ip}:{port}/ws"
        self.ws = None
        self._message_id = 1

    async def connect(self):
        print(f"[*] Verbinden met Matter Server op {self.url}...")
        self.ws = await websockets.connect(self.url)
        # Ontvang het initiële 'hello' of 'server_info' bericht van de server
        await self.ws.recv()

    async def send_command(self, command, args):
        """Stuurt een JSON-RPC commando naar de Matter Server API"""
        msg_id = str(self._message_id)
        self._message_id += 1
        
        payload = {
            "message_id": msg_id,
            "command": command,
            "args": args
        }
        
        await self.ws.send(json.dumps(payload))
        
        # Wacht op het antwoord met het juiste message_id
        async for message in self.ws:
            response = json.loads(message)
            if response.get("message_id") == msg_id:
                if "error" in response:
                    raise Exception(response["error"])
                return response.get("result")

    async def close(self):
        if self.ws:
            await self.ws.close()

async def run_logic(args):
    if len(args.group_name) > 16:
        sys.exit(f"FOUT: group-name mag maximaal 16 tekens zijn.")

    if args.epoch_key:
        epoch_key = args.epoch_key.lower().replace("0x", "").replace(" ", "")
    else:
        epoch_key = os.urandom(16).hex()
        print(f"[*] Automatisch gegenereerde epoch key: {epoch_key}")

    client = MatterRemoteClient(SERVER_IP, SERVER_PORT)
    await client.connect()

    print(f"\n[*] Groep: ID=0x{args.group_id:04X}, naam='{args.group_name}', keyset_id={args.keyset_id}")
    print(f"[*] Nodes: {args.nodes}\n")

    results = {}
    try:
        for node_id in args.nodes:
            print(f"--- Node {node_id} ---")
            epoch_key_bytes = list(bytes.fromhex(epoch_key)) # API verwacht een array van ints voor bytes
            now_us = int(time.time() * 1_000_000)

            # -----------------------------------------------------------------------
            # Stap 1: KeySetWrite
            # -----------------------------------------------------------------------
            try:
                await client.send_command("device_command", {
                    "node_id": node_id,
                    "endpoint_id": 0,
                    "cluster_id": 63, # GroupKeyManagement Cluster ID
                    "command_name": "KeySetWrite",
                    "payload": {
                        "groupKeySet": {
                            "groupKeySetID": args.keyset_id,
                            "groupKeySecurityPolicy": 1, # TrustFirst
                            "epochKey0": epoch_key_bytes,
                            "epochStartTime0": now_us
                        }
                    }
                })
                print(f"  [OK] KeySetWrite geslaagd")
                ok1 = True
            except Exception as e:
                print(f"  [!] Fout bij KeySetWrite: {e}")
                ok1 = False

            # -----------------------------------------------------------------------
            # Stap 2: GroupKeyMap
            # -----------------------------------------------------------------------
            try:
                await client.send_command("write_attribute", {
                    "node_id": node_id,
                    "attribute_path": f"0/63/0", # 63 = GroupKeyManagement, 0 = GroupKeyMap attribute
                    "value": [{
                        "groupId": args.group_id,
                        "groupKeySetID": args.keyset_id,
                        "fabricIndex": 1
                    }]
                })
                print(f"  [OK] GroupKeyMap succesvol geschreven")
                ok2 = True
            except Exception as e:
                print(f"  [!] Fout bij GroupKeyMap: {e}")
                ok2 = False

            # -----------------------------------------------------------------------
            # Stap 3: AddGroup
            # -----------------------------------------------------------------------
            try:
                await client.send_command("device_command", {
                    "node_id": node_id,
                    "endpoint_id": 1,
                    "cluster_id": 4, # Groups Cluster ID
                    "command_name": "AddGroup",
                    "payload": {
                        "groupId": args.group_id,
                        "groupName": args.group_name
                    }
                })
                print(f"  [OK] AddGroup geslaagd")
                ok3 = True
            except Exception as e:
                if "137" in str(e) or "0x89" in str(e):
                    print(f"  [OK] Node al lid (DUPLICATE)")
                    ok3 = True
                else:
                    try:
                        await client.send_command("device_command", {
                            "node_id": node_id,
                            "endpoint_id": 1,
                            "cluster_id": 4,
                            "command_name": "AddGroup",
                            "payload": {
                                "groupID": args.group_id,
                                "groupName": args.group_name
                            }
                        })
                        print(f"  [OK] AddGroup geslaagd (via variant B)")
                        ok3 = True
                    except Exception as e2:
                        print(f"  [!] Fout bij AddGroup: {e2}")
                        ok3 = False

            results[node_id] = (ok1 and ok2 and ok3)
            print()

    finally:
        await client.close()

    print("==================================================")
    print("Samenvatting:")
    all_ok = True
    for node_id, ok in results.items():
        print(f"  Node {node_id}: {'OK' if ok else 'MISLUKT'}")
        if not ok: all_ok = False

    if all_ok:
        print(f"\n[*] Klaar! Alle nodes zijn extern gekoppeld.")
        print(f"  --group-id  0x{args.group_id:04X}")
        print(f"  --keyset-id {args.keyset_id}")
        print(f"  --epoch-key {epoch_key}")

if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("--nodes", type=int, nargs="+", default=DEFAULT_NODES)
    ap.add_argument("--group-id", type=lambda x: int(x, 0), default=DEFAULT_GROUP_ID)
    ap.add_argument("--group-name", default=DEFAULT_GROUP_NAME)
    ap.add_argument("--keyset-id", type=int, default=DEFAULT_KEYSET_ID)
    ap.add_argument("--epoch-key", default=None)
    asyncio.run(run_logic(ap.parse_args()))
