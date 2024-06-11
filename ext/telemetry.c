#include "ddtrace.h"
#include "configuration.h"
#include "integrations/integrations.h"
#include <hook/hook.h>
#include <components-rs/ddtrace.h>
#include "telemetry.h"
#include "sidecar.h"

ZEND_EXTERN_MODULE_GLOBALS(ddtrace);

zend_long dd_composer_hook_id;

static void dd_commit_metrics(ddog_SidecarActionsBuffer *sca_buffer);

static bool dd_check_for_composer_autoloader(zend_ulong invocation, zend_execute_data *execute_data, void *auxiliary, void *dynamic) {
    UNUSED(invocation, auxiliary, dynamic);

    ddog_CharSlice composer_path = dd_zend_string_to_CharSlice(execute_data->func->op_array.filename);
    if (!ddtrace_sidecar // if sidecar connection was broken, let's skip immediately
        || ddtrace_detect_composer_installed_json(&ddtrace_sidecar, ddtrace_sidecar_instance_id, &DDTRACE_G(telemetry_queue_id), composer_path)) {
        zai_hook_remove((zai_str)ZAI_STR_EMPTY, (zai_str)ZAI_STR_EMPTY, dd_composer_hook_id);
    }
    return true;
}

void ddtrace_telemetry_first_init(void) {
    dd_composer_hook_id = zai_hook_install((zai_str)ZAI_STR_EMPTY, (zai_str)ZAI_STR_EMPTY, dd_check_for_composer_autoloader, NULL, ZAI_HOOK_AUX_UNUSED, 0);
}

void ddtrace_telemetry_rinit(void) {
    zend_hash_init(&DDTRACE_G(telemetry_spans_created_per_integration), 8, unused, NULL, 0);
}

void ddtrace_telemetry_rshutdown(void) {
    zend_hash_destroy(&DDTRACE_G(telemetry_spans_created_per_integration));
}

