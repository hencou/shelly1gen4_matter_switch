#!/usr/bin/env python3
"""
Maak een Matter-groep aan voor meerdere lampen (nodes 32, 33, 34, 35).

Wat dit script doet, per lamp:
  1. Leest de huidige groepslidmaatschappen uit (endpoint 1, cluster 0x04, attribuut 0)
  2. Stuurt AddGroup-commando naar elk device afzonderlijk
  3. Leest het resultaat terug ter verificatie

Wat dit script NIET doet (en ook niet hoeft te doen):
  - KeySetWrite / GroupKeyMap: HA/python-matter-server beheert de group keys
    intern. Omdat de lampen al als entiteiten in HA zichtbaar zijn, is de
    IPK (Identity Protection Key) al aanwezig op elk device.
  - ACL aanpassen voor de groep: alleen nodig als je multicast-commando's
    wilt sturen als andere controller. HA stuurt unicast per lamp, dus dit
    is optioneel. Zie de opmerking onderaan dit bestand.

Vereisten:
    pip install websockets

Gebruik:
    python3 create_matter_group.py
    python3 create_matter_group.py --ws ws://192.168.178.2:5580/ws
    python3 create_matter_group.py --group-id 0x0042 --group-name "Woonkamer"
    python3 create_matter_group.py --dry-run
"""

import argparse
import asyncio
import json
import sys

try:
    import websockets
except ImportError:
    sys.exit("Ontbrekende dependency. Installeer met: pip install websockets")

# Nodes die tot de groep moeten behoren
DEFAULT_NODES = [32, 33, 34, 35]

# Groups cluster: cluster_id=4, endpoint=1
# Attribuut 0 = NameSupport (lezen of names ondersteund worden)
# Attribuut 0 op pad "1/4/0" = group membership list
GROUPS_CLUSTER_ID = 4
GROUPS_ENDPOINT   = 1

# Standaard group-ID. Kies een waarde tussen 0x0001 en 0xFFF7.
# Vermijd 0x0000 (gereserveerd). Noteer deze waarde — je drukknop
# heeft hem later nodig.
DEFAULT_GROUP_ID   = 0x0042   # = 66 decimaal
DEFAULT_GROUP_NAME = "Lampengroep"


async def send_command(ws, command: str, args: dict, message_id: str) -> dict:
    """Stuur een JSON-commando naar matter-server, wacht op antwoord."""
    payload = {"message_id": message_id, "command": command, "args": args}
    await ws.send(json.dumps(payload))
    while True:
        raw = await ws.recv()
        msg = json.loads(raw)
        if msg.get("message_id") == message_id:
            return msg


async def read_group_membership(ws, node_id: int) -> list:
    """Lees de huidige groepslijst van een node (pad 1/4/0)."""
    resp = await send_command(
        ws,
        "read_attribute",
        {"node_id": node_id, "attribute_path": "1/4/0"},
        message_id=f"read-groups-{node_id}",
    )
    if "error_code" in resp or "error" in resp:
        print(f"  [!] Kon groepslijst niet lezen van node {node_id}: {resp}")
        return []

    result = resp.get("result")
    # matter-server kan resultaat als dict {"1/4/0": [...]} of als list teruggeven
    if isinstance(result, dict):
        return next(iter(result.values()), []) or []
    elif isinstance(result, list):
        return result
    return []


async def add_to_group(ws, node_id: int, group_id: int, group_name: str, dry_run: bool) -> bool:
    """Stuur AddGroup-commando naar één node. Geeft True terug bij succes."""

    # Payload als camelCase (schema 11 / sdk 2025.x)
    payload = {"groupId": group_id, "groupName": group_name}

    if dry_run:
        print(f"  [dry-run] Zou sturen: device_command AddGroup {payload}")
        return True

    resp = await send_command(
        ws,
        "device_command",
        {
            "node_id":      node_id,
            "endpoint_id":  GROUPS_ENDPOINT,
            "cluster_id":   GROUPS_CLUSTER_ID,
            "command_name": "AddGroup",
            "payload":      payload,
        },
        message_id=f"addgroup-{node_id}",
    )

    if "error_code" in resp or "error" in resp:
        print(f"  [!] Fout van matter-server: {resp}")
        return False

    # AddGroup stuurt een AddGroupResponse terug met een status-veld.
    # Status 0 = SUCCESS, status 137 = DUPLICATE (lamp zat al in de groep).
    result = resp.get("result") or {}
    status = None
    if isinstance(result, dict):
        # Veld heet "status" of TLV-key "0"
        status = result.get("status", result.get("0"))

    if status == 0:
        print(f"  [OK] Node {node_id} toegevoegd aan groep 0x{group_id:04X}")
        return True
    elif status == 137:
        print(f"  [OK] Node {node_id} was al lid van groep 0x{group_id:04X} (DUPLICATE=normaal)")
        return True
    elif status is None:
        # Sommige versies sturen geen expliciete status bij succes
        print(f"  [OK] Node {node_id}: geen fout, aangenomen als succes. Resultaat: {result}")
        return True
    else:
        print(f"  [!] Node {node_id}: AddGroup status={status} (onverwacht). Volledig antwoord: {resp}")
        return False


