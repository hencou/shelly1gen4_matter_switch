/*
 * Matter device implementatie voor Shelly 1 Gen4 (ESP32-C6).
 *
 * 4 endpoints:
 *   EP1 = OnOff Light Switch  + OnOff client + LevelControl client + Binding cluster
 *   EP2 = Temperature Sensor  (server)
 *   EP3 = Occupancy Sensor    (server)
 *   EP4 = Dimmer Switch       + OnOff client + LevelControl client + Binding cluster
 *
 * Commando-emit naar bound nodes/groups:
 *   - app_main.c roept matter_send_onoff_toggle(ep) / matter_send_level_move/stop(ep)
 *   - die data wordt naar de CHIP-thread gescheduled (BindingManager::NotifyBoundClusterChanged)
 *   - LightSwitchChangedHandler ontvangt elke binding-entry (unicast óf multicast)
 *     en stuurt het juiste Matter-commando via chip::Controller::InvokeCommandRequest /
 *     Controller::InvokeGroupCommandRequest.
 */

#include "matter_device.h"

extern "C" {
#include "app_config.h"
#include "esp_log.h"
}

#include <esp_matter.h>
#include <esp_matter_attribute_utils.h>
#include <esp_matter_endpoint.h>
#include <esp_matter_cluster.h>
#include <esp_matter_core.h>
#include <esp_matter_ota.h>

#include <app/server/Server.h>
#include <app/util/binding-table.h>
#include <app/clusters/bindings/BindingManager.h>
#include <app/CASESessionManager.h>
#include <app-common/zap-generated/cluster-objects.h>
#include <controller/InvokeInteraction.h>
#include <credentials/FabricTable.h>
#include <platform/PlatformManager.h>

#if CHIP_DEVICE_CONFIG_ENABLE_THREAD
#include <platform/ESP32/OpenthreadLauncher.h>
#include "esp_openthread_types.h"

/* Default-config macros worden in esp-matter/esp-idf NIET door esp_openthread.h
 * geleverd — canonical Shelly Thread firmware definieert ze lokaal. Zelfde
 * waarden hier: native 802.15.4 radio (ESP32-C6), geen host bridge,
 * NVS-partitie voor SRP key storage. */
#define ESP_OPENTHREAD_DEFAULT_RADIO_CONFIG()                                           \
    {                                                                                   \
        .radio_mode = RADIO_MODE_NATIVE,                                                \
    }
#define ESP_OPENTHREAD_DEFAULT_HOST_CONFIG()                                            \
    {                                                                                   \
        .host_connection_mode = HOST_CONNECTION_MODE_NONE,                              \
    }
#define ESP_OPENTHREAD_DEFAULT_PORT_CONFIG()                                            \
    {                                                                                   \
        .storage_partition_name = "nvs", .netif_queue_size = 10, .task_queue_size = 10, \
    }
#endif

static const char *TAG = "matter_dev";

using namespace esp_matter;
using namespace esp_matter::attribute;
using namespace esp_matter::cluster;
using namespace esp_matter::endpoint;
using namespace chip;
using namespace chip::app::Clusters;

static uint16_t s_ep_drukker = 0;
static uint16_t s_ep_temp    = 0;
static uint16_t s_ep_occ     = 0;
static uint16_t s_ep_touch   = 0;

/* ---------------- Binding-mediated command emit ---------------- */

struct BindingCommandData
{
    chip::EndpointId localEndpointId = 1;
    chip::CommandId  commandId       = 0;
    chip::ClusterId  clusterId       = 0;
    bool             isGroup         = false;
    /* level-control payload */
    uint8_t moveMode = 0; /* 0 = up, 1 = down */
    uint8_t rate     = 50;
};

/* Typed lambda callbacks voor InvokeCommandRequest (v1.4 API).
 * - onSuccess krijgt de typed ResponseType (generic via auto&)
 * - onError krijgt alleen CHIP_ERROR (geen context-pointer meer)
 */
static auto make_on_success() {
    return [](const chip::app::ConcreteCommandPath & path,
              const chip::app::StatusIB & /*status*/,
              const auto & /*response*/) {
        ESP_LOGI(TAG, "Invoke success ep=%u cmd=0x%lx",
                 path.mEndpointId, (unsigned long) path.mCommandId);
    };
}
static auto make_on_error() {
    return [](CHIP_ERROR err) {
        ESP_LOGW(TAG, "Invoke failure: %" CHIP_ERROR_FORMAT, err.Format());
    };
}

