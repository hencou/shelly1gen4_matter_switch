#!/usr/bin/env python3
"""
=============================================================================
HOWTO: MATTER MULTICAST GROUP & BINDING SETUP (CUSTOM FIRMWARE)
=============================================================================

1. VOORBEREIDING:
   Installeer 'websockets' op je externe machine als dat nog niet is gebeurd:
   pip install websockets

2. UITVOERING:
   Run het script vanaf je laptop. Dit script voegt de lampen toe aan de
   multicast-groep en schrijft de binding-tabel naar de switch op Endpoint 1.
   
   python3 tools/create_matter_group.py --nodes 32 33 34 35 --switch 29 --group-id 0x0001

3. PARAMETERS:
   --nodes            Lijst met Node ID's van de doellampen (bijv. 32 33 34 35)
   --switch           Node ID van de Shelly schakelaar (bijv. 29)
   --group-id         De multicast Group ID (Standaard: 0x0001)
   --switch-endpoint  Het endpoint van de knop op de switch (Standaard: 1)
   --group-name       Naam van de groep (Standaard: "Kantoor")
   --server-ip        IP adres van de HA Matter Server (Standaard: 192.168.178.2)
   --server-port      Poort van de HA Matter Server (Standaard: 5580)

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
                if "error_code" in response and response["error_code"] != 0:
                    raise Exception(f"{response.get('details', response)}")
                return response.get("result")

    async def close(self):
        if self.ws: await self.ws.close()

async def run_logic(args):
    client = MatterRemoteClient(args.server_ip, args.server_port)
    await client.connect()

    print(f"\n" + "="*60)
    print(f"[*] START MATTER MULTICAST CONFIGURATIE")
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
                print(f"    [OK] AddGroup succesvol")
            except Exception as e:
                print(f"    [!] AddGroup mislukt: {e}")

        # =======================================================================
        # DEEL 2: GROUPKEYMAP SCHRIJVEN OP LAMPEN EN SWITCH
        # =======================================================================
        # Elk apparaat dat groep-multicast moet ontvangen of versturen, heeft
        # een GroupKeyMap entry nodig die de group ID koppelt aan een key set.
        # KeySet 0 = de IPK (Identity Protection Key) die de controller
        # installeert bij commissioning — aanwezig op alle nodes in de fabric.
        print("\n" + "-"*50)
        print("[DEEL 2] GroupKeyMap schrijven (group → IPK keyset)...")

        # GroupKeyMap: endpoint 0, cluster 63 (GroupKeyManagement), attribute 0
        group_key_entry = {
            "groupId": args.group_id,
            "groupKeySetID": 0,  # IPK keyset
        }

        all_nodes = list(args.nodes) + [args.switch]
        for node_id in all_nodes:
            label = "Switch" if node_id == args.switch else f"Lamp (Node {node_id})"
            print(f"  --> GroupKeyMap op {label}...")
            try:
                result = await client.send_command("write_attribute", {
                    "node_id": node_id,
                    "attribute_path": "0/63/0",
                    "value": [group_key_entry]
                })
                print(f"    [OK] GroupKeyMap geschreven")
            except Exception as e:
                print(f"    [!] GroupKeyMap mislukt: {e}")

        # =======================================================================
        # DEEL 3: DE BINDING OP DE SWITCH ZETTEN
        # =======================================================================
        print("\n" + "-"*50)
        print("[DEEL 3] Binding tabel wegschrijven naar de Switch...")

        # TargetStruct veldnamen conform CHIP Python SDK:
        #   node     = target node ID (voor unicast)
        #   group    = target group ID (voor multicast)
        #   endpoint = target endpoint (voor unicast)
        #   cluster  = target cluster ID
        #   fabricIndex = wordt automatisch ingevuld door de server
        binding_entry = {
            "group": args.group_id,
            "cluster": 6,           # OnOff cluster
        }

        # Probeer eerst set_node_binding (purpose-built API)
        try:
            result = await client.send_command("set_node_binding", {
                "node_id": args.switch,
                "endpoint": args.switch_endpoint,
                "bindings": [binding_entry]
            })
            print(f"    [OK] Binding weggeschreven via set_node_binding!")
            print(f"    Resultaat: {result}")
        except Exception as e:
            print(f"    [!] set_node_binding mislukt ({e}), probeer write_attribute...")
            # Fallback: write_attribute met correcte veldnamen
            try:
                result = await client.send_command("write_attribute", {
                    "node_id": args.switch,
                    "attribute_path": f"{args.switch_endpoint}/30/0",
                    "value": [binding_entry]
                })
                print(f"    [OK] Binding weggeschreven via write_attribute!")
                print(f"    Resultaat: {result}")
            except Exception as e2:
                print(f"    [!] write_attribute ook mislukt: {e2}")

        # =======================================================================
        # DEEL 4: VERIFICATIE - LEES BINDING TERUG
        # =======================================================================
        print("\n" + "-"*50)
        print("[DEEL 4] Verificatie - binding teruglezen...")
        try:
            result = await client.send_command("read_attribute", {
                "node_id": args.switch,
                "attribute_path": f"{args.switch_endpoint}/30/0"
            })
            print(f"    Huidige binding tabel: {json.dumps(result, indent=2)}")
        except Exception as e:
            print(f"    [!] Kon binding niet teruglezen: {e}")

        print("\n" + "="*60)
        print("[*] SCRIPT VOLTOOID!")
        print("="*60)
        print("\nTest nu de schakelaar — druk kort op de knop en controleer")
        print("of alle lampen in de groep tegelijk schakelen.")
        print("\nAls het niet werkt:")
        print("  1. Controleer de Shelly log op 'SwitchWorker' regels")
        print("  2. Controleer of 'total' > 0 (binding gevonden)")
        print("  3. Herstart de Shelly NIET — bindings moeten direct werken")

    except Exception as general_error:
        print(f"\n[!] Algemene fout: {general_error}")
    finally:
        await client.close()

if __name__ == "__main__":
    ap = argparse.ArgumentParser(
        description="Configureer Matter multicast groep + binding voor Shelly switch")
    ap.add_argument("--nodes", type=int, nargs="+", required=True,
                    help="Node ID's van de doellampen")
    ap.add_argument("--switch", type=int, required=True,
                    help="Node ID van de Shelly schakelaar")
    ap.add_argument("--group-id", type=lambda x: int(x, 0), default=DEFAULT_GROUP_ID,
                    help="Multicast Group ID (standaard: 0x0001)")
    ap.add_argument("--group-name", default=DEFAULT_GROUP_NAME,
                    help="Naam van de groep (standaard: Kantoor)")
    ap.add_argument("--switch-endpoint", type=int, default=1,
                    help="Endpoint op de switch (standaard: 1 = pushbutton/toggle)")
    ap.add_argument("--server-ip", default=SERVER_IP,
                    help=f"IP van de HA Matter Server (standaard: {SERVER_IP})")
    ap.add_argument("--server-port", type=int, default=SERVER_PORT,
                    help=f"Poort van de HA Matter Server (standaard: {SERVER_PORT})")
    asyncio.run(run_logic(ap.parse_args()))
