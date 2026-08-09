#include "config.h"

#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "config";

void config_defaults(room_config_t *cfg)
{
    memset(cfg, 0, sizeof *cfg);
    cfg->version   = 1;
    cfg->room_w_mm = 5000.0f;
    cfg->room_h_mm = 4000.0f;
    fusion_default_cfg(&cfg->fusion);
}

/* ---------- JSON ---------- */

static float json_num(const cJSON *o, const char *key, float dflt)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
    return cJSON_IsNumber(v) ? (float)v->valuedouble : dflt;
}

static bool json_bool(const cJSON *o, const char *key, bool dflt)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
    return cJSON_IsBool(v) ? cJSON_IsTrue(v) : dflt;
}

static void json_str(const cJSON *o, const char *key, char *out, size_t cap)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
    out[0] = '\0';
    if (cJSON_IsString(v) && v->valuestring) {
        strncpy(out, v->valuestring, cap - 1);
        out[cap - 1] = '\0';
    }
}

static uint8_t parse_state_mask(const cJSON *arr)
{
    uint8_t mask = 0;
    const cJSON *it = NULL;
    cJSON_ArrayForEach(it, arr) {
        if (!cJSON_IsString(it) || !it->valuestring) continue;
        if (strcmp(it->valuestring, "moving") == 0)  mask |= MOTION_MASK_MOVING;
        else if (strcmp(it->valuestring, "stopped") == 0) mask |= MOTION_MASK_STOPPED;
        else if (strcmp(it->valuestring, "any") == 0)     mask |= MOTION_MASK_ANY;
    }
    /* An empty or unrecognised filter would silently never match; default to
     * matching anything so a misconfigured zone is visibly wrong rather than
     * silently dead. */
    return mask ? mask : MOTION_MASK_ANY;
}

static count_op_t parse_count_op(const cJSON *o)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, "count_op");
    if (cJSON_IsString(v) && v->valuestring) {
        if (strcmp(v->valuestring, "==") == 0) return COUNT_OP_EQ;
        if (strcmp(v->valuestring, "<=") == 0) return COUNT_OP_LE;
    }
    return COUNT_OP_GE;
}

bool config_from_json(const char *json, size_t len, room_config_t *out)
{
    cJSON *root = cJSON_ParseWithLength(json, len);
    if (!root) {
        ESP_LOGW(TAG, "config JSON parse failed");
        return false;
    }

    config_defaults(out);
    out->version = (uint32_t)json_num(root, "version", 1);

    const cJSON *room = cJSON_GetObjectItemCaseSensitive(root, "room");
    if (cJSON_IsObject(room)) {
        out->room_w_mm = json_num(room, "w_mm", 5000.0f);
        out->room_h_mm = json_num(room, "h_mm", 4000.0f);
    }

    const cJSON *sensor = cJSON_GetObjectItemCaseSensitive(root, "sensor");
    if (cJSON_IsObject(sensor)) {
        out->sensor.x_mm      = json_num(sensor, "x_mm", 0);
        out->sensor.y_mm      = json_num(sensor, "y_mm", 0);
        out->sensor.theta_deg = json_num(sensor, "theta_deg", 0);
    }

    const cJSON *it = NULL;
    const cJSON *zones = cJSON_GetObjectItemCaseSensitive(root, "zones");
    cJSON_ArrayForEach(it, zones) {
        if (out->zone_count >= ROOM_MAX_ZONES) break;
        zone_cfg_t *z = &out->zones[out->zone_count++];
        memset(z, 0, sizeof *z);

        json_str(it, "id", z->id, ROOM_ID_LEN);
        json_str(it, "name", z->name, sizeof z->name);
        z->enabled = json_bool(it, "enabled", true);

        const cJSON *r = cJSON_GetObjectItemCaseSensitive(it, "rect");
        z->rect.cx_mm   = json_num(r, "cx", 0);
        z->rect.cy_mm   = json_num(r, "cy", 0);
        z->rect.w_mm    = json_num(r, "w", 1000);
        z->rect.h_mm    = json_num(r, "h", 1000);
        z->rect.rot_deg = json_num(r, "rot_deg", 0);

        const cJSON *tg = cJSON_GetObjectItemCaseSensitive(it, "trigger");
        z->state_mask  = parse_state_mask(
            cJSON_GetObjectItemCaseSensitive(tg, "states"));
        z->count_op    = parse_count_op(tg);
        z->count_n     = (uint8_t)json_num(tg, "count_n", 1);
        z->on_delay_ms = (uint32_t)json_num(tg, "on_delay_ms", 0);

        const cJSON *ut = cJSON_GetObjectItemCaseSensitive(it, "untrigger");
        const cJSON *mode = cJSON_GetObjectItemCaseSensitive(ut, "mode");
        z->untrigger_mode =
            (cJSON_IsString(mode) && mode->valuestring &&
             strcmp(mode->valuestring, "timer") == 0)
                ? UNTRIGGER_TIMER : UNTRIGGER_CONDITIONS_UNMET;
        z->off_delay_ms = (uint32_t)json_num(ut, "off_delay_ms", 0);
        z->max_on_ms    = (uint32_t)json_num(ut, "max_on_ms", 0);

        const cJSON *actions = cJSON_GetObjectItemCaseSensitive(it, "actions");
        const cJSON *a = NULL;
        cJSON_ArrayForEach(a, actions) {
            if (z->action_count >= ZONE_MAX_ACTIONS) break;
            zone_action_t *act = &z->actions[z->action_count++];
            act->type = ACTION_GPIO;
            act->pin          = (uint8_t)json_num(a, "pin", 0);
            act->active_level = (uint8_t)json_num(a, "active_level", 1);
            act->pulse_ms     = (uint32_t)json_num(a, "pulse_ms", 0);
            const cJSON *m = cJSON_GetObjectItemCaseSensitive(a, "mode");
            act->pulse = cJSON_IsString(m) && m->valuestring &&
                         strcmp(m->valuestring, "pulse") == 0;
        }
    }

    const cJSON *fz = cJSON_GetObjectItemCaseSensitive(root, "fusion");
    if (cJSON_IsObject(fz)) {
        out->fusion.assoc_gate_mm =
            json_num(fz, "assoc_gate_mm", out->fusion.assoc_gate_mm);
        out->fusion.coast_ms =
            (uint32_t)json_num(fz, "coast_ms", (float)out->fusion.coast_ms);
        out->fusion.moving_thresh_mms =
            json_num(fz, "moving_thresh_mms", out->fusion.moving_thresh_mms);
        out->fusion.stopped_thresh_mms =
            json_num(fz, "stopped_thresh_mms", out->fusion.stopped_thresh_mms);
        out->fusion.stopped_hold_ms =
            (uint32_t)json_num(fz, "stopped_hold_ms",
                               (float)out->fusion.stopped_hold_ms);
    }

    cJSON_Delete(root);
    return true;
}