static void send_onoff_unicast(BindingCommandData *d, const EmberBindingTableEntry &b,
                               chip::OperationalDeviceProxy *peer)
{
    if (d->commandId == OnOff::Commands::Toggle::Id) {
        OnOff::Commands::Toggle::Type cmd;
        chip::Controller::InvokeCommandRequest(peer->GetExchangeManager(),
            peer->GetSecureSession().Value(), b.remote, cmd, make_on_success(), make_on_error());
    }
}

static void send_level_unicast(BindingCommandData *d, const EmberBindingTableEntry &b,
                               chip::OperationalDeviceProxy *peer)
{
    if (d->commandId == LevelControl::Commands::Move::Id) {
        LevelControl::Commands::Move::Type cmd;
        cmd.moveMode = (d->moveMode == 0) ? LevelControl::MoveModeEnum::kUp
                                          : LevelControl::MoveModeEnum::kDown;
        cmd.rate.SetNonNull(d->rate);
        /* optionsMask/optionsOverride default-init naar lege BitMask */
        chip::Controller::InvokeCommandRequest(peer->GetExchangeManager(),
            peer->GetSecureSession().Value(), b.remote, cmd, make_on_success(), make_on_error());
    } else if (d->commandId == LevelControl::Commands::Stop::Id) {
        LevelControl::Commands::Stop::Type cmd;
        chip::Controller::InvokeCommandRequest(peer->GetExchangeManager(),
            peer->GetSecureSession().Value(), b.remote, cmd, make_on_success(), make_on_error());
    }
}

static void send_onoff_multicast(BindingCommandData *d, const EmberBindingTableEntry &b)
{
    if (d->commandId == OnOff::Commands::Toggle::Id) {
        OnOff::Commands::Toggle::Type cmd;
        chip::Controller::InvokeGroupCommandRequest(nullptr, b.fabricIndex, b.groupId, cmd);
    }
}

static void send_level_multicast(BindingCommandData *d, const EmberBindingTableEntry &b)
{
    if (d->commandId == LevelControl::Commands::Move::Id) {
        LevelControl::Commands::Move::Type cmd;
        cmd.moveMode = (d->moveMode == 0) ? LevelControl::MoveModeEnum::kUp
                                          : LevelControl::MoveModeEnum::kDown;
        cmd.rate.SetNonNull(d->rate);
        chip::Controller::InvokeGroupCommandRequest(nullptr, b.fabricIndex, b.groupId, cmd);
    } else if (d->commandId == LevelControl::Commands::Stop::Id) {
        LevelControl::Commands::Stop::Type cmd;
        chip::Controller::InvokeGroupCommandRequest(nullptr, b.fabricIndex, b.groupId, cmd);
    }
}

static void LightSwitchChangedHandler(const EmberBindingTableEntry & binding,
                                      chip::OperationalDeviceProxy * peer,
                                      void * context)
{
    /* ESP_LOGE forceert dat dit log altijd zichtbaar is, ongeacht log-level. */
    ESP_LOGE(TAG, ">>> BindingHandler CALLED <<< local=%u type=%u remote-ep=%u cluster=0x%lx fabric=%u nodeId=0x%llx group=%u peer=%s ctx=%p",
             binding.local, binding.type, binding.remote,
             (unsigned long) binding.clusterId.value_or(0),
             binding.fabricIndex,
             (unsigned long long) binding.nodeId,
             binding.groupId,
             peer ? "yes" : "no",
             context);
    BindingCommandData *d = static_cast<BindingCommandData *>(context);
    if (d->localEndpointId != binding.local) {
        ESP_LOGW(TAG, "BindingHandler skip: local %u != binding.local %u",
                 d->localEndpointId, binding.local);
        return;
    }

    if (binding.type == MATTER_MULTICAST_BINDING) {
        if (d->clusterId == OnOff::Id)            send_onoff_multicast(d, binding);
        else if (d->clusterId == LevelControl::Id) send_level_multicast(d, binding);
    } else if (binding.type == MATTER_UNICAST_BINDING && peer) {
        if (d->clusterId == OnOff::Id)            send_onoff_unicast(d, binding, peer);
        else if (d->clusterId == LevelControl::Id) send_level_unicast(d, binding, peer);
    } else {
        ESP_LOGW(TAG, "BindingHandler: no-op (type=%u peer=%s)",
                 binding.type, peer ? "yes" : "no");
    }
}

static void LightSwitchContextReleaseHandler(void *context)
{
    chip::Platform::Delete(static_cast<BindingCommandData *>(context));
}

