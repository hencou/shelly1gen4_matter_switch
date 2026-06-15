/*
 * Matter device implementation for Shelly 1 Gen4 (ESP32-C6).
 *
 * Endpoints are created dynamically based on script slot configuration.
 * Each script slot with a non-NONE type gets a corresponding Matter endpoint.
 * Supported endpoint types:
 *   ONOFF_TOGGLE  → OnOff Light Switch + LevelControl + ColorControl + Binding
 *   ONOFF_STATE   → OnOff Light Switch + Binding (state-follow)
 *   TEMPERATURE   → Temperature Sensor (server)
 *   OCCUPANCY     → Occupancy Sensor (server)
 *   RELAY         → OnOff Light (server, physical relay)
 *
 * Command emit to bound nodes/groups:
 *   - Scripts call endpoint.command("toggle") etc.
 *   - Data is scheduled to the CHIP thread
 *   - SwitchWorkerFunction iterates the BindingTable and sends commands
 *     directly via FindOrEstablishSession + InvokeCommandRequest.
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
#include <app/clusters/bindings/binding-table.h>
#include <app/OperationalSessionSetup.h>
#include <app-common/zap-generated/cluster-objects.h>
#include <controller/InvokeInteraction.h>
#include <credentials/FabricTable.h>
#include <platform/PlatformManager.h>
#include <platform/ThreadStackManager.h>

#if CHIP_DEVICE_CONFIG_ENABLE_THREAD
#include <platform/ESP32/OpenthreadLauncher.h>
#include "esp_openthread_types.h"
#if CONFIG_OPENTHREAD_BORDER_ROUTER
#include "esp_openthread_border_router.h"
#endif

/* Default config macros are NOT provided by esp_openthread.h in esp-matter/
 * esp-idf — canonical Shelly Thread firmware defines them locally. Same
 * values here: native 802.15.4 radio (ESP32-C6), no host bridge,
 * NVS partition for SRP key storage. */
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

/* Dynamic endpoint tracking — indexed by script slot */
static uint16_t s_slot_endpoints[SCRIPT_MAX_SLOTS] = {0};
static script_slot_type_t s_slot_types[SCRIPT_MAX_SLOTS] = {SLOT_TYPE_NONE};
static uint8_t s_num_slots = 0;


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
    /* color-temperature payload */
    uint16_t colorTempMireds = 0;
    uint16_t colorTempRate   = 0;
};

/* Typed lambda callbacks for InvokeCommandRequest.
 * - onSuccess receives the typed ResponseType (generic via auto&)
 * - onError receives only CHIP_ERROR
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

/* ------------- Multicast helpers (no CASE session needed) ------------- */

static void send_onoff_multicast(const BindingCommandData &d, const Binding::TableEntry &b)
{
    auto *em = &chip::Server::GetInstance().GetExchangeManager();
    CHIP_ERROR err = CHIP_NO_ERROR;
    if (d.commandId == OnOff::Commands::Toggle::Id) {
        OnOff::Commands::Toggle::Type cmd;
        err = chip::Controller::InvokeGroupCommandRequest(em, b.fabricIndex, b.groupId, cmd);
    } else if (d.commandId == OnOff::Commands::On::Id) {
        OnOff::Commands::On::Type cmd;
        err = chip::Controller::InvokeGroupCommandRequest(em, b.fabricIndex, b.groupId, cmd);
    } else if (d.commandId == OnOff::Commands::Off::Id) {
        OnOff::Commands::Off::Type cmd;
        err = chip::Controller::InvokeGroupCommandRequest(em, b.fabricIndex, b.groupId, cmd);
    }
    if (err != CHIP_NO_ERROR) {
        ESP_LOGE(TAG, "send_onoff_multicast FAILED: fabric=%u group=0x%04X cmd=0x%lx err=%" CHIP_ERROR_FORMAT,
                 b.fabricIndex, b.groupId, (unsigned long)d.commandId, err.Format());
    } else {
        ESP_LOGI(TAG, "send_onoff_multicast OK: fabric=%u group=0x%04X cmd=0x%lx",
                 b.fabricIndex, b.groupId, (unsigned long)d.commandId);
    }
}

