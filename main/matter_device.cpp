/*
 * Matter device implementatie voor Shelly 1 Gen4 (ESP32-C6).
 *
 * 4 endpoints:
 *   EP1 = OnOff Light Switch  + OnOff client + LevelControl client + Binding cluster
 *   EP2 = Temperature Sensor  (server)
 *   EP3 = Occupancy Sensor    (server)
 *   EP4 = OnOff Light          (server) — fysiek relais, aanstuurbaar vanuit HA
 *
 * Commando-emit naar bound nodes/groups:
 *   - app_main.c roept matter_send_onoff_toggle(ep) / matter_send_level_move/stop(ep)
 *   - die data wordt naar de CHIP-thread gescheduled
 *   - SwitchWorkerFunction itereert de BindingTable en stuurt commando's
 *     direct via FindOrEstablishSession + InvokeCommandRequest.
 *
 * NB: BindingManager::NotifyBoundClusterChanged() wordt NIET gebruikt.
 * In esp-matter v1.4 roept de interne PendingNotificationMap dispatch
 * de handler soms niet aan ondanks een actieve CASE-sessie. We omzeilen
 * dit door FindOrEstablishSession + InvokeCommandRequest direct aan te
 * roepen vanuit SwitchWorkerFunction.
 */

#include "matter_device.h"

