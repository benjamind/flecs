#include "../private_api.h"

#ifdef FLECS_REST

typedef struct {
    ecs_world_t *world;
    ecs_entity_t entity;
    ecs_http_server_t *srv;
    int32_t rc;
} ecs_rest_ctx_t;

static ECS_COPY(EcsRest, dst, src, {
    ecs_rest_ctx_t *impl = src->impl;
    if (impl) {
        impl->rc ++;
    }

    ecs_os_strset(&dst->ipaddr, src->ipaddr);
    dst->port = src->port;
    dst->impl = impl;
})

static ECS_MOVE(EcsRest, dst, src, {
    *dst = *src;
    src->ipaddr = NULL;
    src->impl = NULL;
})

static ECS_DTOR(EcsRest, ptr, { 
    ecs_rest_ctx_t *impl = ptr->impl;
    if (impl) {
        impl->rc --;
        if (!impl->rc) {
            ecs_http_server_fini(impl->srv);
            ecs_os_free(impl);
        }
    }
    ecs_os_free(ptr->ipaddr);
})

static char *rest_last_err;

static 
void flecs_rest_capture_log(
    int32_t level, 
    const char *file, 
    int32_t line, 
    const char *msg)
{
    (void)file; (void)line;

    if (!rest_last_err && level < 0) {
        rest_last_err = ecs_os_strdup(msg);
    }
}

static
char* flecs_rest_get_captured_log(void) {
    char *result = rest_last_err;
    rest_last_err = NULL;
    return result;
}

static
void flecs_reply_verror(
    ecs_http_reply_t *reply,
    const char *fmt,
    va_list args)
{
    ecs_strbuf_appendlit(&reply->body, "{\"error\":\"");
    ecs_strbuf_vappend(&reply->body, fmt, args);
    ecs_strbuf_appendlit(&reply->body, "\"}");
}

static
void flecs_reply_error(
    ecs_http_reply_t *reply,
    const char *fmt,
    ...)
{
    va_list args;
    va_start(args, fmt);
    flecs_reply_verror(reply, fmt, args);
    va_end(args);
}

static
void flecs_rest_bool_param(
    const ecs_http_request_t *req,
    const char *name,
    bool *value_out)
{
    const char *value = ecs_http_get_param(req, name);
    if (value) {
        if (!ecs_os_strcmp(value, "true")) {
            value_out[0] = true;
        } else {
            value_out[0] = false;
        }
    }
}

static
void flecs_rest_int_param(
    const ecs_http_request_t *req,
    const char *name,
    int32_t *value_out)
{
    const char *value = ecs_http_get_param(req, name);
    if (value) {
        *value_out = atoi(value);
    }
}

static
void flecs_rest_string_param(
    const ecs_http_request_t *req,
    const char *name,
    char **value_out)
{
    const char *value = ecs_http_get_param(req, name);
    if (value) {
        *value_out = (char*)value;
    }
}

static
void flecs_rest_parse_json_ser_entity_params(
    ecs_entity_to_json_desc_t *desc,
    const ecs_http_request_t *req)
{
    flecs_rest_bool_param(req, "path", &desc->serialize_path);
    flecs_rest_bool_param(req, "label", &desc->serialize_label);
    flecs_rest_bool_param(req, "brief", &desc->serialize_brief);
    flecs_rest_bool_param(req, "link", &desc->serialize_link);
    flecs_rest_bool_param(req, "color", &desc->serialize_color);
    flecs_rest_bool_param(req, "id_labels", &desc->serialize_id_labels);
    flecs_rest_bool_param(req, "base", &desc->serialize_base);
    flecs_rest_bool_param(req, "values", &desc->serialize_values);
    flecs_rest_bool_param(req, "private", &desc->serialize_private);
    flecs_rest_bool_param(req, "type_info", &desc->serialize_type_info);
}

