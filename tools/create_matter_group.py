#!/usr/bin/env python3
"""
=============================================================================
HOWTO: GESTROOMLIJNDE MATTER MULTICAST & BINDING SETUP (CUSTOM FIRMWARE)
=============================================================================

1. VOORBEREIDING:
   Installeer 'websockets' op je externe machine als dat nog niet is gebeurd:
   pip install websockets

2. UITVOERING:
   Run het script vanaf je laptop. Dit script voegt de lampen toe aan de
   multicast-groep en schrijft de binding-tabel naar de switch op Endpoint 2.
   
   python3 tools/create_matter_group.py --nodes 32 33 34 35 --switch 29 --group-id 0x0001

3. PARAMETERS:
   --nodes            Lijst met Node ID's van de doellampen (bijv. 32 33 34 35)
   --switch           Node ID van de Shelly schakelaar (bijv. 29)
   --group-id         De multicast Group ID (Standaard: 0x0001)
   --switch-endpoint  Het endpoint van de knop op de switch (Standaard: 2 voor custom firmware)
   --group-name       Naam van de groep (Standaard: "Kantoor")

=============================================================================
"""

import argparse
import asyncio
import json
import sys
import websockets

DEFAULT_GROUP_ID   = 0x0001
DEFAULT_GROUP_NAME = "Kantoor"

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
    client = MatterRemoteClient(SERVER_IP, SERVER_PORT)
    await client.connect()

    print(f"\n" + "="*60)
    print(f"[*] START GESTROOMLIJNDE MULTICAST CONFIGURATIE")
    print(f"[*] Groep ID:   0x{args.group_id:04X} ({args.group_name})")
    print(f"[*] Doellampen: {args.nodes}")
    print(f"[*] Schakelaar: Node {args.switch} (Endpoint {args.switch_endpoint})")
    print("="*60 + "\n")

    try:
        # =======================================================================
        # DEEL 1: DE LAMPEN LID MAKEN VAN DE GROEP
        # =======================================================================
        print("[DEEL 1] Lampen toevoegen aan de multicast groep...")
        for node_id in args.nodes:
            print(f"  --> Configureer Lamp (Node {node_id})...")
            try:
                await client.send_command("device_command", {
                    "node_id": node_id, "endpoint_id": 1, "cluster_id": 4,
                    "command_name": "AddGroup",
                    "payload": {"groupId": args.group_id, "groupName": args.group_name}
                })
                print(f"    [OK] AddGroup succesvol afgerond")
            except Exception as e:
                print(f"    [!] AddGroup variant A mislukt, probeer variant B: {e}")
                try:
                    await client.send_command("device_command", {
                        "node_id": node_id, "endpoint_id": 1, "cluster_id": 4,
                        "command_name": "AddGroup",
                        "payload": {"groupID": args.group_id, "groupName": args.group_name}
                    })
                    print(f"    [OK] AddGroup succesvol afgerond (Variant B)")
                except Exception as e2:
                    print(f"    [!] Fout bij toevoegen aan groep: {e2}")

        # =======================================================================
        # DEEL 2: DE BINDING OP DE SWITCH ZETTEN
        # =======================================================================
        print("\n" + "-"*50)
        print("[DEEL 2] Binding tabel wegschrijven naar de Switch...")
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

        print("\n" + "="*60)
        print("[*] SCRIPT VOLTOOID!")
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
    asyncio.run(run_logic(ap.parse_args()))