extern "C" {
#include "app_config.h"
#include "relay.h"
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
#include <app/OperationalSessionSetup.h>
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
static uint16_t s_ep_relay   = 0;


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

/* ------------- Multicast helpers (geen CASE-sessie nodig) ------------- */

static void send_onoff_multicast(const BindingCommandData &d, const EmberBindingTableEntry &b)
{
    if (d.commandId == OnOff::Commands::Toggle::Id) {
        OnOff::Commands::Toggle::Type cmd;
        chip::Controller::InvokeGroupCommandRequest(nullptr, b.fabricIndex, b.groupId, cmd);
    }
}

static void send_level_multicast(const BindingCommandData &d, const EmberBindingTableEntry &b)
{
    if (d.commandId == LevelControl::Commands::Move::Id) {
        LevelControl::Commands::Move::Type cmd;
        cmd.moveMode = (d.moveMode == 0) ? LevelControl::MoveModeEnum::kUp
                                         : LevelControl::MoveModeEnum::kDown;
        cmd.rate.SetNonNull(d.rate);
        chip::Controller::InvokeGroupCommandRequest(nullptr, b.fabricIndex, b.groupId, cmd);
    } else if (d.commandId == LevelControl::Commands::Stop::Id) {
        LevelControl::Commands::Stop::Type cmd;
        chip::Controller::InvokeGroupCommandRequest(nullptr, b.fabricIndex, b.groupId, cmd);
    }
}

/* ------------- Direct-send via FindOrEstablishSession ------------- */
/*
 * Omzeilt BindingManager::NotifyBoundClusterChanged() die in esp-matter
 * v1.4 de registered handler soms niet aanroept ondanks een actieve
 * CASE-sessie. In plaats daarvan gebruiken we FindOrEstablishSession
 * direct, zodat we bij de OnDeviceConnected callback meteen het commando
 * versturen via InvokeCommandRequest.
 */
struct DirectSendCtx {
    BindingCommandData cmd;
    EmberBindingTableEntry entry;
    chip::Callback::Callback<chip::OnDeviceConnected> connCb;
    chip::Callback::Callback<chip::OnDeviceConnectionFailure> failCb;

    DirectSendCtx(const BindingCommandData &d, const EmberBindingTableEntry &e)
        : cmd(d), entry(e),
          connCb(OnConn, this), failCb(OnFail, this) {}

    static void OnConn(void *raw, chip::Messaging::ExchangeManager &em,
                       const chip::SessionHandle &sh)
    {
        auto *self = static_cast<DirectSendCtx *>(raw);
        auto &d = self->cmd;
        auto &b = self->entry;
        ESP_LOGI(TAG, "DirectSend: session ready -> remote ep %u cluster 0x%lx cmd 0x%lx",
                 b.remote, (unsigned long)d.clusterId, (unsigned long)d.commandId);

        if (d.clusterId == OnOff::Id) {
            if (d.commandId == OnOff::Commands::Toggle::Id) {
                OnOff::Commands::Toggle::Type c;
                chip::Controller::InvokeCommandRequest(
                    &em, sh, b.remote, c, make_on_success(), make_on_error());
            }
        } else if (d.clusterId == LevelControl::Id) {
            if (d.commandId == LevelControl::Commands::Move::Id) {
                LevelControl::Commands::Move::Type c;
                c.moveMode = (d.moveMode == 0) ? LevelControl::MoveModeEnum::kUp
                                               : LevelControl::MoveModeEnum::kDown;
                c.rate.SetNonNull(d.rate);
                chip::Controller::InvokeCommandRequest(
                    &em, sh, b.remote, c, make_on_success(), make_on_error());
            } else if (d.commandId == LevelControl::Commands::Stop::Id) {
                LevelControl::Commands::Stop::Type c;
                chip::Controller::InvokeCommandRequest(
                    &em, sh, b.remote, c, make_on_success(), make_on_error());
            }
        }
        chip::Platform::Delete(self);
    }

    static void OnFail(void *raw, const chip::ScopedNodeId &peer, CHIP_ERROR err)
    {
        ESP_LOGE(TAG, "DirectSend: CASE failed [%u:0x%llx]: %" CHIP_ERROR_FORMAT,
                 peer.GetFabricIndex(),
                 (unsigned long long)peer.GetNodeId(),
                 err.Format());
        chip::Platform::Delete(static_cast<DirectSendCtx *>(raw));
    }
};

static void SwitchWorkerFunction(intptr_t context)
{
    BindingCommandData *d = reinterpret_cast<BindingCommandData *>(context);
    uint32_t sent  = 0;
    uint32_t total = 0;

    for (const auto & e : chip::BindingTable::GetInstance()) {
        total++;
        ESP_LOGI(TAG, "BindingTable[%lu]: type=%u local=%u remote=%u "
                 "nodeId=0x%llx group=%u cluster=0x%lx fabric=%u",
                 (unsigned long) total,
                 (unsigned) e.type, e.local, e.remote,
                 (unsigned long long) e.nodeId,
                 (unsigned) e.groupId,
                 (unsigned long) e.clusterId.value_or(0),
                 (unsigned) e.fabricIndex);

        if (e.local != d->localEndpointId) continue;
        if (e.clusterId.has_value() && e.clusterId.value() != d->clusterId) continue;

        if (e.type == MATTER_UNICAST_BINDING) {
            auto *ctx = chip::Platform::New<DirectSendCtx>(*d, e);
            if (!ctx) {
                ESP_LOGE(TAG, "SwitchWorker: OOM DirectSendCtx");
                continue;
            }
            chip::ScopedNodeId peer(e.nodeId, e.fabricIndex);
            ESP_LOGI(TAG, "SwitchWorker: FindOrEstablishSession [%u:0x%llx]",
                     peer.GetFabricIndex(),
                     (unsigned long long) peer.GetNodeId());
            chip::Server::GetInstance().GetCASESessionManager()->
                FindOrEstablishSession(peer, &ctx->connCb, &ctx->failCb);
            sent++;
        } else if (e.type == MATTER_MULTICAST_BINDING) {
            if (d->clusterId == OnOff::Id)            send_onoff_multicast(*d, e);
            else if (d->clusterId == LevelControl::Id) send_level_multicast(*d, e);
            sent++;
        }
    }

    ESP_LOGI(TAG, "SwitchWorker: ep=%u cluster=0x%lx cmd=0x%lx total=%lu sent=%lu",
             d->localEndpointId, (unsigned long) d->clusterId,
             (unsigned long) d->commandId, (unsigned long) total, (unsigned long) sent);
    if (sent == 0) {
        ESP_LOGW(TAG, "SwitchWorker: geen matching binding entries voor ep=%u",
                 d->localEndpointId);
    }
    chip::Platform::Delete(d);
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
extern "C" uint16_t matter_ep_relay(void)   { return s_ep_relay; }


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
    if (type == PRE_UPDATE && endpoint_id == s_ep_relay) {
        if (cluster_id == OnOff::Id &&
            attribute_id == OnOff::Attributes::OnOff::Id) {
            relay_set(val->val.b);
            ESP_LOGI(TAG, "EP%u OnOff -> relay %s", endpoint_id,
                     val->val.b ? "ON" : "OFF");
        }
    }
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
    /* Handler-registratie is niet nodig: we gebruiken DirectSendCtx
     * in SwitchWorkerFunction i.p.v. NotifyBoundClusterChanged.
     * BindingManager::Init is nog steeds nuttig voor het laden van de
     * binding table uit NVS en het pre-establischen van CASE sessies. */
    ESP_LOGI(TAG, "BindingManager initialised on CHIP-task");
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

    /* EP3 — Occupancy Sensor (LD2410 op GPIO17) */
    occupancy_sensor::config_t o_cfg;
    endpoint_t *ep_occ = occupancy_sensor::create(node, &o_cfg, ENDPOINT_FLAG_NONE, NULL);
    s_ep_occ = endpoint::get_id(ep_occ);
    ESP_LOGI(TAG, "EP%u = Occupancy Sensor (LD2410)", s_ep_occ);

    /* EP4 — OnOff Light (relais op GPIO5, server — aanstuurbaar vanuit HA) */
    on_off_light::config_t relay_cfg;
    relay_cfg.on_off.on_off = relay_get();  /* herstel NVS-state */
    endpoint_t *ep_relay = on_off_light::create(node, &relay_cfg, ENDPOINT_FLAG_NONE, NULL);
    s_ep_relay = endpoint::get_id(ep_relay);
    ESP_LOGI(TAG, "EP%u = OnOff Light (relais)", s_ep_relay);

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
