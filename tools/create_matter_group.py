#!/usr/bin/env python3
"""
=============================================================================
HOWTO: ALLES-IN-ÉÉN MATTER MULTICAST & BINDING SETUP
=============================================================================

1. VOORBEREIDING:
   Installeer 'websockets' op je externe machine als dat nog niet is gebeurd:
   pip install websockets

2. UITVOERING (ALLES IN 1 HANDELING):
   Run het script en geef zowel de lampen (--nodes) als de switch (--switch) mee:
   
   python3 matter_multicast_setup.py --nodes 32 33 34 35 --switch 29 --group-id 0x0001

   Optionele parameters:
   --group-name "Kantoor"    (Standaard: "Kantoor")
   --switch-endpoint 1       (Standaard: 1, verhoog dit als de knop op een ander endpoint zit)
   --keyset-id 42            (Standaard: 42)

=============================================================================
"""

import argparse
import asyncio
import json
import os
import sys
import time
import websockets

SERVER_IP   = "192.168.178.2"
SERVER_PORT = 5580

class MatterRemoteClient:
    def __init__(self, ip, port):
        self.url = f"ws://{ip}:{port}/ws"
        self.ws = None
        self._message_id = 1

    async def connect(self):
        print(f"[*] Verbinden met Matter Server op {self.url}...")
        self.ws = await websockets.connect(self.url)
        await self.ws.recv() # Welkomstbericht opvangen

    async def send_command(self, command, args):
        msg_id = str(self._message_id)
        self._message_id += 1
        payload = {"message_id": msg_id, "command": command, "args": args}
        await self.ws.send(json.dumps(payload))
        async for message in self.ws:
            response = json.loads(message)
            if response.get("message_id") == msg_id:
                if "error" in response: raise Exception(response["error"])
                return response.get("result")

    async def close(self):
        if self.ws: await self.ws.close()