static void SwitchWorkerFunction(intptr_t context)
{
    BindingCommandData *d = reinterpret_cast<BindingCommandData *>(context);
    /* Tel binding-entries voor dit endpoint+cluster + dump ELKE entry voor diagnose. */
    uint32_t matching = 0;
    uint32_t total    = 0;
    bool     command_sent = false;

    for (const auto & e : chip::BindingTable::GetInstance()) {
        total++;
        ESP_LOGI(TAG, "BindingTable[%lu]: type=%u local=%u remote=%u nodeId=0x%llx group=%u cluster=0x%lx fabric=%u",
                 (unsigned long) total,
                 (unsigned) e.type, e.local, e.remote,
                 (unsigned long long) e.nodeId,
                 (unsigned) e.groupId,
                 (unsigned long) e.clusterId.value_or(0),
                 (unsigned) e.fabricIndex);
        if (e.local != d->localEndpointId) continue;
        if (e.clusterId.HasValue() && e.clusterId.Value() != d->clusterId) continue;
        matching++;

        /* --- Direct send: bypass NotifyBoundClusterChanged callback dispatch --- */
        if (e.type == MATTER_UNICAST_BINDING) {
            chip::ScopedNodeId peer(e.nodeId, e.fabricIndex);
            auto * caseMgr = chip::Server::GetInstance().GetCASESessionManager();
            chip::OperationalDeviceProxy * proxy = (caseMgr != nullptr)
                ? caseMgr->FindExistingSession(peer) : nullptr;
            if (proxy) {
                ESP_LOGI(TAG, "SwitchWorker: direct send to node 0x%llx ep=%u cluster=0x%lx cmd=0x%lx",
                         (unsigned long long) e.nodeId, e.remote,
                         (unsigned long) d->clusterId, (unsigned long) d->commandId);
                if (d->clusterId == OnOff::Id)
                    send_onoff_unicast(d, e, proxy);
                else if (d->clusterId == LevelControl::Id)
                    send_level_unicast(d, e, proxy);
                command_sent = true;
            } else {
                ESP_LOGW(TAG, "SwitchWorker: no session to node 0x%llx, will use NotifyBoundClusterChanged",
                         (unsigned long long) e.nodeId);
            }
        } else if (e.type == MATTER_MULTICAST_BINDING) {
            ESP_LOGI(TAG, "SwitchWorker: multicast send group=%u cluster=0x%lx cmd=0x%lx",
                     e.groupId, (unsigned long) d->clusterId, (unsigned long) d->commandId);
            if (d->clusterId == OnOff::Id)
                send_onoff_multicast(d, e);
            else if (d->clusterId == LevelControl::Id)
                send_level_multicast(d, e);
            command_sent = true;
        }
    }

    ESP_LOGI(TAG, "SwitchWorker: total_entries=%lu matching=%lu command_sent=%s",
             (unsigned long) total, (unsigned long) matching,
             command_sent ? "yes" : "no");

    if (!command_sent && matching > 0) {
        /* Fallback: geen bestaande sessie gevonden — laat BindingManager
         * de CASE-connectie initiëren en via callback afhandelen. */
        ESP_LOGI(TAG, "SwitchWorker: fallback NotifyBoundClusterChanged ep=%u cluster=0x%lx",
                 d->localEndpointId, (unsigned long) d->clusterId);
        CHIP_ERROR err = chip::BindingManager::GetInstance().NotifyBoundClusterChanged(
            d->localEndpointId, d->clusterId, static_cast<void *>(d));
        ESP_LOGI(TAG, "SwitchWorker: NotifyBoundClusterChanged returned %" CHIP_ERROR_FORMAT, err.Format());
        /* d wordt vrijgegeven door LightSwitchContextReleaseHandler */
    } else {
        /* Direct verstuurd of geen bindings: ruim context zelf op */
        chip::Platform::Delete(d);
    }
}

static void switch_send(uint16_t local_ep, chip::ClusterId cluster, chip::CommandId cmd,
                        uint8_t move_mode = 0, uint8_t rate = 50)
{
    ESP_LOGI(TAG, "switch_send ep=%u cluster=0x%lx cmd=0x%lx",
             local_ep, (unsigned long) cluster, (unsigned long) cmd);
    BindingCommandData *d = chip::Platform::New<BindingCommandData>();
    d->localEndpointId = local_ep;
    d->clusterId       = cluster;
    d->commandId       = cmd;
    d->moveMode        = move_mode;
    d->rate            = rate;
    chip::DeviceLayer::PlatformMgr().ScheduleWork(
        SwitchWorkerFunction, reinterpret_cast<intptr_t>(d));
}

