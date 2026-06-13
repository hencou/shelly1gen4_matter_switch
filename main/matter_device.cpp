/*
 * Matter device implementation for Shelly 1 Gen4 (ESP32-C6).
 *
 * 5 endpoints:
 *   EP1 = OnOff Light Switch  + OnOff client + LevelControl client
 *          + ColorControl client + Binding — Toggle / Color Temp
 *   EP2 = OnOff Light Switch  + OnOff client + Binding — State-follow (On/Off)
 *   EP3 = Temperature Sensor  (server)
 *   EP4 = Occupancy Sensor    (server)
 *   EP5 = OnOff Light          (server) — physical relay, controllable from HA
 *
 * EP1 vs EP2:
 *   EP1 sends Toggle on every short press — suitable for momentary buttons.
 *   EP2 sends On on contact close and Off on contact open —
 *   suitable for maintained/toggle switches. The user chooses via
 *   binding which endpoint controls their light/relay.
 *
 * Command emit to bound nodes/groups:
 *   - app_main.c calls matter_send_onoff_toggle/on/off(ep) /
 *     matter_send_level_move/stop(ep)
 *   - data is scheduled to the CHIP thread
 *   - SwitchWorkerFunction iterates the BindingTable and sends commands
 *     directly via FindOrEstablishSession + InvokeCommandRequest.
 *
 * NB: BindingManager::NotifyBoundClusterChanged() is NOT used.
 * In esp-matter v1.4 the internal PendingNotificationMap dispatch
 * sometimes does not call the handler despite an active CASE session.
 * We bypass this by calling FindOrEstablishSession + InvokeCommandRequest
 * directly from SwitchWorkerFunction.
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
#include <app/clusters/bindings/BindingManager.h>
#include <app/OperationalSessionSetup.h>
#include <app-common/zap-generated/cluster-objects.h>
#include <controller/InvokeInteraction.h>
#include <credentials/FabricTable.h>
#include <platform/PlatformManager.h>

#if CHIP_DEVICE_CONFIG_ENABLE_THREAD
#include <platform/ESP32/OpenthreadLauncher.h>
#include "esp_openthread_types.h"

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

static uint16_t s_ep_pushbutton = 0;
static uint16_t s_ep_state   = 0;
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
    /* color-temperature payload */
    uint16_t colorTempMireds = 0;
    uint16_t colorTempRate   = 0;
};

/* Typed lambda callbacks for InvokeCommandRequest (v1.4 API).
 * - onSuccess receives the typed ResponseType (generic via auto&)
 * - onError receives only CHIP_ERROR (no more context pointer)
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

static void send_onoff_multicast(const BindingCommandData &d, const EmberBindingTableEntry &b)
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

static void send_level_multicast(const BindingCommandData &d, const EmberBindingTableEntry &b)
{
    auto *em = &chip::Server::GetInstance().GetExchangeManager();
    CHIP_ERROR err = CHIP_NO_ERROR;
    if (d.commandId == LevelControl::Commands::Move::Id) {
        LevelControl::Commands::Move::Type cmd;
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

static void send_colorcontrol_multicast(const BindingCommandData &d, const EmberBindingTableEntry &b)
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
 * Bypasses BindingManager::NotifyBoundClusterChanged() which in esp-matter
 * v1.4 sometimes does not call the registered handler despite an active
 * CASE session. Instead we use FindOrEstablishSession directly, so that
 * in the OnDeviceConnected callback we immediately send the command
 * via InvokeCommandRequest.
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
    switch_send(ep, LevelControl::Id, LevelControl::Commands::Move::Id,
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
    esp_matter::factory_reset();   /* wipes Matter NVS + reboot */
}

extern "C" uint16_t matter_ep_pushbutton(void) { return s_ep_pushbutton; }
extern "C" uint16_t matter_ep_state(void)   { return s_ep_state; }
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
    if (type != PRE_UPDATE) return ESP_OK;
    if (cluster_id != OnOff::Id || attribute_id != OnOff::Attributes::OnOff::Id)
        return ESP_OK;

    if (endpoint_id == s_ep_relay) {
        relay_set(val->val.b);
        ESP_LOGI(TAG, "EP%u OnOff -> relay %s", endpoint_id,
                 val->val.b ? "ON" : "OFF");
    }
    return ESP_OK;
}