static
void flecs_rest_parse_json_ser_iter_params(
    ecs_iter_to_json_desc_t *desc,
    const ecs_http_request_t *req)
{
    flecs_rest_bool_param(req, "term_ids", &desc->serialize_term_ids);
    flecs_rest_bool_param(req, "ids", &desc->serialize_ids);
    flecs_rest_bool_param(req, "sources", &desc->serialize_sources);
    flecs_rest_bool_param(req, "variables", &desc->serialize_variables);
    flecs_rest_bool_param(req, "is_set", &desc->serialize_is_set);
    flecs_rest_bool_param(req, "values", &desc->serialize_values);
    flecs_rest_bool_param(req, "entities", &desc->serialize_entities);
    flecs_rest_bool_param(req, "entity_labels", &desc->serialize_entity_labels);
    flecs_rest_bool_param(req, "entity_ids", &desc->serialize_entity_ids);
    flecs_rest_bool_param(req, "variable_labels", &desc->serialize_variable_labels);
    flecs_rest_bool_param(req, "variable_ids", &desc->serialize_variable_ids);
    flecs_rest_bool_param(req, "colors", &desc->serialize_colors);
    flecs_rest_bool_param(req, "duration", &desc->measure_eval_duration);
    flecs_rest_bool_param(req, "type_info", &desc->serialize_type_info);
}

static
bool flecs_rest_reply_entity(
    ecs_world_t *world,
    const ecs_http_request_t* req,
    ecs_http_reply_t *reply)
{
    char *path = &req->path[7];
    ecs_dbg_2("rest: request entity '%s'", path);

    ecs_entity_t e = ecs_lookup_path_w_sep(
        world, 0, path, "/", NULL, false);
    if (!e) {
        ecs_dbg_2("rest: entity '%s' not found", path);
        flecs_reply_error(reply, "entity '%s' not found", path);
        reply->code = 404;
        return true;
    }

    ecs_entity_to_json_desc_t desc = ECS_ENTITY_TO_JSON_INIT;
    flecs_rest_parse_json_ser_entity_params(&desc, req);

    ecs_entity_to_json_buf(world, e, &reply->body, &desc);
    return true;
}

static
bool flecs_rest_reply_query(
    ecs_world_t *world,
    const ecs_http_request_t* req,
    ecs_http_reply_t *reply)
{
    const char *q = ecs_http_get_param(req, "q");
    if (!q) {
        ecs_strbuf_appendlit(&reply->body, "Missing parameter 'q'");
        reply->code = 400; /* bad request */
        return true;
    }

    ecs_dbg_2("rest: request query '%s'", q);
    bool prev_color = ecs_log_enable_colors(false);
    ecs_os_api_log_t prev_log_ = ecs_os_api.log_;
    ecs_os_api.log_ = flecs_rest_capture_log;

    ecs_rule_t *r = ecs_rule_init(world, &(ecs_filter_desc_t){
        .expr = q
    });
    if (!r) {
        char *err = flecs_rest_get_captured_log();
        char *escaped_err = ecs_astresc('"', err);
        flecs_reply_error(reply, escaped_err);
        reply->code = 400; /* bad request */
        ecs_os_free(escaped_err);
        ecs_os_free(err);
    } else {
        ecs_iter_to_json_desc_t desc = ECS_ITER_TO_JSON_INIT;
        flecs_rest_parse_json_ser_iter_params(&desc, req);

        int32_t offset = 0;
        int32_t limit = 1000;

        flecs_rest_int_param(req, "offset", &offset);
        flecs_rest_int_param(req, "limit", &limit);

        ecs_iter_t it = ecs_rule_iter(world, r);
        ecs_iter_t pit = ecs_page_iter(&it, offset, limit);
        ecs_iter_to_json_buf(world, &pit, &reply->body, &desc);
        ecs_rule_fini(r);
    }

    ecs_os_api.log_ = prev_log_;
    ecs_log_enable_colors(prev_color);

    return true;
}

#ifdef FLECS_MONITOR

static
void _flecs_rest_array_append(
    ecs_strbuf_t *reply,
    const char *field,
    int32_t field_len,
    const ecs_float_t *values,
    int32_t t)
{
    ecs_strbuf_list_appendch(reply, '"');
    ecs_strbuf_appendstrn(reply, field, field_len);
    ecs_strbuf_appendlit(reply, "\":");
    ecs_strbuf_list_push(reply, "[", ",");

    int32_t i;
    for (i = t + 1; i <= (t + ECS_STAT_WINDOW); i ++) {
        int32_t index = i % ECS_STAT_WINDOW;
        ecs_strbuf_list_next(reply);
        ecs_strbuf_appendflt(reply, (double)values[index], '"');
    }

    ecs_strbuf_list_pop(reply, "]");
}