/* ---------------- Public API (C-callable) ---------------- */

/* Guard: ep == 0 betekent dat de bijbehorende endpoint nog niet door
 * matter_start() is aangemaakt. Defensief: vroege return zodat
 * spurious-ISR-callbacks vlak na boot niet in chip:: code crashen. */
extern "C" void matter_send_onoff_toggle(uint16_t ep)
{
    if (!ep) return;
    switch_send(ep, OnOff::Id, OnOff::Commands::Toggle::Id);
}

extern "C" void matter_send_level_move(uint16_t ep, bool up, uint8_t rate)
{
    if (!ep) return;
    switch_send(ep, LevelControl::Id, LevelControl::Commands::Move::Id,
                up ? 0 : 1, rate);
}

extern "C" void matter_send_level_stop(uint16_t ep)
{
    if (!ep) return;
    switch_send(ep, LevelControl::Id, LevelControl::Commands::Stop::Id);
}

extern "C" void matter_update_temperature(int16_t centi_c)
{
    if (!s_ep_temp) return;
    esp_matter_attr_val_t v = esp_matter_int16(centi_c);
    attribute::update(s_ep_temp,
        TemperatureMeasurement::Id,
        TemperatureMeasurement::Attributes::MeasuredValue::Id, &v);
}

extern "C" void matter_update_occupancy(bool occupied)
{
    if (!s_ep_occ) return;
    uint8_t b = occupied ? 1 : 0;
    esp_matter_attr_val_t v = esp_matter_bitmap8(b);
    attribute::update(s_ep_occ,
        OccupancySensing::Id,
        OccupancySensing::Attributes::Occupancy::Id, &v);
}

extern "C" void matter_factory_reset(void)
{
    ESP_LOGW(TAG, "factory reset requested");
    esp_matter::factory_reset();   /* wist Matter NVS + reboot */
}

extern "C" uint16_t matter_ep_drukker(void) { return s_ep_drukker; }
extern "C" uint16_t matter_ep_touch(void)   { return s_ep_touch;   }

/* ---------------- Endpoint setup ---------------- */

static esp_err_t identify_cb(identification::callback_type_t /*type*/, uint16_t endpoint_id,
                             uint8_t /*effect_id*/, uint8_t /*effect_variant*/, void * /*priv*/)
{
    ESP_LOGI(TAG, "Identify on ep=%u", endpoint_id);
    return ESP_OK;
}

static esp_err_t attribute_update_cb(attribute::callback_type_t type, uint16_t endpoint_id,
                                     uint32_t cluster_id, uint32_t attribute_id,
                                     esp_matter_attr_val_t *val, void * /*priv*/)
{
    /* We hebben geen client-side attribute writes nodig — alle outgoing
     * commands gaan via Binding (zie switch_send). */
    return ESP_OK;
}

/* Wordt op de CHIP-task gedraaid via PlatformMgr().ScheduleWork().
 * BindingManager::Init() en de Register*Handler-aanroepen praten met
 * Server-state (FabricTable / CASESessionMgr / PersistentStorage) en
 * MOETEN dus onder de CHIP-stack-lock draaien. Vanuit main_task gebeurt
 * dat niet -> 'Chip stack locking error ... unsafe/racy' -> chipDie.
 * Scheduling op CHIP-task lost dit deterministisch op. */
static void init_binding_handler_internal(intptr_t /*arg*/)
{
    auto & mgr = chip::BindingManager::GetInstance();
    chip::BindingManagerInitParams params;
    params.mFabricTable        = &chip::Server::GetInstance().GetFabricTable();
    params.mCASESessionManager = chip::Server::GetInstance().GetCASESessionManager();
    params.mStorage            = &chip::Server::GetInstance().GetPersistentStorage();
    CHIP_ERROR err = mgr.Init(params);
    if (err != CHIP_NO_ERROR) {
        ESP_LOGE(TAG, "BindingManager::Init failed: %" CHIP_ERROR_FORMAT, err.Format());
        return;
    }
    mgr.RegisterBoundDeviceChangedHandler(LightSwitchChangedHandler);
    mgr.RegisterBoundDeviceContextReleaseHandler(LightSwitchContextReleaseHandler);
    ESP_LOGI(TAG, "BindingManager initialised on CHIP-task (handler=%p ctx-release=%p)",
             (void *) &LightSwitchChangedHandler,
             (void *) &LightSwitchContextReleaseHandler);
}