static const char *state_mask_name(uint8_t mask)
{
    if (mask == MOTION_MASK_ANY)     return "any";
    if (mask == MOTION_MASK_MOVING)  return "moving";
    if (mask == MOTION_MASK_STOPPED) return "stopped";
    return "any";
}

size_t config_to_json(const room_config_t *cfg, char *out, size_t cap)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) return 0;

    cJSON_AddNumberToObject(root, "version", cfg->version);

    cJSON *room = cJSON_AddObjectToObject(root, "room");
    cJSON_AddNumberToObject(room, "w_mm", cfg->room_w_mm);
    cJSON_AddNumberToObject(room, "h_mm", cfg->room_h_mm);

    cJSON *sensor = cJSON_AddObjectToObject(root, "sensor");
    cJSON_AddNumberToObject(sensor, "x_mm", cfg->sensor.x_mm);
    cJSON_AddNumberToObject(sensor, "y_mm", cfg->sensor.y_mm);
    cJSON_AddNumberToObject(sensor, "theta_deg", cfg->sensor.theta_deg);

    cJSON *zones = cJSON_AddArrayToObject(root, "zones");
    for (uint8_t i = 0; i < cfg->zone_count; i++) {
        const zone_cfg_t *z = &cfg->zones[i];
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "id", z->id);
        cJSON_AddStringToObject(o, "name", z->name);
        cJSON_AddBoolToObject(o, "enabled", z->enabled);

        cJSON *r = cJSON_AddObjectToObject(o, "rect");
        cJSON_AddNumberToObject(r, "cx", z->rect.cx_mm);
        cJSON_AddNumberToObject(r, "cy", z->rect.cy_mm);
        cJSON_AddNumberToObject(r, "w",  z->rect.w_mm);
        cJSON_AddNumberToObject(r, "h",  z->rect.h_mm);
        cJSON_AddNumberToObject(r, "rot_deg", z->rect.rot_deg);

        cJSON *tg = cJSON_AddObjectToObject(o, "trigger");
        cJSON *states = cJSON_AddArrayToObject(tg, "states");
        cJSON_AddItemToArray(states,
            cJSON_CreateString(state_mask_name(z->state_mask)));
        cJSON_AddStringToObject(tg, "count_op",
            z->count_op == COUNT_OP_EQ ? "==" :
            z->count_op == COUNT_OP_LE ? "<=" : ">=");
        cJSON_AddNumberToObject(tg, "count_n", z->count_n);
        cJSON_AddNumberToObject(tg, "on_delay_ms", z->on_delay_ms);

        cJSON *ut = cJSON_AddObjectToObject(o, "untrigger");
        cJSON_AddStringToObject(ut, "mode",
            z->untrigger_mode == UNTRIGGER_TIMER ? "timer" : "conditions_unmet");
        cJSON_AddNumberToObject(ut, "off_delay_ms", z->off_delay_ms);
        cJSON_AddNumberToObject(ut, "max_on_ms", z->max_on_ms);

        cJSON *actions = cJSON_AddArrayToObject(o, "actions");
        for (uint8_t j = 0; j < z->action_count; j++) {
            const zone_action_t *a = &z->actions[j];
            cJSON *ao = cJSON_CreateObject();
            cJSON_AddStringToObject(ao, "type", "gpio");
            cJSON_AddNumberToObject(ao, "pin", a->pin);
            cJSON_AddNumberToObject(ao, "active_level", a->active_level);
            cJSON_AddStringToObject(ao, "mode", a->pulse ? "pulse" : "latch");
            cJSON_AddNumberToObject(ao, "pulse_ms", a->pulse_ms);
            cJSON_AddItemToArray(actions, ao);
        }
        cJSON_AddItemToArray(zones, o);
    }

    cJSON *fz = cJSON_AddObjectToObject(root, "fusion");
    cJSON_AddNumberToObject(fz, "assoc_gate_mm", cfg->fusion.assoc_gate_mm);
    cJSON_AddNumberToObject(fz, "coast_ms", cfg->fusion.coast_ms);
    cJSON_AddNumberToObject(fz, "moving_thresh_mms", cfg->fusion.moving_thresh_mms);
    cJSON_AddNumberToObject(fz, "stopped_thresh_mms", cfg->fusion.stopped_thresh_mms);
    cJSON_AddNumberToObject(fz, "stopped_hold_ms", cfg->fusion.stopped_hold_ms);

    char *s = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!s) return 0;

    size_t n = strlen(s);
    if (n + 1 > cap) {
        ESP_LOGW(TAG, "config JSON %u bytes exceeds %u cap", (unsigned)n, (unsigned)cap);
        cJSON_free(s);
        return 0;
    }
    memcpy(out, s, n + 1);
    cJSON_free(s);
    return n;
}