#define flecs_rest_array_append(reply, field, values, t)\
    _flecs_rest_array_append(reply, field, sizeof(field) - 1, values, t)

static
void flecs_rest_gauge_append(
    ecs_strbuf_t *reply,
    const ecs_metric_t *m,
    const char *field,
    int32_t field_len,
    int32_t t,
    const char *brief,
    int32_t brief_len)
{
    ecs_strbuf_list_appendch(reply, '"');
    ecs_strbuf_appendstrn(reply, field, field_len);
    ecs_strbuf_appendlit(reply, "\":");
    ecs_strbuf_list_push(reply, "{", ",");

    flecs_rest_array_append(reply, "avg", m->gauge.avg, t);
    flecs_rest_array_append(reply, "min", m->gauge.min, t);
    flecs_rest_array_append(reply, "max", m->gauge.max, t);

    if (brief) {
        ecs_strbuf_list_appendlit(reply, "\"brief\":\"");
        ecs_strbuf_appendstrn(reply, brief, brief_len);
        ecs_strbuf_appendch(reply, '"');
    }

    ecs_strbuf_list_pop(reply, "}");
}

static
void flecs_rest_counter_append(
    ecs_strbuf_t *reply,
    const ecs_metric_t *m,
    const char *field,
    int32_t field_len,
    int32_t t,
    const char *brief,
    int32_t brief_len)
{
    flecs_rest_gauge_append(reply, m, field, field_len, t, brief, brief_len);
}

