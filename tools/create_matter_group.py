#!/usr/bin/env python3
"""
=============================================================================
HOWTO: MATTER MULTICAST GROUP & BINDING SETUP (CUSTOM FIRMWARE)
=============================================================================

1. PREREQUISITES:
   Install 'websockets' on your machine if not already done:
   pip install websockets

2. USAGE:
   Run this script from your laptop. It installs a shared group encryption
   key on all nodes, adds lamps to the multicast group, writes the
   GroupKeyMap, and sets the binding table on the switch (Endpoint 1).

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
import base64
import json
import sys
import websockets

DEFAULT_GROUP_ID   = 0x0001
DEFAULT_GROUP_NAME = "Kantoor"

SERVER_IP   = "192.168.178.2"
SERVER_PORT = 5580

# KeySet ID used for group encryption. We use 1 (not 0/IPK) because some
# SDK versions cannot use the IPK directly for group messaging.
GROUP_KEYSET_ID = 1

# Default 128-bit epoch key (hex). All nodes in the group MUST share the
# same key to encrypt/decrypt multicast messages.  Change this if you want
# your own key; it must be exactly 32 hex characters (16 bytes).
# IMPORTANT: must match the key in app_main.cpp (kGroupEpochKey).
DEFAULT_EPOCH_KEY = "d0d1d2d3d4d5d6d7d8d9dadbdcdddedf"

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
        # STEP 1: INSTALL GROUP ENCRYPTION KEY ON ALL NODES (KeySetWrite)
        # =======================================================================
        # Matter group multicast messages are encrypted with a shared key.
        # All nodes (sender + receivers) must have the same KeySet installed.
        # We use KeySet ID 1 (not 0/IPK) because some SDK versions have issues
        # using the IPK directly for group encryption.
        print("[STEP 1] Installing group encryption key (KeySetWrite)...")

        # Convert hex key to base64 (python-matter-server API expects
        # bytes fields as base64-encoded strings)
        key_bytes = bytes.fromhex(args.epoch_key)
        if len(key_bytes) != 16:
            print(f"[!] ERROR: epoch key must be exactly 16 bytes (32 hex chars)")
            return
        epoch_key_b64 = base64.b64encode(key_bytes).decode()
        print(f"    Key (hex):    {args.epoch_key}")
        print(f"    Key (base64): {epoch_key_b64}")

        all_nodes = list(args.nodes) + [args.switch]
        for node_id in all_nodes:
            label = "Switch" if node_id == args.switch else f"Lamp (Node {node_id})"
            print(f"  --> KeySetWrite on {label}...")
            try:
                await client.send_command("device_command", {
                    "node_id": node_id,
                    "endpoint_id": 0,
                    "cluster_id": 63,  # GroupKeyManagement
                    "command_name": "KeySetWrite",
                    "payload": {
                        "groupKeySet": {
                            "groupKeySetID": GROUP_KEYSET_ID,
                            "groupKeySecurityPolicy": 0,  # TrustFirst
                            "epochKey0": epoch_key_b64,
                            "epochStartTime0": 1,  # 1 microsecond = always valid
                            "epochKey1": None,
                            "epochStartTime1": None,
                            "epochKey2": None,
                            "epochStartTime2": None,
                        }
                    }
                })
                print(f"    [OK] KeySet {GROUP_KEYSET_ID} installed")
            except Exception as e:
                print(f"    [!] KeySetWrite failed: {e}")

        # =======================================================================
        # STEP 2: ADD LAMPS TO THE MULTICAST GROUP
        # =======================================================================
        print("\n" + "-"*50)
        print("[STEP 2] Adding lamps to multicast group...")
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
        # STEP 3: ADD GROUP ACL ON LAMPS
        # =======================================================================
        # Matter Access Control requires an explicit ACL entry that allows
        # group commands.  Without this, the lamp decrypts the message but
        # silently rejects it because no ACL grants Operate privilege for
        # the group's authMode.
        #
        # IMPORTANT: python-matter-server returns ACL entries with integer
        # TLV tag keys ("1","2","3","4","254") but write_attribute expects
        # string field names ("privilege","authMode","subjects","targets").
        # We must normalise all entries to string keys before writing back.
        print("\n" + "-"*50)
        print("[STEP 3] Adding Group ACL entry on lamps...")

        # TLV tag → field-name mapping for AccessControlEntryStruct
        ACL_TAG_MAP = {"1": "privilege", "2": "authMode", "3": "subjects",
                       "4": "targets", "254": "fabricIndex"}

        def normalise_acl_entry(entry):
            """Convert an ACL entry from TLV integer keys to string keys."""
            out = {}
            for k, v in entry.items():
                name = ACL_TAG_MAP.get(str(k), str(k))
                out[name] = v
            return out

        for node_id in args.nodes:
            print(f"  --> ACL on Lamp (Node {node_id})...")
            try:
                # Read current ACL (cluster 31, attribute 0, endpoint 0)
                acl = await client.send_command("read_attribute", {
                    "node_id": node_id,
                    "attribute_path": "0/31/0"
                })
                # Extract the list (response format: {"0/31/0": [...]} )
                acl_key = list(acl.keys())[0] if isinstance(acl, dict) else None
                acl_list = acl[acl_key] if acl_key else acl
                if not isinstance(acl_list, list):
                    acl_list = [acl_list] if acl_list else []

                # Normalise all entries to string field names
                acl_list = [normalise_acl_entry(e) for e in acl_list]
                print(f"    Current ACL ({len(acl_list)} entries):")
                for i, e in enumerate(acl_list):
                    priv = e.get("privilege", "?")
                    auth = e.get("authMode", "?")
                    subj = e.get("subjects", [])
                    print(f"      #{i+1}: privilege={priv} authMode={auth} subjects={subj}")

                # Check if a group ACL entry for our group already exists
                group_acl_exists = False
                for entry in acl_list:
                    if (entry.get("authMode") == 3 and
                        args.group_id in (entry.get("subjects") or [])):
                        group_acl_exists = True
                        break

                if group_acl_exists:
                    print(f"    [OK] Group ACL already present")
                else:
                    # Drop fabricIndex from all entries — the server auto-fills it
                    for entry in acl_list:
                        entry.pop("fabricIndex", None)

                    # IMPORTANT: the admin entry (Administer/CASE) MUST come first
                    # so the commissioner does not lose access mid-write.
                    # Re-sort: Administer entries first, then the rest.
                    admin_entries = [e for e in acl_list if e.get("privilege") == 5]
                    other_entries = [e for e in acl_list if e.get("privilege") != 5]

                    # Build new list: admin first, then others, then group ACL
                    new_acl = admin_entries + other_entries + [{
                        "privilege": 3,   # Operate
                        "authMode": 3,    # Group
                        "subjects": [args.group_id],
                        "targets": None,  # All endpoints/clusters
                    }]
                    await client.send_command("write_attribute", {
                        "node_id": node_id,
                        "attribute_path": "0/31/0",
                        "value": new_acl
                    })
                    print(f"    [OK] Group ACL added (Operate privilege for group 0x{args.group_id:04X})")
            except Exception as e:
                print(f"    [!] ACL setup failed: {e}")

        # =======================================================================
        # STEP 4: WRITE GROUPKEYMAP ON ALL NODES
        # =======================================================================
        # Maps the group ID to our KeySet so the SDK can find the encryption key.
        print("\n" + "-"*50)
        print(f"[STEP 4] Writing GroupKeyMap (group -> keyset {GROUP_KEYSET_ID})...")

        # GroupKeyMap: endpoint 0, cluster 63 (GroupKeyManagement), attribute 0
        group_key_entry = {
            "groupId": args.group_id,
            "groupKeySetID": GROUP_KEYSET_ID,
        }

        for node_id in all_nodes:
            label = "Switch" if node_id == args.switch else f"Lamp (Node {node_id})"
            print(f"  --> GroupKeyMap on {label}...")
            try:
                await client.send_command("write_attribute", {
                    "node_id": node_id,
                    "attribute_path": "0/63/0",
                    "value": [group_key_entry]
                })
                print(f"    [OK] GroupKeyMap written")
            except Exception as e:
                print(f"    [!] GroupKeyMap failed: {e}")

        # =======================================================================
        # STEP 5: WRITE BINDING ON THE SWITCH
        # =======================================================================
        print("\n" + "-"*50)
        print("[STEP 5] Writing binding table to the Switch...")

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
        # STEP 6: VERIFICATION - READ BINDING BACK
        # =======================================================================
        print("\n" + "-"*50)
        print("[STEP 6] Verification - reading binding back...")
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
        print("  1. Check the Shelly log for 'send_onoff_multicast' lines")
        print("  2. Look for OK (sent) vs FAILED (SDK error)")
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
    ap.add_argument("--epoch-key", default=DEFAULT_EPOCH_KEY,
                    help="128-bit hex key for group encryption (32 hex chars)")
    asyncio.run(run_logic(ap.parse_args()))