/* ---------- NVS ---------- */

bool config_load(room_config_t *cfg)
{
    nvs_handle_t h;
    if (nvs_open(CONFIG_NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
        ESP_LOGI(TAG, "no stored config; using defaults");
        config_defaults(cfg);
        return true;
    }

    size_t len = 0;
    esp_err_t err = nvs_get_blob(h, CONFIG_NVS_KEY, NULL, &len);
    if (err != ESP_OK || len == 0 || len > CONFIG_JSON_MAX) {
        nvs_close(h);
        ESP_LOGI(TAG, "no stored config; using defaults");
        config_defaults(cfg);
        return true;
    }

    char *buf = malloc(len + 1);
    if (!buf) {
        nvs_close(h);
        config_defaults(cfg);
        return false;
    }

    err = nvs_get_blob(h, CONFIG_NVS_KEY, buf, &len);
    nvs_close(h);

    bool ok = (err == ESP_OK) && config_from_json(buf, len, cfg);
    if (!ok) {
        ESP_LOGW(TAG, "stored config unreadable; falling back to defaults");
        config_defaults(cfg);
    } else {
        ESP_LOGI(TAG, "loaded config v%u: %u zones",
                 (unsigned)cfg->version, cfg->zone_count);
    }
    free(buf);
    return true;
}

bool config_save(const room_config_t *cfg)
{
    char *buf = malloc(CONFIG_JSON_MAX);
    if (!buf) return false;

    size_t n = config_to_json(cfg, buf, CONFIG_JSON_MAX);
    if (n == 0) {
        free(buf);
        return false;
    }

    nvs_handle_t h;
    if (nvs_open(CONFIG_NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) {
        free(buf);
        return false;
    }

    esp_err_t err = nvs_set_blob(h, CONFIG_NVS_KEY, buf, n);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    free(buf);

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "saved config v%u (%u bytes)", (unsigned)cfg->version, (unsigned)n);
    }
    return err == ESP_OK;
}
