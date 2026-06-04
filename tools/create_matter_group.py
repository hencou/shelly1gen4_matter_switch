#!/usr/bin/env python3
"""
=============================================================================
HOWTO: MATTER MULTICAST GROUP & BINDING SETUP (CUSTOM FIRMWARE)
=============================================================================

1. PREREQUISITES:
   Install 'websockets' on your machine if not already done:
   pip install websockets

2. USAGE:
   Run this script from your laptop. It adds lamps to the multicast group,
   writes the GroupKeyMap on all nodes, and sets the binding table on the
   switch (Endpoint 1).

   python3 tools/create_matter_group.py --nodes 32 33 34 35 --switch 29 --group-id 0x0001

3. PARAMETERS:
   --nodes            List of Node IDs of the target lamps (e.g. 32 33 34 35)
   --switch           Node ID of the Shelly switch (e.g. 29)
   --group-id         The multicast Group ID (default: 0x0001)
   --switch-endpoint  The button endpoint on the switch (default: 1)
   --group-name       Name of the group (default: "Kantoor")
   --server-ip        IP address of the HA Matter Server (default: 192.168.178.2)
   --server-port      Port of the HA Matter Server (default: 5580)

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
        print(f"[*] Connecting to Matter Server at {self.url}...")
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
    print(f"[*] START MATTER MULTICAST CONFIGURATION")
    print(f"[*] Group ID:    0x{args.group_id:04X} ({args.group_name})")
    print(f"[*] Target lamps: {args.nodes}")
    print(f"[*] Switch:      Node {args.switch} (Endpoint {args.switch_endpoint})")
    print("="*60 + "\n")

    try:
        # =======================================================================
        # STEP 1: ADD LAMPS TO THE MULTICAST GROUP
        # =======================================================================
        print("[STEP 1] Adding lamps to multicast group...")
        for node_id in args.nodes:
            print(f"  --> Configure Lamp (Node {node_id})...")
            try:
                await client.send_command("device_command", {
                    "node_id": node_id, "endpoint_id": 1, "cluster_id": 4,
                    "command_name": "AddGroup",
                    "payload": {"groupId": args.group_id, "groupName": args.group_name}
                })
                print(f"    [OK] AddGroup successful")
            except Exception as e:
                print(f"    [!] AddGroup failed: {e}")

        # =======================================================================
        # STEP 2: WRITE GROUPKEYMAP ON LAMPS AND SWITCH
        # =======================================================================
        # Every device that needs to receive or send group multicast messages
        # requires a GroupKeyMap entry linking the group ID to a key set.
        # KeySet 0 = the IPK (Identity Protection Key) installed by the
        # controller during commissioning — present on all nodes in the fabric.
        print("\n" + "-"*50)
        print("[STEP 2] Writing GroupKeyMap (group -> IPK keyset)...")

        # GroupKeyMap: endpoint 0, cluster 63 (GroupKeyManagement), attribute 0
        group_key_entry = {
            "groupId": args.group_id,
            "groupKeySetID": 0,  # IPK keyset
        }

        all_nodes = list(args.nodes) + [args.switch]
        for node_id in all_nodes:
            label = "Switch" if node_id == args.switch else f"Lamp (Node {node_id})"
            print(f"  --> GroupKeyMap on {label}...")
            try:
                result = await client.send_command("write_attribute", {
                    "node_id": node_id,
                    "attribute_path": "0/63/0",
                    "value": [group_key_entry]
                })
                print(f"    [OK] GroupKeyMap written")
            except Exception as e:
                print(f"    [!] GroupKeyMap failed: {e}")

        # =======================================================================
        # STEP 3: WRITE BINDING ON THE SWITCH
        # =======================================================================
        print("\n" + "-"*50)
        print("[STEP 3] Writing binding table to the Switch...")

        # TargetStruct field names per CHIP Python SDK:
        #   node     = target node ID (for unicast)
        #   group    = target group ID (for multicast)
        #   endpoint = target endpoint (for unicast)
        #   cluster  = target cluster ID
        #   fabricIndex = auto-filled by the server
        binding_entry = {
            "group": args.group_id,
            "cluster": 6,           # OnOff cluster
        }

        # Try set_node_binding first (purpose-built API)
        try:
            result = await client.send_command("set_node_binding", {
                "node_id": args.switch,
                "endpoint": args.switch_endpoint,
                "bindings": [binding_entry]
            })
            print(f"    [OK] Binding written via set_node_binding!")
            print(f"    Result: {result}")
        except Exception as e:
            print(f"    [!] set_node_binding failed ({e}), trying write_attribute...")
            # Fallback: write_attribute with correct field names
            try:
                result = await client.send_command("write_attribute", {
                    "node_id": args.switch,
                    "attribute_path": f"{args.switch_endpoint}/30/0",
                    "value": [binding_entry]
                })
                print(f"    [OK] Binding written via write_attribute!")
                print(f"    Result: {result}")
            except Exception as e2:
                print(f"    [!] write_attribute also failed: {e2}")

        # =======================================================================
        # STEP 4: VERIFICATION - READ BINDING BACK
        # =======================================================================
        print("\n" + "-"*50)
        print("[STEP 4] Verification - reading binding back...")
        try:
            result = await client.send_command("read_attribute", {
                "node_id": args.switch,
                "attribute_path": f"{args.switch_endpoint}/30/0"
            })
            print(f"    Current binding table: {json.dumps(result, indent=2)}")
        except Exception as e:
            print(f"    [!] Could not read binding: {e}")

        print("\n" + "="*60)
        print("[*] SCRIPT COMPLETE!")
        print("="*60)
        print("\nTest the switch now — press the button briefly and check")
        print("whether all lamps in the group toggle simultaneously.")
        print("\nIf it does not work:")
        print("  1. Check the Shelly log for 'SwitchWorker' lines")
        print("  2. Verify that 'total' > 0 (binding found)")
        print("  3. Do NOT restart the Shelly — bindings should work immediately")

    except Exception as general_error:
        print(f"\n[!] General error: {general_error}")
    finally:
        await client.close()

if __name__ == "__main__":
    ap = argparse.ArgumentParser(
        description="Configure Matter multicast group + binding for Shelly switch")
    ap.add_argument("--nodes", type=int, nargs="+", required=True,
                    help="Node IDs of the target lamps")
    ap.add_argument("--switch", type=int, required=True,
                    help="Node ID of the Shelly switch")
    ap.add_argument("--group-id", type=lambda x: int(x, 0), default=DEFAULT_GROUP_ID,
                    help="Multicast Group ID (default: 0x0001)")
    ap.add_argument("--group-name", default=DEFAULT_GROUP_NAME,
                    help="Name of the group (default: Kantoor)")
    ap.add_argument("--switch-endpoint", type=int, default=1,
                    help="Endpoint on the switch (default: 1 = pushbutton/toggle)")
    ap.add_argument("--server-ip", default=SERVER_IP,
                    help=f"IP of the HA Matter Server (default: {SERVER_IP})")
    ap.add_argument("--server-port", type=int, default=SERVER_PORT,
                    help=f"Port of the HA Matter Server (default: {SERVER_PORT})")
    asyncio.run(run_logic(ap.parse_args()))