void ddtrace_telemetry_finalize(void) {
    if (!ddtrace_sidecar || !get_global_DD_INSTRUMENTATION_TELEMETRY_ENABLED()) {
        return;
    }

    ddog_SidecarActionsBuffer *buffer = ddog_sidecar_telemetry_buffer_alloc();

    zend_module_entry *module;
    char module_name[261] = { 'e', 'x', 't', '-' };
    ZEND_HASH_FOREACH_PTR(&module_registry, module) {
        size_t namelen = strlen(module->name);
        memcpy(module_name + 4, module->name, MIN(256, strlen(module->name)));
        const char *version = module->version ? module->version : "";
        ddog_sidecar_telemetry_addDependency_buffer(buffer,
                                                    (ddog_CharSlice) {.len = namelen + 4, .ptr = module_name},
                                                    (ddog_CharSlice) {.len = strlen(version), .ptr = version});
    } ZEND_HASH_FOREACH_END();

    for (uint8_t i = 0; i < zai_config_memoized_entries_count; i++) {
        zai_config_memoized_entry *cfg = &zai_config_memoized_entries[i];
        zend_ini_entry *ini = cfg->ini_entries[0];
#if ZTS
        ini = zend_hash_find_ptr(EG(ini_directives), ini->name);
#endif
        if (!zend_string_equals_literal(ini->name, "datadog.trace.enabled")) { // datadog.trace.enabled is meaningless: always off at rshutdown
            ddog_ConfigurationOrigin origin = cfg->name_index == -1 ? DDOG_CONFIGURATION_ORIGIN_DEFAULT : DDOG_CONFIGURATION_ORIGIN_ENV_VAR;
            if (!zend_string_equals_cstr(ini->value, cfg->default_encoded_value.ptr, cfg->default_encoded_value.len)) {
                origin = cfg->name_index >= 0 ? DDOG_CONFIGURATION_ORIGIN_ENV_VAR : DDOG_CONFIGURATION_ORIGIN_CODE;
            }
            ddog_CharSlice name = dd_zend_string_to_CharSlice(ini->name);
            name.len -= strlen("datadog.");
            name.ptr += strlen("datadog.");
            ddog_sidecar_telemetry_enqueueConfig_buffer(buffer, name, dd_zend_string_to_CharSlice(ini->value), origin);
        }
    }

    // Send information about explicitly disabled integrations
    for (size_t i = 0; i < ddtrace_integrations_len; ++i) {
        ddtrace_integration *integration = &ddtrace_integrations[i];
        if (!integration->is_enabled()) {
            ddog_CharSlice integration_name = (ddog_CharSlice) {.len = integration->name_len, .ptr = integration->name_lcase};
            ddog_sidecar_telemetry_addIntegration_buffer(buffer, integration_name, DDOG_CHARSLICE_C(""), false);
        }
    }

    // Telemetry metrics
    ddog_CharSlice metric_name = DDOG_CHARSLICE_C("spans_created");
    ddog_sidecar_telemetry_register_metric_buffer(buffer, metric_name, DDOG_METRIC_TYPE_COUNT, DDOG_METRIC_NAMESPACE_TRACERS);
    zend_string *integration_name;
    zval *metric_value;
    ZEND_HASH_FOREACH_STR_KEY_VAL(&DDTRACE_G(telemetry_spans_created_per_integration), integration_name, metric_value) {
        zai_string tags = zai_string_concat3((zai_str)ZAI_STRL("integration_name:"), (zai_str)ZAI_STR_FROM_ZSTR(integration_name), (zai_str)ZAI_STRING_EMPTY);
        ddog_sidecar_telemetry_add_span_metric_point_buffer(buffer, metric_name, Z_DVAL_P(metric_value), dd_zai_string_to_CharSlice(tags));
        zai_string_destroy(&tags);
    } ZEND_HASH_FOREACH_END();

    metric_name = DDOG_CHARSLICE_C("logs_created");
    ddog_sidecar_telemetry_register_metric_buffer(buffer, metric_name, DDOG_METRIC_TYPE_COUNT, DDOG_METRIC_NAMESPACE_GENERAL);
    static struct {
        ddog_CharSlice level;
        ddog_CharSlice tags;
    } log_levels[] = {
        {DDOG_CHARSLICE_C_BARE("trace"), DDOG_CHARSLICE_C_BARE("level:trace")},
        {DDOG_CHARSLICE_C_BARE("debug"), DDOG_CHARSLICE_C_BARE("level:debug")},
        {DDOG_CHARSLICE_C_BARE("info"), DDOG_CHARSLICE_C_BARE("level:info")},
        {DDOG_CHARSLICE_C_BARE("warn"), DDOG_CHARSLICE_C_BARE("level:warn")},
        {DDOG_CHARSLICE_C_BARE("error"), DDOG_CHARSLICE_C_BARE("level:error")},
    };
    uint32_t count;
    for (size_t i = 0; i < sizeof(log_levels) / sizeof(log_levels[0]); ++i) {
        if ((count = ddog_get_logs_count(log_levels[i].level)) > 0) {
            ddog_sidecar_telemetry_add_span_metric_point_buffer(buffer, metric_name, (double)count, log_levels[i].tags);
        }
    }

    dd_commit_metrics(buffer);

    ddog_sidecar_telemetry_buffer_flush(&ddtrace_sidecar, ddtrace_sidecar_instance_id, &DDTRACE_G(telemetry_queue_id), buffer);

    ddog_CharSlice service_name = DDOG_CHARSLICE_C_BARE("unnamed-php-service");
    if (DDTRACE_G(last_flushed_root_service_name)) {
        service_name = dd_zend_string_to_CharSlice(DDTRACE_G(last_flushed_root_service_name));
    }

    ddog_CharSlice env_name = DDOG_CHARSLICE_C_BARE("none");
    if (DDTRACE_G(last_flushed_root_env_name)) {
        env_name = dd_zend_string_to_CharSlice(DDTRACE_G(last_flushed_root_env_name));
    }

    ddog_CharSlice php_version = dd_zend_string_to_CharSlice(Z_STR_P(zend_get_constant_str(ZEND_STRL("PHP_VERSION"))));
    struct ddog_RuntimeMetadata *meta = ddog_sidecar_runtimeMeta_build(DDOG_CHARSLICE_C("php"), php_version, DDOG_CHARSLICE_C(PHP_DDTRACE_VERSION));

    ddog_sidecar_telemetry_flushServiceData(&ddtrace_sidecar, ddtrace_sidecar_instance_id, &DDTRACE_G(telemetry_queue_id), meta, service_name, env_name);

    ddog_sidecar_runtimeMeta_drop(meta);

    ddog_sidecar_telemetry_end(&ddtrace_sidecar, ddtrace_sidecar_instance_id, &DDTRACE_G(telemetry_queue_id));
}

void ddtrace_telemetry_notify_integration(const char *name, size_t name_len) {
    if (ddtrace_sidecar && get_global_DD_INSTRUMENTATION_TELEMETRY_ENABLED()) {
        ddog_CharSlice integration = (ddog_CharSlice) {.len = name_len, .ptr = name};
        ddog_sidecar_telemetry_addIntegration(&ddtrace_sidecar, ddtrace_sidecar_instance_id, &DDTRACE_G(telemetry_queue_id), integration,
                                              DDOG_CHARSLICE_C(""), true);
    }
}

void ddtrace_telemetry_inc_spans_created(ddtrace_span_data *span) {
    zval *component = NULL;
    if (Z_TYPE(span->property_meta) == IS_ARRAY) {
        component = zend_hash_str_find(Z_ARRVAL(span->property_meta), ZEND_STRL("component"));
    }

    zend_string *integration = NULL;
    if (component && Z_TYPE_P(component) == IS_STRING) {
        integration = zend_string_copy(Z_STR_P(component));
    } else if (span->flags & DDTRACE_SPAN_FLAG_OPENTELEMETRY) {
        integration = zend_string_init(ZEND_STRL("otel"), 0);
    } else if (span->flags & DDTRACE_SPAN_FLAG_OPENTRACING) {
        integration = zend_string_init(ZEND_STRL("opentracing"), 0);
    } else {
        // Fallback value when the span has not been created by an integration, nor OpenTelemetry/OpenTracing (i.e. \DDTrace\span_start())
        integration = zend_string_init(ZEND_STRL("datadog"), 0);
    }

    zval *current = zend_hash_find(&DDTRACE_G(telemetry_spans_created_per_integration), integration);
    if (current) {
        ++Z_DVAL_P(current);
    } else {
        zval counter;
        ZVAL_DOUBLE(&counter, 1.0);
        zend_hash_add(&DDTRACE_G(telemetry_spans_created_per_integration), integration, &counter);
    }

    zend_string_release(integration);
}