async def main() -> int:
    ap = argparse.ArgumentParser(
        description="Voeg Matter-lampen toe aan een groep.",
        formatter_class=argparse.ArgumentDefaultsHelpFormatter,
    )
    ap.add_argument("--ws",         default="ws://192.168.178.2:5580/ws",
                    help="matter-server WebSocket URL")
    ap.add_argument("--nodes",      type=int, nargs="+", default=DEFAULT_NODES,
                    help="node-IDs om aan de groep toe te voegen")
    ap.add_argument("--group-id",   type=lambda x: int(x, 0), default=DEFAULT_GROUP_ID,
                    help="groep-ID (hex of dec, bv. 0x0042 of 66)")
    ap.add_argument("--group-name", default=DEFAULT_GROUP_NAME,
                    help="naam van de groep (max 16 tekens)")
    ap.add_argument("--dry-run",    action="store_true",
                    help="toon wat er zou worden gedaan, schrijf niets")
    args = ap.parse_args()

    if len(args.group_name) > 16:
        sys.exit(f"FOUT: group-name mag maximaal 16 tekens zijn (nu {len(args.group_name)})")

    print(f"[*] Verbinding maken met {args.ws} ...")
    try:
        ws = await websockets.connect(args.ws, max_size=2**22)
    except Exception as exc:
        sys.exit(f"FOUT: kan niet verbinden: {exc}")

    async with ws:
        # Server-info pakket direct na connect
        try:
            info = json.loads(await asyncio.wait_for(ws.recv(), timeout=5))
            print(f"[*] matter-server: schema={info.get('schema_version')} "
                  f"sdk={info.get('sdk_version')} "
                  f"fabric_id={info.get('fabric_id')}")
        except asyncio.TimeoutError:
            print("[!] Geen server-info pakket binnen 5s — ga door")

        print(f"\n[*] Groep aanmaken: ID=0x{args.group_id:04X} ({args.group_id}), "
              f"naam='{args.group_name}'")
        print(f"[*] Nodes: {args.nodes}")
        if args.dry_run:
            print("[*] DRY-RUN modus — er wordt niets geschreven\n")

        results = {}
        for node_id in args.nodes:
            print(f"\n--- Node {node_id} ---")

            # Lees huidige groepslidmaatschappen
            current = await read_group_membership(ws, node_id)
            if current:
                print(f"  Huidige groepen: {current}")
            else:
                print(f"  Huidige groepen: (geen of kon niet lezen)")

            # Voeg toe aan groep
            ok = await add_to_group(ws, node_id, args.group_id, args.group_name, args.dry_run)
            results[node_id] = ok

            # Verificatie: lees opnieuw (alleen bij echte schrijfactie)
            if ok and not args.dry_run:
                await asyncio.sleep(0.5)   # kleine pauze zodat lamp kan bijwerken
                updated = await read_group_membership(ws, node_id)
                print(f"  Groepen na toevoeging: {updated}")

        # Samenvatting
        print("\n" + "="*50)
        print("Samenvatting:")
        all_ok = True
        for node_id, ok in results.items():
            status_str = "OK" if ok else "MISLUKT"
            print(f"  Node {node_id}: {status_str}")
            if not ok:
                all_ok = False

        if all_ok:
            print(f"\n[*] Klaar. Alle nodes zijn lid van groep 0x{args.group_id:04X}.")
            if not args.dry_run:
                print(f"\n--- Volgende stap: drukknop koppelen ---")
                print(f"Noteer dit group-ID voor de binding van je drukknop: 0x{args.group_id:04X} ({args.group_id})")
                print("Zie de uitleg in de comments onderaan dit script.")
        else:
            print("\n[!] Eén of meer nodes mislukten. Controleer de output hierboven.")

        return 0 if all_ok else 1


# =============================================================================
# UITLEG: drukknop koppelen aan de groep
# =============================================================================
#
# Een Matter-drukknop (Generic Switch of On/Off Switch) bestuurt lampen via
# een Binding in het Binding cluster (cluster 0x001E, endpoint 1).
#
# Voor groepsbesturing schrijf je een binding-entry die verwijst naar de
# group-ID in plaats van een individuele node. De knop stuurt dan een
# multicast OnOff-commando naar het groepsadres — alle lampen in de groep
# reageren tegelijk.
#
# Stap 1: ACL op elke lamp uitbreiden met een Group-entry
# -------------------------------------------------------
# Elke lamp moet in zijn ACL een entry hebben die het groepsadres toelaat.
# Voeg aan de bestaande ACL van elke lamp een entry toe:
#
#   {
#     "privilege": 3,          # Operate
#     "authMode": 3,           # Group
#     "subjects": [<group_id>],
#     "targets": null
#   }
#
# Dit kun je doen met een aangepaste versie van wipe_acl.py: lees de ACL,
# voeg deze entry toe (als die nog niet aanwezig is), en schrijf terug.
#
# Stap 2: Binding schrijven op de drukknop
# -----------------------------------------
# Schrijf naar de drukknop (jouw node-ID) de volgende binding-entry via
# write_attribute op pad "1/30/0":
#
#   [
#     {
#       "node": <knop_node_id>,         # verplicht eigen node
#       "group": <group_id>,            # 0x0042 (of wat je hebt gekozen)
#       "endpoint": null,
#       "cluster": 6                    # OnOff cluster
#     }
#   ]
#
# Na deze stap stuurt de knop bij een druk een multicast OnOff-commando
# naar groep 0x0042 — alle lampen reageren gelijktijdig zonder dat HA
# tussenkomt.
#
# Let op: de drukknop en de lampen moeten in hetzelfde Matter-fabric zitten
# (dat is het geval als ze allemaal via dezelfde HA zijn gecommissiond).
# =============================================================================

if __name__ == "__main__":
    sys.exit(asyncio.run(main()) or 0)