static void send_level_multicast(const BindingCommandData &d, const Binding::TableEntry &b)
{
    auto *em = &chip::Server::GetInstance().GetExchangeManager();
    CHIP_ERROR err = CHIP_NO_ERROR;
    if (d.commandId == LevelControl::Commands::MoveWithOnOff::Id) {
        LevelControl::Commands::MoveWithOnOff::Type cmd;
        cmd.moveMode = (d.moveMode == 0) ? LevelControl::MoveModeEnum::kUp
                                         : LevelControl::MoveModeEnum::kDown;
        cmd.rate.SetNonNull(d.rate);
        err = chip::Controller::InvokeGroupCommandRequest(em, b.fabricIndex, b.groupId, cmd);
    } else if (d.commandId == LevelControl::Commands::Stop::Id) {
        LevelControl::Commands::Stop::Type cmd;
        err = chip::Controller::InvokeGroupCommandRequest(em, b.fabricIndex, b.groupId, cmd);
    }
    if (err != CHIP_NO_ERROR) {
        ESP_LOGE(TAG, "send_level_multicast FAILED: fabric=%u group=0x%04X err=%" CHIP_ERROR_FORMAT,
                 b.fabricIndex, b.groupId, err.Format());
    } else {
        ESP_LOGI(TAG, "send_level_multicast OK: fabric=%u group=0x%04X",
                 b.fabricIndex, b.groupId);
    }
}

static void send_colorcontrol_multicast(const BindingCommandData &d, const Binding::TableEntry &b)
{
    auto *em = &chip::Server::GetInstance().GetExchangeManager();
    CHIP_ERROR err = CHIP_NO_ERROR;
    if (d.commandId == ColorControl::Commands::MoveToColorTemperature::Id) {
        ColorControl::Commands::MoveToColorTemperature::Type cmd;
        cmd.colorTemperatureMireds = d.colorTempMireds;
        cmd.transitionTime = 0;
        err = chip::Controller::InvokeGroupCommandRequest(em, b.fabricIndex, b.groupId, cmd);
    } else if (d.commandId == ColorControl::Commands::MoveColorTemperature::Id) {
        ColorControl::Commands::MoveColorTemperature::Type cmd;
        cmd.moveMode = (d.moveMode == 0)
            ? ColorControl::HueMoveMode::kUp
            : ColorControl::HueMoveMode::kDown;
        cmd.rate = d.colorTempRate;
        cmd.colorTemperatureMinimumMireds = 0;
        cmd.colorTemperatureMaximumMireds = 0;
        err = chip::Controller::InvokeGroupCommandRequest(em, b.fabricIndex, b.groupId, cmd);
    } else if (d.commandId == ColorControl::Commands::StopMoveStep::Id) {
        ColorControl::Commands::StopMoveStep::Type cmd;
        err = chip::Controller::InvokeGroupCommandRequest(em, b.fabricIndex, b.groupId, cmd);
    }
    if (err != CHIP_NO_ERROR) {
        ESP_LOGE(TAG, "send_colorcontrol_multicast FAILED: fabric=%u group=0x%04X err=%" CHIP_ERROR_FORMAT,
                 b.fabricIndex, b.groupId, err.Format());
    } else {
        ESP_LOGI(TAG, "send_colorcontrol_multicast OK: fabric=%u group=0x%04X cmd=0x%lx",
                 b.fabricIndex, b.groupId, (unsigned long)d.commandId);
    }
}

/* ------------- Direct-send via FindOrEstablishSession ------------- */
/*
 * Uses FindOrEstablishSession directly for unicast commands, so that
 * in the OnDeviceConnected callback we immediately send the command
 * via InvokeCommandRequest.
 */