ZEND_TLS HashTable metric_buffers;

typedef struct {
    double value;
    zend_string *tags;
} ddtrace_value_and_tags;
typedef struct {
    union {
        ddtrace_value_and_tags single_value;
        ddtrace_value_and_tags *values;
    };
    uint32_t capacity;
    uint32_t count;
    ddog_MetricType type;
    ddog_MetricNamespace ns;
} ddtrace_metric_buffer;

static void dd_metric_buffer_free(zval *buffer_zv) {
    ddtrace_metric_buffer *buf = Z_PTR_P(buffer_zv);
    if (buf->capacity == 1) {
        zend_string_release(buf->single_value.tags);
    } else {
        for (uint32_t i = 0; i < buf->count; ++i) {
            zend_string_release(buf->values[i].tags);
        }
        pefree(buf->values, 1);
    }
    pefree(buf, 1);
}

static HashTable *dd_metric_buffers_get() {
    if (GC_REFCOUNT(&metric_buffers) == 0) {
        zend_hash_init(&metric_buffers, sizeof(ddtrace_metric_buffer), NULL, dd_metric_buffer_free, 1);
        assert(GC_REFCOUNT(&metric_buffers) > 0);
    }
    return &metric_buffers;
}

DDTRACE_PUBLIC void ddtrace_metric_register_buffer(zend_string *name, ddog_MetricType type, ddog_MetricNamespace ns)
{
    HashTable *buffers_ht = dd_metric_buffers_get();
    if (zend_hash_exists(buffers_ht, name)) {
        return;
    }

    ddtrace_metric_buffer *buf = pemalloc(sizeof *buf, 1);
    buf->capacity = 1;
    buf->count = 0;
    buf->type = type;
    buf->ns = ns;
    zend_hash_add_ptr(buffers_ht, name, buf);
}
DDTRACE_PUBLIC bool ddtrace_metric_add_point(zend_string *name, double value, zend_string *tags) {
    ddtrace_metric_buffer *buf = zend_hash_find_ptr(dd_metric_buffers_get(), name);
    if (!buf) {
        return false;
    }
    if (!tags) {
        tags = zend_empty_string;
    }

    ddtrace_value_and_tags value_and_tags = {
        .value = value,
        .tags = (zend_string_addref(tags), tags),
    };
    if (buf->count == 0) {
        if (buf->capacity == 1) {
            buf->single_value = value_and_tags;
        } else {
            buf->values[0] = value_and_tags;
        }
        buf->count = 1;
    } else {
        if (buf->count == buf->capacity) {
            if (buf->capacity == 1) {
                buf->values = pemalloc(8 * sizeof(*buf->values), 1);
                buf->capacity = 8;
                buf->values[0] = buf->single_value;
            } else {
                if (buf->capacity * 2 < buf->capacity) {
                    // overflow
                    return false;
                }
                buf->capacity *= 2;
                buf->values = safe_perealloc(buf->values, buf->capacity, sizeof(*buf->values), 0, 1);
            }
        }

        buf->values[buf->count++] = value_and_tags;
    }

    return true;
}

static void dd_commit_metrics(ddog_SidecarActionsBuffer *sca_buffer) {
    zend_string *name;
    ddtrace_metric_buffer *buf;
    ZEND_HASH_FOREACH_STR_KEY_PTR(dd_metric_buffers_get(), name, buf) {
        if (buf->count == 0) {
            continue;
        }
        ddog_CharSlice metric_name = dd_zend_string_to_CharSlice(name);
        ddog_sidecar_telemetry_register_metric_buffer(sca_buffer, metric_name, buf->type, buf->ns);
        if (buf->capacity == 1) {  // count == 1
            ddog_sidecar_telemetry_add_span_metric_point_buffer(sca_buffer, metric_name, buf->single_value.value,
                                                                dd_zend_string_to_CharSlice(buf->single_value.tags));
        } else {
            for (uint32_t i = 0; i < buf->count; ++i) {
                zend_string *tags = buf->values[i].tags;
                ddog_sidecar_telemetry_add_span_metric_point_buffer(sca_buffer, metric_name, buf->values[i].value,
                                                                    dd_zend_string_to_CharSlice(tags));
                zend_string_release(tags);
            }
        }
        buf->count = 0;
    }
    ZEND_HASH_FOREACH_END();
}