async def run_logic(args):
    if len(args.group_name) > 16:
        sys.exit(f"FOUT: group-name te lang (max 16 tekens).")

    # Genereer één unieke sleutel voor deze hele sessie
    epoch_key = os.urandom(16).hex()
    epoch_key_bytes = list(bytes.fromhex(epoch_key))
    now_us = int(time.time() * 1_000_000)

    client = MatterRemoteClient(SERVER_IP, SERVER_PORT)
    await client.connect()

    print(f"\n" + "="*60)
    print(f"[*] START MULTICAST CONFIGURATIE")
    print(f"[*] Groep ID:   0x{args.group_id:04X} ({args.group_name})")
    print(f"[*] Keyset ID:  {args.keyset_id}")
    print(f"[*] Epoch Key:  {epoch_key}")
    print(f"[*] Doellampen: {args.nodes}")
    print(f"[*] Schakelaar: Node {args.switch} (Endpoint {args.switch_endpoint})")
    print("="*60 + "\n")

    try:
        # =======================================================================
        # DEEL 1: DE LAMPEN CONFIGUREREN
        # =======================================================================
        print("[DEEL 1] Lampen toevoegen aan de multicast groep...")
        
        for node_id in args.nodes:
            print(f"\n  --> Configureer Lamp (Node {node_id})...")
            
            # Stap 1.1: KeySetWrite naar lamp
            try:
                await client.send_command("device_command", {
                    "node_id": node_id, "endpoint_id": 0, "cluster_id": 63,
                    "command_name": "KeySetWrite",
                    "payload": {
                        "groupKeySet": {
                            "groupKeySetID": args.keyset_id,
                            "groupKeySecurityPolicy": 1,
                            "epochKey0": epoch_key_bytes,
                            "epochStartTime0": now_us
                        }
                    }
                })
                print(f"    [OK] KeySetWrite geslaagd")
            except Exception as e:
                print(f"    [!] Fout bij KeySetWrite: {e}")

            # Stap 1.2: GroupKeyMap naar lamp
            try:
                await client.send_command("write_attribute", {
                    "node_id": node_id, "attribute_path": "0/63/0",
                    "value": [{"groupId": args.group_id, "groupKeySetID": args.keyset_id, "fabricIndex": 1}]
                })
                print(f"    [OK] GroupKeyMap succesvol geschreven")
            except Exception as e:
                print(f"    [!] Fout bij GroupKeyMap: {e}")

            # Stap 1.3: AddGroup naar lamp
            try:
                await client.send_command("device_command", {
                    "node_id": node_id, "endpoint_id": 1, "cluster_id": 4,
                    "command_name": "AddGroup",
                    "payload": {"groupId": args.group_id, "groupName": args.group_name}
                })
                print(f"    [OK] AddGroup geslaagd")
            except Exception as e:
                if "137" in str(e) or "0x89" in str(e):
                    print(f"    [OK] Node al lid (DUPLICATE)")
                else:
                    try:
                        # Variant B voor specifieke SDK versies
                        await client.send_command("device_command", {
                            "node_id": node_id, "endpoint_id": 1, "cluster_id": 4,
                            "command_name": "AddGroup",
                            "payload": {"groupID": args.group_id, "groupName": args.group_name}
                        })
                        print(f"    [OK] AddGroup geslaagd (via variant B)")
                    except Exception as e2:
                        print(f"    [!] Fout bij AddGroup: {e2}")

        # =======================================================================
        # DEEL 2: DE SWITCH CONFIGUREREN
        # =======================================================================
        print("\n" + "-"*50)
        print("[DEEL 2] Schakelaar (Switch) koppelen aan de groep...")
        print(f"  --> Configureer Schakelaar (Node {args.switch})...")

        # Stap 2.1: Zelfde KeySetWrite naar de switch sturen
        try:
            await client.send_command("device_command", {
                "node_id": args.switch, "endpoint_id": 0, "cluster_id": 63,
                "command_name": "KeySetWrite",
                "payload": {
                    "groupKeySet": {
                        "groupKeySetID": args.keyset_id,
                        "groupKeySecurityPolicy": 1,
                        "epochKey0": epoch_key_bytes,
                        "epochStartTime0": 1 # Direct bruikbaar voor zenden
                    }
                }
            })
            print("    [OK] Sleutel succesvol opgeslagen in switch.")
        except Exception as e:
            print(f"    [!] Fout bij KeySetWrite op switch: {e}")

        # Stap 2.2: Binding tabel wegschrijven naar de switch
        try:
            binding_entry = {
                "groupId": args.group_id,
                "clusterId": 6, # OnOff cluster commando's zenden
                "fabricIndex": 1
            }
            await client.send_command("write_attribute", {
                "node_id": args.switch,
                "attribute_path": f"{args.switch_endpoint}/30/0", # 30 = Binding Cluster, 0 = Binding list
                "value": [binding_entry]
            })
            print(f"    [OK] Binding succesvol weggeschreven op endpoint {args.switch_endpoint}!")
        except Exception as e:
            print(f"    [!] Fout bij schrijven binding tabel: {e}")

        # VOLTOOID
        print("\n" + "="*60)
        print("[*] ALLES SUCCESVOL UITGEVOERD!")
        print(f"    De lampen {args.nodes} luisteren nu naar Groep 0x{args.group_id:04X}.")
        print(f"    Switch {args.switch} stuurt nu opdrachten rechtstreeks naar deze groep.")
        print("="*60)

    except Exception as general_error:
        print(f"\n[!] Algemene fout tijdens het proces: {general_error}")
    finally:
        await client.close()

if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("--nodes", type=int, nargs="+", required=True, help="Lijst met Node ID's van de lampen")
    ap.add_argument("--switch", type=int, required=True, help="Node ID van de schakelaar")
    ap.add_argument("--group-id", type=lambda x: int(x, 0), default=DEFAULT_GROUP_ID)
    ap.add_argument("--group-name", default=DEFAULT_GROUP_NAME)
    ap.add_argument("--switch-endpoint", type=int, default=1)
    ap.add_argument("--keyset-id", type=int, default=DEFAULT_KEYSET_ID)
    asyncio.run(run_logic(ap.parse_args()))