#define ECS_GAUGE_APPEND_T(reply, s, field, t, brief)\
    flecs_rest_gauge_append(reply, &(s)->field, #field, sizeof(#field) - 1, t, brief, sizeof(brief) - 1)

#define ECS_COUNTER_APPEND_T(reply, s, field, t, brief)\
    flecs_rest_counter_append(reply, &(s)->field, #field, sizeof(#field) - 1, t, brief, sizeof(brief) - 1)

#define ECS_GAUGE_APPEND(reply, s, field, brief)\
    ECS_GAUGE_APPEND_T(reply, s, field, (s)->t, brief)

#define ECS_COUNTER_APPEND(reply, s, field, brief)\
    ECS_COUNTER_APPEND_T(reply, s, field, (s)->t, brief)

static
void flecs_world_stats_to_json(
    ecs_strbuf_t *reply,
    const EcsWorldStats *monitor_stats)
{
    const ecs_world_stats_t *stats = &monitor_stats->stats;

    ecs_strbuf_list_push(reply, "{", ",");
    ECS_GAUGE_APPEND(reply, stats, entities.count, "Alive entity ids in the world");
    ECS_GAUGE_APPEND(reply, stats, entities.not_alive_count, "Not alive entity ids in the world");

    ECS_GAUGE_APPEND(reply, stats, performance.fps, "Frames per second");
    ECS_COUNTER_APPEND(reply, stats, performance.frame_time, "Time spent in frame");
    ECS_COUNTER_APPEND(reply, stats, performance.system_time, "Time spent on running systems in frame");
    ECS_COUNTER_APPEND(reply, stats, performance.emit_time, "Time spent on notifying observers in frame");
    ECS_COUNTER_APPEND(reply, stats, performance.merge_time, "Time spent on merging commands in frame");
    ECS_COUNTER_APPEND(reply, stats, performance.rematch_time, "Time spent on revalidating query caches in frame");

    ECS_COUNTER_APPEND(reply, stats, commands.add_count, "Add commands executed");
    ECS_COUNTER_APPEND(reply, stats, commands.remove_count, "Remove commands executed");
    ECS_COUNTER_APPEND(reply, stats, commands.delete_count, "Delete commands executed");
    ECS_COUNTER_APPEND(reply, stats, commands.clear_count, "Clear commands executed");
    ECS_COUNTER_APPEND(reply, stats, commands.set_count, "Set commands executed");
    ECS_COUNTER_APPEND(reply, stats, commands.get_mut_count, "Get_mut commands executed");
    ECS_COUNTER_APPEND(reply, stats, commands.modified_count, "Modified commands executed");
    ECS_COUNTER_APPEND(reply, stats, commands.other_count, "Misc commands executed");
    ECS_COUNTER_APPEND(reply, stats, commands.discard_count, "Commands for already deleted entities");
    ECS_COUNTER_APPEND(reply, stats, commands.batched_entity_count, "Entities with batched commands");
    ECS_COUNTER_APPEND(reply, stats, commands.batched_count, "Number of commands batched");

    ECS_COUNTER_APPEND(reply, stats, frame.merge_count, "Number of merges (sync points)");
    ECS_COUNTER_APPEND(reply, stats, frame.pipeline_build_count, "Pipeline rebuilds (happen when systems become active/enabled)");
    ECS_COUNTER_APPEND(reply, stats, frame.systems_ran, "Systems ran in frame");
    ECS_COUNTER_APPEND(reply, stats, frame.observers_ran, "Number of times an observer was invoked in frame");
    ECS_COUNTER_APPEND(reply, stats, frame.event_emit_count, "Events emitted in frame");
    ECS_COUNTER_APPEND(reply, stats, frame.rematch_count, "Number of query cache revalidations");

    ECS_GAUGE_APPEND(reply, stats, tables.count, "Tables in the world (including empty)");
    ECS_GAUGE_APPEND(reply, stats, tables.empty_count, "Empty tables in the world");
    ECS_GAUGE_APPEND(reply, stats, tables.tag_only_count, "Tables with only tags");
    ECS_GAUGE_APPEND(reply, stats, tables.trivial_only_count, "Tables with only trivial types (no hooks)");
    ECS_GAUGE_APPEND(reply, stats, tables.record_count, "Table records registered with search indices");
    ECS_GAUGE_APPEND(reply, stats, tables.storage_count, "Component storages for all tables");
    ECS_COUNTER_APPEND(reply, stats, tables.create_count, "Number of new tables created");
    ECS_COUNTER_APPEND(reply, stats, tables.delete_count, "Number of tables deleted");

    ECS_GAUGE_APPEND(reply, stats, ids.count, "Component, tag and pair ids in use");
    ECS_GAUGE_APPEND(reply, stats, ids.tag_count, "Tag ids in use");
    ECS_GAUGE_APPEND(reply, stats, ids.component_count, "Component ids in use");
    ECS_GAUGE_APPEND(reply, stats, ids.pair_count, "Pair ids in use");
    ECS_GAUGE_APPEND(reply, stats, ids.wildcard_count, "Wildcard ids in use");
    ECS_GAUGE_APPEND(reply, stats, ids.type_count, "Registered component types");
    ECS_COUNTER_APPEND(reply, stats, ids.create_count, "Number of new component, tag and pair ids created");
    ECS_COUNTER_APPEND(reply, stats, ids.delete_count, "Number of component, pair and tag ids deleted");

    ECS_GAUGE_APPEND(reply, stats, queries.query_count, "Queries in the world");
    ECS_GAUGE_APPEND(reply, stats, queries.observer_count, "Observers in the world");
    ECS_GAUGE_APPEND(reply, stats, queries.system_count, "Systems in the world");

    ECS_COUNTER_APPEND(reply, stats, memory.alloc_count, "Allocations by OS API");
    ECS_COUNTER_APPEND(reply, stats, memory.realloc_count, "Reallocs by OS API");
    ECS_COUNTER_APPEND(reply, stats, memory.free_count, "Frees by OS API");
    ECS_GAUGE_APPEND(reply, stats, memory.outstanding_alloc_count, "Outstanding allocations by OS API");
    ECS_COUNTER_APPEND(reply, stats, memory.block_alloc_count, "Blocks allocated by block allocators");
    ECS_COUNTER_APPEND(reply, stats, memory.block_free_count, "Blocks freed by block allocators");
    ECS_GAUGE_APPEND(reply, stats, memory.block_outstanding_alloc_count, "Outstanding block allocations");
    ECS_COUNTER_APPEND(reply, stats, memory.stack_alloc_count, "Pages allocated by stack allocators");
    ECS_COUNTER_APPEND(reply, stats, memory.stack_free_count, "Pages freed by stack allocators");
    ECS_GAUGE_APPEND(reply, stats, memory.stack_outstanding_alloc_count, "Outstanding page allocations");

    ECS_COUNTER_APPEND(reply, stats, trav_cache.entity_down_hit, "Cache hit for downwards search from entity");
    ECS_COUNTER_APPEND(reply, stats, trav_cache.entity_down_miss, "Cache miss for downwards search from entity");
    ECS_COUNTER_APPEND(reply, stats, trav_cache.entity_down_count, "Number of entries in the entity down cache");
    ECS_COUNTER_APPEND(reply, stats, trav_cache.table_down_hit, "Cache hit for downwards search from table");
    ECS_COUNTER_APPEND(reply, stats, trav_cache.table_down_miss, "Cache miss for downwards search from table");
    ECS_COUNTER_APPEND(reply, stats, trav_cache.table_down_count, "Number of entries in the table down cache");

    ecs_strbuf_list_pop(reply, "}");
}

static
void flecs_system_stats_to_json(
    ecs_world_t *world,
    ecs_strbuf_t *reply,
    ecs_entity_t system,
    const ecs_system_stats_t *stats)
{
    ecs_strbuf_list_push(reply, "{", ",");
    ecs_strbuf_list_appendlit(reply, "\"name\":\"");
    ecs_get_path_w_sep_buf(world, 0, system, ".", NULL, reply);
    ecs_strbuf_appendch(reply, '"');

    if (!stats->task) {
        ECS_GAUGE_APPEND(reply, &stats->query, matched_table_count, "");
        ECS_GAUGE_APPEND(reply, &stats->query, matched_entity_count, "");
    }

    ECS_COUNTER_APPEND_T(reply, stats, time_spent, stats->query.t, "");
    ecs_strbuf_list_pop(reply, "}");
}

static
void flecs_pipeline_stats_to_json(
    ecs_world_t *world,
    ecs_strbuf_t *reply,
    const EcsPipelineStats *stats)
{
    ecs_strbuf_list_push(reply, "[", ",");

    int32_t i, count = ecs_vector_count(stats->stats.systems);
    ecs_entity_t *ids = ecs_vector_first(stats->stats.systems, ecs_entity_t);
    for (i = 0; i < count; i ++) {
        ecs_entity_t id = ids[i];
        
        ecs_strbuf_list_next(reply);

        if (id) {
            ecs_system_stats_t *sys_stats = ecs_map_get(
                    &stats->stats.system_stats, ecs_system_stats_t, id);
            flecs_system_stats_to_json(world, reply, id, sys_stats);
        } else {
            /* Sync point */
            ecs_strbuf_list_push(reply, "{", ",");
            ecs_strbuf_list_pop(reply, "}");
        }
    }

    ecs_strbuf_list_pop(reply, "]");
}

static
bool flecs_rest_reply_stats(
    ecs_world_t *world,
    const ecs_http_request_t* req,
    ecs_http_reply_t *reply)
{
    char *period_str = NULL;
    flecs_rest_string_param(req, "period", &period_str);
    char *category = &req->path[6];

    ecs_entity_t period = EcsPeriod1s;
    if (period_str) {
        char *period_name = ecs_asprintf("Period%s", period_str);
        period = ecs_lookup_child(world, ecs_id(FlecsMonitor), period_name);
        ecs_os_free(period_name);
        if (!period) {
            flecs_reply_error(reply, "bad request (invalid period string)");
            reply->code = 400;
            return false;
        }
    }

    if (!ecs_os_strcmp(category, "world")) {
        const EcsWorldStats *stats = ecs_get_pair(world, EcsWorld, 
            EcsWorldStats, period);
        flecs_world_stats_to_json(&reply->body, stats);
        return true;

    } else if (!ecs_os_strcmp(category, "pipeline")) {
        const EcsPipelineStats *stats = ecs_get_pair(world, EcsWorld, 
            EcsPipelineStats, period);
        flecs_pipeline_stats_to_json(world, &reply->body, stats);
        return true;

    } else {
        flecs_reply_error(reply, "bad request (unsupported category)");
        reply->code = 400;
        return false;
    }

    return true;
}
#else
static
bool flecs_rest_reply_stats(
    ecs_world_t *world,
    const ecs_http_request_t* req,
    ecs_http_reply_t *reply)
{
    (void)world;
    (void)req;
    (void)reply;
    return false;
}
#endif

static
void flecs_rest_reply_table_append_type(
    ecs_world_t *world,
    ecs_strbuf_t *reply,
    const ecs_table_t *table)
{
    ecs_strbuf_list_push(reply, "[", ",");
    int32_t i, count = table->type.count;
    ecs_id_t *ids = table->type.array;
    for (i = 0; i < count; i ++) {
        ecs_strbuf_list_next(reply);
        ecs_strbuf_appendch(reply, '"');
        ecs_id_str_buf(world, ids[i], reply);
        ecs_strbuf_appendch(reply, '"');
    }
    ecs_strbuf_list_pop(reply, "]");
}

static
void flecs_rest_reply_table_append_memory(
    ecs_strbuf_t *reply,
    const ecs_table_t *table)
{
    int32_t used = 0, allocated = 0;

    used += table->data.entities.count * ECS_SIZEOF(ecs_entity_t);
    used += table->data.records.count * ECS_SIZEOF(ecs_record_t*);
    allocated += table->data.entities.size * ECS_SIZEOF(ecs_entity_t);
    allocated += table->data.records.size * ECS_SIZEOF(ecs_record_t*);

    int32_t i, storage_count = table->storage_count;
    ecs_type_info_t **ti = table->type_info;
    ecs_vec_t *storages = table->data.columns;

    for (i = 0; i < storage_count; i ++) {
        used += storages[i].count * ti[i]->size;
        allocated += storages[i].size * ti[i]->size;
    }

    ecs_strbuf_list_push(reply, "{", ",");
    ecs_strbuf_list_append(reply, "\"used\":%d", used);
    ecs_strbuf_list_append(reply, "\"allocated\":%d", allocated);
    ecs_strbuf_list_pop(reply, "}");
}

static
void flecs_rest_reply_table_append(
    ecs_world_t *world,
    ecs_strbuf_t *reply,
    const ecs_table_t *table)
{
    ecs_strbuf_list_next(reply);
    ecs_strbuf_list_push(reply, "{", ",");
    ecs_strbuf_list_append(reply, "\"id\":%u", (uint32_t)table->id);
    ecs_strbuf_list_appendstr(reply, "\"type\":");
    flecs_rest_reply_table_append_type(world, reply, table);
    ecs_strbuf_list_append(reply, "\"count\":%d", ecs_table_count(table));
    ecs_strbuf_list_append(reply, "\"memory\":");
    flecs_rest_reply_table_append_memory(reply, table);
    ecs_strbuf_list_append(reply, "\"refcount\":%d", table->refcount);
    ecs_strbuf_list_pop(reply, "}");
}

static
bool flecs_rest_reply_tables(
    ecs_world_t *world,
    const ecs_http_request_t* req,
    ecs_http_reply_t *reply)
{
    (void)req;

    ecs_strbuf_list_push(&reply->body, "[", ",");
    ecs_sparse_t *tables = &world->store.tables;
    int32_t i, count = flecs_sparse_count(tables);
    for (i = 0; i < count; i ++) {
        ecs_table_t *table = flecs_sparse_get_dense(tables, ecs_table_t, i);
        flecs_rest_reply_table_append(world, &reply->body, table);
    }
    ecs_strbuf_list_pop(&reply->body, "]");

    return true;
}

static
void flecs_rest_reply_id_append(
    ecs_world_t *world,
    ecs_strbuf_t *reply,
    const ecs_id_record_t *idr)
{
    ecs_strbuf_list_next(reply);
    ecs_strbuf_list_push(reply, "{", ",");
    ecs_strbuf_list_appendstr(reply, "\"id\":\"");
    ecs_id_str_buf(world, idr->id, reply);
    ecs_strbuf_appendch(reply, '"');

    if (idr->type_info) {
        if (idr->type_info->component != idr->id) {
            ecs_strbuf_list_appendstr(reply, "\"component\":\"");
            ecs_id_str_buf(world, idr->type_info->component, reply);
            ecs_strbuf_appendch(reply, '"');
        }

        ecs_strbuf_list_append(reply, "\"size\":%d", 
            idr->type_info->size);
        ecs_strbuf_list_append(reply, "\"alignment\":%d", 
            idr->type_info->alignment);
    }

    ecs_strbuf_list_append(reply, "\"table_count\":%d", 
        idr->cache.tables.count);
    ecs_strbuf_list_append(reply, "\"empty_table_count\":%d", 
        idr->cache.empty_tables.count);

    ecs_strbuf_list_pop(reply, "}");
}

static
bool flecs_rest_reply_ids(
    ecs_world_t *world,
    const ecs_http_request_t* req,
    ecs_http_reply_t *reply)
{
    (void)req;

    ecs_strbuf_list_push(&reply->body, "[", ",");
    ecs_map_iter_t it = ecs_map_iter(&world->id_index);
    ecs_id_record_t *idr;
    while ((idr = ecs_map_next_ptr(&it, ecs_id_record_t*, NULL))) {
        flecs_rest_reply_id_append(world, &reply->body, idr);
    }
    ecs_strbuf_list_pop(&reply->body, "]");

    return true;
}

static
bool flecs_rest_reply(
    const ecs_http_request_t* req,
    ecs_http_reply_t *reply,
    void *ctx)
{
    ecs_rest_ctx_t *impl = ctx;
    ecs_world_t *world = impl->world;

    if (req->path == NULL) {
        ecs_dbg("rest: bad request (missing path)");
        flecs_reply_error(reply, "bad request (missing path)");
        reply->code = 400;
        return false;
    }

    ecs_strbuf_appendlit(&reply->headers, "Access-Control-Allow-Origin: *\r\n");

    if (req->method == EcsHttpGet) {
        /* Entity endpoint */
        if (!ecs_os_strncmp(req->path, "entity/", 7)) {
            return flecs_rest_reply_entity(world, req, reply);
        
        /* Query endpoint */
        } else if (!ecs_os_strcmp(req->path, "query")) {
            return flecs_rest_reply_query(world, req, reply);

        /* Stats endpoint */
        } else if (!ecs_os_strncmp(req->path, "stats/", 6)) {
            return flecs_rest_reply_stats(world, req, reply);

        /* Tables endpoint */
        } else if (!ecs_os_strncmp(req->path, "tables", 6)) {
            return flecs_rest_reply_tables(world, req, reply);

        /* Ids endpoint */
        } else if (!ecs_os_strncmp(req->path, "ids", 3)) {
            return flecs_rest_reply_ids(world, req, reply);
        }
    } else if (req->method == EcsHttpOptions) {
        return true;
    }

    return false;
}

static
void flecs_on_set_rest(ecs_iter_t *it)
{
    EcsRest *rest = it->ptrs[0];

    int i;
    for(i = 0; i < it->count; i ++) {
        if (!rest[i].port) {
            rest[i].port = ECS_REST_DEFAULT_PORT;
        }

        ecs_rest_ctx_t *srv_ctx = ecs_os_malloc_t(ecs_rest_ctx_t);
        ecs_http_server_t *srv = ecs_http_server_init(&(ecs_http_server_desc_t){
            .ipaddr = rest[i].ipaddr,
            .port = rest[i].port,
            .callback = flecs_rest_reply,
            .ctx = srv_ctx
        });

        if (!srv) {
            const char *ipaddr = rest[i].ipaddr ? rest[i].ipaddr : "0.0.0.0";
            ecs_err("failed to create REST server on %s:%u", 
                ipaddr, rest[i].port);
            ecs_os_free(srv_ctx);
            continue;
        }

        srv_ctx->world = it->world;
        srv_ctx->entity = it->entities[i];
        srv_ctx->srv = srv;
        srv_ctx->rc = 1;

        rest[i].impl = srv_ctx;

        ecs_http_server_start(srv_ctx->srv);
    }
}

static
void DequeueRest(ecs_iter_t *it) {
    EcsRest *rest = ecs_field(it, EcsRest, 1);

    if (it->delta_system_time > (ecs_ftime_t)1.0) {
        ecs_warn(
            "detected large progress interval (%.2fs), REST request may timeout",
            (double)it->delta_system_time);
    }

    int32_t i;
    for(i = 0; i < it->count; i ++) {
        ecs_rest_ctx_t *ctx = rest[i].impl;
        if (ctx) {
            ecs_http_server_dequeue(ctx->srv, it->delta_time);
        }
    } 
}

void FlecsRestImport(
    ecs_world_t *world)
{
    ECS_MODULE(world, FlecsRest);

    ecs_set_name_prefix(world, "Ecs");

    flecs_bootstrap_component(world, EcsRest);

    ecs_set_hooks(world, EcsRest, { 
        .ctor = ecs_default_ctor,
        .move = ecs_move(EcsRest),
        .copy = ecs_copy(EcsRest),
        .dtor = ecs_dtor(EcsRest),
        .on_set = flecs_on_set_rest
    });

    ECS_SYSTEM(world, DequeueRest, EcsPostFrame, EcsRest);
}

#endif