/* Runs on the CHIP task via PlatformMgr().ScheduleWork().
 * BindingManager::Init() and the Register*Handler calls talk to
 * Server state (FabricTable / CASESessionMgr / PersistentStorage) and
 * MUST therefore run under the CHIP stack lock. From main_task this
 * is not the case -> 'Chip stack locking error ... unsafe/racy' -> chipDie.
 * Scheduling on the CHIP task resolves this deterministically. */
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
    /* Handler registration is not needed: we use DirectSendCtx
     * in SwitchWorkerFunction instead of NotifyBoundClusterChanged.
     * BindingManager::Init is still useful for loading the binding table
     * from NVS and pre-establishing CASE sessions. */
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

    /* EP1 — OnOff Light Switch + LevelControl client + ColorControl client + Binding */
    on_off_switch::config_t sw_cfg;
    endpoint_t *ep_pushbutton = on_off_switch::create(node, &sw_cfg, ENDPOINT_FLAG_NONE, NULL);
    {
        level_control::config_t lvl_cfg;
        level_control::create(ep_pushbutton, &lvl_cfg,
                              CLUSTER_FLAG_CLIENT, ESP_MATTER_NONE_FEATURE_ID);
        color_control::config_t cc_cfg;
        color_control::create(ep_pushbutton, &cc_cfg,
                              CLUSTER_FLAG_CLIENT, ESP_MATTER_NONE_FEATURE_ID);
        binding::config_t bind_cfg;
        binding::create(ep_pushbutton, &bind_cfg, CLUSTER_FLAG_SERVER);
    }
    s_ep_pushbutton = endpoint::get_id(ep_pushbutton);
    ESP_LOGI(TAG, "EP%u = OnOff Light Switch (pushbutton)", s_ep_pushbutton);

    /* EP2 — OnOff Light Switch (state-follow) + Binding
     * Same device type as EP1 but intended for maintained switches:
     * sends On on contact close, Off on contact open.
     * User binds EP2 (instead of EP1) for state-following behavior. */
    on_off_switch::config_t sf_cfg;
    endpoint_t *ep_state = on_off_switch::create(node, &sf_cfg, ENDPOINT_FLAG_NONE, NULL);
    {
        binding::config_t bind_cfg;
        binding::create(ep_state, &bind_cfg, CLUSTER_FLAG_SERVER);
    }
    s_ep_state = endpoint::get_id(ep_state);
    ESP_LOGI(TAG, "EP%u = OnOff Light Switch (state-follow)", s_ep_state);

    /* EP3 — Temperature Sensor */
    temperature_sensor::config_t t_cfg;
    endpoint_t *ep_temp = temperature_sensor::create(node, &t_cfg, ENDPOINT_FLAG_NONE, NULL);
    s_ep_temp = endpoint::get_id(ep_temp);
    ESP_LOGI(TAG, "EP%u = Temperature Sensor", s_ep_temp);

    /* EP4 — Occupancy Sensor (LD2410 on GPIO17) */
    occupancy_sensor::config_t o_cfg;
    endpoint_t *ep_occ = occupancy_sensor::create(node, &o_cfg, ENDPOINT_FLAG_NONE, NULL);
    s_ep_occ = endpoint::get_id(ep_occ);
    ESP_LOGI(TAG, "EP%u = Occupancy Sensor (LD2410)", s_ep_occ);

    /* EP5 — OnOff Light (relay on GPIO5, server — controllable from HA) */
    on_off_light::config_t relay_cfg;
    relay_cfg.on_off.on_off = relay_get();  /* restore NVS state */
    endpoint_t *ep_relay = on_off_light::create(node, &relay_cfg, ENDPOINT_FLAG_NONE, NULL);
    s_ep_relay = endpoint::get_id(ep_relay);
    ESP_LOGI(TAG, "EP%u = OnOff Light (relay)", s_ep_relay);

    /* OTA cluster requestor (optional: for Matter OTA via TBR — works alongside our WiFi OTA) */
    esp_matter_ota_requestor_init();

#if CHIP_DEVICE_CONFIG_ENABLE_THREAD
    /* Register the OpenThread platform config (radio/host/port) MUST
     * happen before esp_matter::start() — otherwise assert s_platform_config
     * in OpenthreadLauncher.cpp. Default macros come from
     * esp_openthread.h and are correct for ESP32-C6 native 802.15.4. */
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

    /* Binding handler after server start so FabricTable + CASESessionManager are available */
    init_binding_handler();

    return ESP_OK;
}