static esp_err_t init_binding_handler(void)
{
    chip::DeviceLayer::PlatformMgr().ScheduleWork(init_binding_handler_internal, 0);
    return ESP_OK;
}

extern "C" esp_err_t matter_start(void)
{
    /* Node */
    node::config_t node_cfg;
    node_t *node = node::create(&node_cfg, attribute_update_cb, identify_cb);
    if (!node) { ESP_LOGE(TAG, "node create failed"); return ESP_FAIL; }

    /* EP1 — OnOff Light Switch + LevelControl client + Binding */
    on_off_switch::config_t sw_cfg;
    endpoint_t *ep_drukker = on_off_switch::create(node, &sw_cfg, ENDPOINT_FLAG_NONE, NULL);
    {
        level_control::config_t lvl_cfg;
        level_control::create(ep_drukker, &lvl_cfg,
                              CLUSTER_FLAG_CLIENT, ESP_MATTER_NONE_FEATURE_ID);
        binding::config_t bind_cfg;
        binding::create(ep_drukker, &bind_cfg, CLUSTER_FLAG_SERVER);
    }
    s_ep_drukker = endpoint::get_id(ep_drukker);
    ESP_LOGI(TAG, "EP%u = OnOff Light Switch (drukker)", s_ep_drukker);

    /* EP2 — Temperature Sensor */
    temperature_sensor::config_t t_cfg;
    endpoint_t *ep_temp = temperature_sensor::create(node, &t_cfg, ENDPOINT_FLAG_NONE, NULL);
    s_ep_temp = endpoint::get_id(ep_temp);
    ESP_LOGI(TAG, "EP%u = Temperature Sensor", s_ep_temp);

#if SECONDARY_INPUT_IS_LD2410
    /* EP3 — Occupancy Sensor (alleen wanneer LD2410 gekozen is) */
    occupancy_sensor::config_t o_cfg;
    endpoint_t *ep_occ = occupancy_sensor::create(node, &o_cfg, ENDPOINT_FLAG_NONE, NULL);
    s_ep_occ = endpoint::get_id(ep_occ);
    ESP_LOGI(TAG, "EP%u = Occupancy Sensor (LD2410)", s_ep_occ);
#else
    ESP_LOGI(TAG, "Occupancy endpoint overgeslagen (secondary input != LD2410)");
#endif

#if SECONDARY_INPUT_IS_TTP223
    /* EP4 — Dimmer Switch + Binding (alleen wanneer TTP223 gekozen is) */
    dimmer_switch::config_t d_cfg;
    endpoint_t *ep_touch = dimmer_switch::create(node, &d_cfg, ENDPOINT_FLAG_NONE, NULL);
    {
        binding::config_t bind_cfg;
        binding::create(ep_touch, &bind_cfg, CLUSTER_FLAG_SERVER);
    }
    s_ep_touch = endpoint::get_id(ep_touch);
    ESP_LOGI(TAG, "EP%u = Dimmer Switch (TTP223 touch)", s_ep_touch);
#else
    ESP_LOGI(TAG, "Dimmer-Switch endpoint overgeslagen (secondary input != TTP223)");
#endif

    /* OTA-cluster requestor (optioneel: voor Matter OTA via TBR — werkt naast onze WiFi-OTA) */
    esp_matter_ota_requestor_init();

#if CHIP_DEVICE_CONFIG_ENABLE_THREAD
    /* Registreer de OpenThread platform-config (radio/host/port) MOET
     * voor esp_matter::start() gebeuren — anders assert s_platform_config
     * in OpenthreadLauncher.cpp. Default-macros komen uit
     * esp_openthread.h en zijn correct voor ESP32-C6 native 802.15.4. */
    esp_openthread_platform_config_t ot_cfg = {
        .radio_config = ESP_OPENTHREAD_DEFAULT_RADIO_CONFIG(),
        .host_config  = ESP_OPENTHREAD_DEFAULT_HOST_CONFIG(),
        .port_config  = ESP_OPENTHREAD_DEFAULT_PORT_CONFIG(),
    };
    set_openthread_platform_config(&ot_cfg);
#endif

    /* Start de stack */
    esp_err_t err = esp_matter::start(NULL);
    if (err != ESP_OK) { ESP_LOGE(TAG, "esp_matter::start: %d", err); return err; }

    /* Binding handler na server-start zodat FabricTable + CASESessionManager beschikbaar zijn */
    init_binding_handler();

    return ESP_OK;
}
