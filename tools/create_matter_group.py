#!/usr/bin/env python3
"""
=============================================================================
HOWTO: ALLES-IN-ÉÉN MATTER MULTICAST & BINDING SETUP (CUSTOM FIRMWARE)
=============================================================================

1. VOORBEREIDING:
   Installeer 'websockets' op je externe machine als dat nog niet is gebeurd:
   pip install websockets

2. UITVOERING:
   Run het script vanaf je laptop en geef zowel de lampen (--nodes) als de 
   schakelaar (--switch) mee. De multicast-groep wordt automatisch aangemaakt
   en de switch krijgt direct de juiste groeps- en ACL-rechten op Endpoint 2.
   
   python3 tools/create_matter_group.py --nodes 32 33 34 35 --switch 29 --group-id 0x0001

3. PARAMETERS:
   --nodes            Lijst met Node ID's van de doellampen (bijv. 32 33 34 35)
   --switch           Node ID van de Shelly schakelaar (bijv. 29)
   --group-id         De multicast Group ID (Standaard: 0x0001)
   --switch-endpoint  Het endpoint van de knop op de switch (Standaard: 2 voor custom firmware)
   --group-name       Naam van de groep (Standaard: "Kantoor")
   --keyset-id        Interne sleutelset ID (Standaard: 42)

=============================================================================
"""

import argparse
import asyncio
import json
import os
import sys
import time
import websockets

DEFAULT_GROUP_ID   = 0x0001
DEFAULT_GROUP_NAME = "Kantoor"
DEFAULT_KEYSET_ID  = 42

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
        await self.ws.recv()

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

    epoch_key = os.urandom(16).hex()
    epoch_key_bytes = list(bytes.fromhex(epoch_key))
    now_us = int(time.time() * 1_000_000)

    client = MatterRemoteClient(SERVER_IP, SERVER_PORT)
    await client.connect()

    print(f"\n" + "="*60)
    print(f"[*] START MULTICAST CONFIGURATIE (CUSTOM FIRMWARE MODE)")
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

            try:
                await client.send_command("write_attribute", {
                    "node_id": node_id, "attribute_path": "0/63/0",
                    "value": [{"groupId": args.group_id, "groupKeySetID": args.keyset_id, "fabricIndex": 1}]
                })
                print(f"    [OK] GroupKeyMap succesvol geschreven")
            except Exception as e:
                print(f"    [!] Fout bij GroupKeyMap: {e}")

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

        # 2.1 KeySetWrite naar de switch
        try:
            await client.send_command("device_command", {
                "node_id": args.switch, "endpoint_id": 0, "cluster_id": 63,
                "command_name": "KeySetWrite",
                "payload": {
                    "groupKeySet": {
                        "groupKeySetID": args.keyset_id,
                        "groupKeySecurityPolicy": 1,
                        "epochKey0": epoch_key_bytes,
                        "epochStartTime0": 1
                    }
                }
            })
            print("    [OK] Sleutel succesvol opgeslagen in switch.")
        except Exception as e:
            print(f"    [!] Fout bij KeySetWrite op switch: {e}")

        # 2.2 Binding tabel wegschrijven
        try:
            binding_entry = {
                "groupId": args.group_id,
                "clusterId": 6,
                "fabricIndex": 1
            }
            await client.send_command("write_attribute", {
                "node_id": args.switch,
                "attribute_path": f"{args.switch_endpoint}/30/0",
                "value": [binding_entry]
            })
            print(f"    [OK] Binding succesvol weggeschreven op endpoint {args.switch_endpoint}!")
        except Exception as e:
            print(f"    [!] Fout bij schrijven binding tabel: {e}")

        # 2.3 ACL Rechten aanpassen (Veilige verwerking van dict/list returns)
        try:
            print("    [*] ACL-rechten ophalen en bijwerken...")
            response_data = await client.send_command("read_attribute", {
                "node_id": args.switch,
                "attribute_path": "0/31/0"
            })
            
            if isinstance(response_data, dict):
                acl_list = response_data.get("value", response_data.get("acl", []))
            elif isinstance(response_data, list):
                acl_list = response_data
            else:
                acl_list = []

            # Maak de nieuwe regel aan voor de multicast groep
            group_acl_entry = {
                "privilege": 3, # Manage privileges
                "authMode": 3,  # Group auth mode
                "subjects": [args.group_id],
                "targets": None,
                "fabricIndex": 1
            }
            
            acl_list.append(group_acl_entry)
            
            await client.send_command("write_attribute", {
                "node_id": args.switch,
                "attribute_path": "0/31/0",
                "value": acl_list
            })
            print("    [OK] ACL rechten succesvol geüpdatet (Groepsautorisatie toegevoegd).")
        except Exception as e:
            print(f"    [!] Fout bij configureren ACL rechten: {e}")

        print("\n" + "="*60)
        print("[*] ALLES SUCCESVOL UITGEVOERD!")
        print("="*60)

    except Exception as general_error:
        print(f"\n[!] Algemene fout tijdens het proces: {general_error}")
    finally:
        await client.close()

if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("--nodes", type=int, nargs="+", required=True)
    ap.add_argument("--switch", type=int, required=True)
    ap.add_argument("--group-id", type=lambda x: int(x, 0), default=DEFAULT_GROUP_ID)
    ap.add_argument("--group-name", default=DEFAULT_GROUP_NAME)
    ap.add_argument("--switch-endpoint", type=int, default=2)
    ap.add_argument("--keyset-id", type=int, default=DEFAULT_KEYSET_ID)
    asyncio.run(run_logic(ap.parse_args()))