struct DirectSendCtx {
    BindingCommandData cmd;
    Binding::TableEntry entry;
    chip::Callback::Callback<chip::OnDeviceConnected> connCb;
    chip::Callback::Callback<chip::OnDeviceConnectionFailure> failCb;

    DirectSendCtx(const BindingCommandData &d, const Binding::TableEntry &e)
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
            } else if (d.commandId == OnOff::Commands::On::Id) {
                OnOff::Commands::On::Type c;
                chip::Controller::InvokeCommandRequest(
                    &em, sh, b.remote, c, make_on_success(), make_on_error());
            } else if (d.commandId == OnOff::Commands::Off::Id) {
                OnOff::Commands::Off::Type c;
                chip::Controller::InvokeCommandRequest(
                    &em, sh, b.remote, c, make_on_success(), make_on_error());
            }
        } else if (d.clusterId == LevelControl::Id) {
            if (d.commandId == LevelControl::Commands::MoveWithOnOff::Id) {
                LevelControl::Commands::MoveWithOnOff::Type c;
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
        } else if (d.clusterId == ColorControl::Id) {
            if (d.commandId == ColorControl::Commands::MoveToColorTemperature::Id) {
                ColorControl::Commands::MoveToColorTemperature::Type c;
                c.colorTemperatureMireds = d.colorTempMireds;
                c.transitionTime = 0;
                chip::Controller::InvokeCommandRequest(
                    &em, sh, b.remote, c, make_on_success(), make_on_error());
            } else if (d.commandId == ColorControl::Commands::MoveColorTemperature::Id) {
                ColorControl::Commands::MoveColorTemperature::Type c;
                c.moveMode = (d.moveMode == 0)
                    ? ColorControl::HueMoveMode::kUp
                    : ColorControl::HueMoveMode::kDown;
                c.rate = d.colorTempRate;
                c.colorTemperatureMinimumMireds = 0;
                c.colorTemperatureMaximumMireds = 0;
                chip::Controller::InvokeCommandRequest(
                    &em, sh, b.remote, c, make_on_success(), make_on_error());
            } else if (d.commandId == ColorControl::Commands::StopMoveStep::Id) {
                ColorControl::Commands::StopMoveStep::Type c;
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

    for (const auto & e : Binding::Table::GetInstance()) {
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

        if (e.type == Binding::MATTER_UNICAST_BINDING) {
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
        } else if (e.type == Binding::MATTER_MULTICAST_BINDING) {
            if (d->clusterId == OnOff::Id)              send_onoff_multicast(*d, e);
            else if (d->clusterId == LevelControl::Id)  send_level_multicast(*d, e);
            else if (d->clusterId == ColorControl::Id)  send_colorcontrol_multicast(*d, e);
            sent++;
        }
    }

    ESP_LOGI(TAG, "SwitchWorker: ep=%u cluster=0x%lx cmd=0x%lx total=%lu sent=%lu",
             d->localEndpointId, (unsigned long) d->clusterId,
             (unsigned long) d->commandId, (unsigned long) total, (unsigned long) sent);
    if (sent == 0) {
        ESP_LOGW(TAG, "SwitchWorker: no matching binding entries for ep=%u",
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

/* Guard: ep == 0 means the corresponding endpoint has not yet been created
 * by matter_start(). Defensive: early return so that spurious ISR callbacks
 * right after boot do not crash into chip:: code. */
extern "C" void matter_send_onoff_toggle(uint16_t ep)
{
    if (!ep) return;
    switch_send(ep, OnOff::Id, OnOff::Commands::Toggle::Id);
}

extern "C" void matter_send_onoff_on(uint16_t ep)
{
    if (!ep) return;
    switch_send(ep, OnOff::Id, OnOff::Commands::On::Id);
}

extern "C" void matter_send_onoff_off(uint16_t ep)
{
    if (!ep) return;
    switch_send(ep, OnOff::Id, OnOff::Commands::Off::Id);
}

extern "C" void matter_send_level_move(uint16_t ep, bool up, uint8_t rate)
{
    if (!ep) return;
    switch_send(ep, LevelControl::Id, LevelControl::Commands::MoveWithOnOff::Id,
                up ? 0 : 1, rate);
}

extern "C" void matter_send_level_stop(uint16_t ep)
{
    if (!ep) return;
    switch_send(ep, LevelControl::Id, LevelControl::Commands::Stop::Id);
}

extern "C" void matter_send_color_temp_set(uint16_t ep, uint16_t mireds)
{
    if (!ep) return;
    ESP_LOGI(TAG, "color_temp_set ep=%u mireds=%u", ep, mireds);
    BindingCommandData *d = chip::Platform::New<BindingCommandData>();
    d->localEndpointId = ep;
    d->clusterId       = ColorControl::Id;
    d->commandId       = ColorControl::Commands::MoveToColorTemperature::Id;
    d->colorTempMireds = mireds;
    chip::DeviceLayer::PlatformMgr().ScheduleWork(
        SwitchWorkerFunction, reinterpret_cast<intptr_t>(d));
}

extern "C" void matter_send_color_temp_move(uint16_t ep, bool warmer, uint16_t rate)
{
    if (!ep) return;
    ESP_LOGI(TAG, "color_temp_move ep=%u warmer=%d rate=%u", ep, warmer, rate);
    BindingCommandData *d = chip::Platform::New<BindingCommandData>();
    d->localEndpointId = ep;
    d->clusterId       = ColorControl::Id;
    d->commandId       = ColorControl::Commands::MoveColorTemperature::Id;
    d->moveMode        = warmer ? 0 : 1;  /* 0=Up(warmer/higher mireds), 1=Down(cooler) */
    d->colorTempRate   = rate;
    chip::DeviceLayer::PlatformMgr().ScheduleWork(
        SwitchWorkerFunction, reinterpret_cast<intptr_t>(d));
}

extern "C" void matter_send_color_temp_stop(uint16_t ep)
{
    if (!ep) return;
    ESP_LOGI(TAG, "color_temp_stop ep=%u", ep);
    BindingCommandData *d = chip::Platform::New<BindingCommandData>();
    d->localEndpointId = ep;
    d->clusterId       = ColorControl::Id;
    d->commandId       = ColorControl::Commands::StopMoveStep::Id;
    chip::DeviceLayer::PlatformMgr().ScheduleWork(
        SwitchWorkerFunction, reinterpret_cast<intptr_t>(d));
}

extern "C" void matter_update_temperature(int16_t centi_c)
{
    esp_matter_attr_val_t v = esp_matter_int16(centi_c);
    for (int i = 0; i < s_num_slots; i++) {
        if (s_slot_types[i] == SLOT_TYPE_TEMPERATURE && s_slot_endpoints[i]) {
            attribute::update(s_slot_endpoints[i],
                TemperatureMeasurement::Id,
                TemperatureMeasurement::Attributes::MeasuredValue::Id, &v);
        }
    }
}

extern "C" void matter_update_occupancy(bool occupied)
{
    uint8_t b = occupied ? 1 : 0;
    esp_matter_attr_val_t v = esp_matter_bitmap8(b);
    for (int i = 0; i < s_num_slots; i++) {
        if (s_slot_types[i] == SLOT_TYPE_OCCUPANCY && s_slot_endpoints[i]) {
            attribute::update(s_slot_endpoints[i],
                OccupancySensing::Id,
                OccupancySensing::Attributes::Occupancy::Id, &v);
        }
    }
}

extern "C" void matter_update_relay_onoff(bool on)
{
    esp_matter_attr_val_t v = esp_matter_bool(on);
    for (int i = 0; i < s_num_slots; i++) {
        if (s_slot_types[i] == SLOT_TYPE_RELAY && s_slot_endpoints[i]) {
            attribute::update(s_slot_endpoints[i],
                OnOff::Id,
                OnOff::Attributes::OnOff::Id, &v);
        }
    }
}

extern "C" void matter_disable_thread(void)
{
#if CHIP_DEVICE_CONFIG_ENABLE_THREAD
    ESP_LOGW(TAG, "Disabling Thread to free 2.4 GHz radio for WiFi");
    chip::DeviceLayer::ThreadStackMgr().SetThreadEnabled(false);
#endif
}

extern "C" void matter_set_tbr_backbone(void *wifi_sta_netif)
{
#if CONFIG_OPENTHREAD_BORDER_ROUTER
    ESP_LOGI(TAG, "Setting WiFi STA as TBR backbone netif");
    esp_openthread_set_backbone_netif((esp_netif_t *)wifi_sta_netif);
#else
    ESP_LOGW(TAG, "TBR not compiled in (CONFIG_OPENTHREAD_BORDER_ROUTER=n)");
    (void)wifi_sta_netif;
#endif
}

extern "C" esp_err_t matter_tbr_init(void)
{
#if CONFIG_OPENTHREAD_BORDER_ROUTER
    ESP_LOGI(TAG, "Initializing Thread Border Router");
    esp_err_t err = esp_openthread_border_router_init();
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Thread Border Router initialized successfully");
    } else {
        ESP_LOGE(TAG, "Thread Border Router init failed: %d", err);
    }
    return err;
#else
    ESP_LOGW(TAG, "TBR not compiled in (CONFIG_OPENTHREAD_BORDER_ROUTER=n)");
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

extern "C" void matter_factory_reset(void)
{
    ESP_LOGW(TAG, "factory reset requested");
    esp_matter::factory_reset();   /* wipes Matter NVS + reboot */
}

extern "C" uint16_t matter_get_slot_endpoint(uint8_t slot)
{
    if (slot >= s_num_slots) return 0;
    return s_slot_endpoints[slot];
}


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
    if (type != PRE_UPDATE) return ESP_OK;
    if (cluster_id != OnOff::Id || attribute_id != OnOff::Attributes::OnOff::Id)
        return ESP_OK;

    /* Check if this endpoint is a relay slot */
    for (int i = 0; i < s_num_slots; i++) {
        if (s_slot_types[i] == SLOT_TYPE_RELAY && s_slot_endpoints[i] == endpoint_id) {
            relay_set(val->val.b);
            ESP_LOGI(TAG, "EP%u OnOff -> relay %s", endpoint_id,
                     val->val.b ? "ON" : "OFF");
            break;
        }
    }
    return ESP_OK;
}

/* BindingManager is initialized automatically by esp_matter in v1.5
 * (client::binding_manager_init() called on kDnssdInitialized event).
 * No manual init needed here. */

static endpoint_t *create_endpoint_for_type(node_t *node, script_slot_type_t type)
{
    switch (type) {
    case SLOT_TYPE_ONOFF_TOGGLE:
    case SLOT_TYPE_DIMMER: {
        /* OnOff Light Switch + LevelControl + ColorControl + Binding */
        on_off_light_switch::config_t cfg;
        endpoint_t *ep = on_off_light_switch::create(node, &cfg, ENDPOINT_FLAG_NONE, NULL);
        level_control::config_t lvl_cfg;
        level_control::create(ep, &lvl_cfg, CLUSTER_FLAG_CLIENT);
        color_control::config_t cc_cfg;
        color_control::create(ep, &cc_cfg, CLUSTER_FLAG_CLIENT);
        binding::config_t bind_cfg;
        binding::create(ep, &bind_cfg, CLUSTER_FLAG_SERVER);
        return ep;
    }
    case SLOT_TYPE_ONOFF_STATE: {
        /* OnOff Light Switch + Binding (state-follow) */
        on_off_light_switch::config_t cfg;
        endpoint_t *ep = on_off_light_switch::create(node, &cfg, ENDPOINT_FLAG_NONE, NULL);
        binding::config_t bind_cfg;
        binding::create(ep, &bind_cfg, CLUSTER_FLAG_SERVER);
        return ep;
    }
    case SLOT_TYPE_TEMPERATURE: {
        temperature_sensor::config_t cfg;
        return temperature_sensor::create(node, &cfg, ENDPOINT_FLAG_NONE, NULL);
    }
    case SLOT_TYPE_OCCUPANCY: {
        occupancy_sensor::config_t cfg;
        cfg.occupancy_sensing.feature_flags =
            cluster::occupancy_sensing::feature::other::get_id();
        return occupancy_sensor::create(node, &cfg, ENDPOINT_FLAG_NONE, NULL);
    }
    case SLOT_TYPE_RELAY: {
        on_off_light::config_t cfg;
        cfg.on_off.on_off = relay_get();
        return on_off_light::create(node, &cfg, ENDPOINT_FLAG_NONE, NULL);
    }
    case SLOT_TYPE_ILLUMINANCE:
    default:
        return NULL;
    }
}

static const char *slot_type_name(script_slot_type_t type)
{
    switch (type) {
    case SLOT_TYPE_ONOFF_TOGGLE: return "OnOff Toggle+Dim+Color (client)";
    case SLOT_TYPE_DIMMER:       return "OnOff Toggle+Dim+Color (client)";
    case SLOT_TYPE_ONOFF_STATE:  return "OnOff State-Follow (client)";
    case SLOT_TYPE_TEMPERATURE:  return "Temperature Sensor";
    case SLOT_TYPE_OCCUPANCY:    return "Occupancy Sensor";
    case SLOT_TYPE_RELAY:        return "OnOff Light (relay)";
    case SLOT_TYPE_ILLUMINANCE:  return "Illuminance Sensor";
    default:                     return "Unknown";
    }
}

extern "C" esp_err_t matter_start(const script_slot_type_t *slot_types, uint8_t num_slots)
{
    /* Store slot types for later use by update functions */
    s_num_slots = (num_slots > SCRIPT_MAX_SLOTS) ? SCRIPT_MAX_SLOTS : num_slots;
    for (int i = 0; i < s_num_slots; i++) {
        s_slot_types[i] = slot_types[i];
        s_slot_endpoints[i] = 0;
    }

    /* Node (root) */
    node::config_t node_cfg;
    node_t *node = node::create(&node_cfg, attribute_update_cb, identify_cb);
    if (!node) { ESP_LOGE(TAG, "node create failed"); return ESP_FAIL; }

    /* Create endpoints dynamically based on slot configuration */
    for (int i = 0; i < s_num_slots; i++) {
        if (slot_types[i] == SLOT_TYPE_NONE) continue;

        endpoint_t *ep = create_endpoint_for_type(node, slot_types[i]);
        if (ep) {
            s_slot_endpoints[i] = endpoint::get_id(ep);
            ESP_LOGI(TAG, "Slot %d: EP%u = %s", i, s_slot_endpoints[i],
                     slot_type_name(slot_types[i]));
        } else {
            ESP_LOGW(TAG, "Slot %d: failed to create endpoint for type %d", i, slot_types[i]);
        }
    }

    /* OTA cluster requestor */
    esp_matter_ota_requestor_init();

#if CHIP_DEVICE_CONFIG_ENABLE_THREAD
    esp_openthread_platform_config_t ot_cfg = {
        .radio_config = ESP_OPENTHREAD_DEFAULT_RADIO_CONFIG(),
        .host_config  = ESP_OPENTHREAD_DEFAULT_HOST_CONFIG(),
        .port_config  = ESP_OPENTHREAD_DEFAULT_PORT_CONFIG(),
    };
    set_openthread_platform_config(&ot_cfg);
#endif

    /* Start the stack */
    esp_err_t err = esp_matter::start(NULL);
    if (err != ESP_OK) { ESP_LOGE(TAG, "esp_matter::start: %d", err); return err; }

    return ESP_OK;
}
