#include "private_api.h"

void flecs_observable_init(
    ecs_observable_t *observable)
{
    observable->events = ecs_sparse_new(ecs_event_record_t);
}

void flecs_observable_fini(
    ecs_observable_t *observable)
{
    ecs_sparse_t *triggers = observable->events;
    int32_t i, count = flecs_sparse_count(triggers);

    for (i = 0; i < count; i ++) {
        ecs_event_record_t *et = 
            ecs_sparse_get_dense(triggers, ecs_event_record_t, i);
        ecs_assert(et != NULL, ECS_INTERNAL_ERROR, NULL);
        (void)et;

        /* All triggers should've unregistered by now */
        ecs_assert(!ecs_map_is_initialized(&et->event_ids), 
            ECS_INTERNAL_ERROR, NULL);
    }

    flecs_sparse_free(observable->events);
}

static
bool flecs_id_has_observers(
    ecs_id_record_t *idr,
    ecs_entity_t event,
    bool builtin_event)
{
    if (builtin_event) {
        ecs_flags32_t flags = idr->flags;
        if (!(flags & EcsIdEventMask)) {
            return false;
        }
        if ((event == EcsOnAdd) && !(flags & EcsIdHasOnAdd)) {
            return false;
        }
        if ((event == EcsOnRemove) && !(flags & EcsIdHasOnRemove)) {
            return false;
        }
        if ((event == EcsOnSet) && !(flags & EcsIdHasOnSet)) {
            return false;
        }
        if ((event == EcsUnSet) && !(flags & EcsIdHasUnSet)) {
            return false;
        }
    }
    return true;
}

static
void notify_subset(
    ecs_world_t *world,
    ecs_iter_t *it,
    ecs_observable_t *observable,
    ecs_entity_t entity,
    ecs_entity_t event,
    const ecs_type_t *ids)
{
    ecs_id_t pair = ecs_pair(EcsWildcard, entity);
    ecs_id_record_t *idr = flecs_id_record_get(world, pair);
    if (!idr) {
        return;
    }

    bool builtin_event = (event == EcsOnAdd) || (event == EcsOnRemove) ||
                         (event == EcsOnSet) || (event == EcsUnSet);

    /* Iterate acyclic relationships */
    ecs_id_record_t *cur = idr;
    while ((cur = cur->acyclic.next)) {
        ecs_entity_t trav = ECS_PAIR_FIRST(cur->id);
        int32_t i, count = ids->count;
        for (i = 0; i < count; i ++) {
            ecs_id_t with = ids->array[i];
            ecs_type_t one_id = { .array = &with, .count = 1 };
            ecs_id_record_t *with_idr = flecs_id_record_get(world, with);
            if (!with_idr) {
                continue;
            }

            if (!flecs_id_has_observers(with_idr, event, builtin_event)) {
                continue;
            }

            const ecs_trav_down_t *cache = flecs_trav_entity_down_w_idr(
                world, trav, entity, with_idr);
            if (!cache) {
                continue;
            }

            ecs_vector_t *velems = cache->elems;
            int32_t t, elem_count = ecs_vector_count(velems);
            ecs_trav_elem_t *elems = ecs_vector_first(velems, ecs_trav_elem_t);
            for (t = 0; t < elem_count; t ++) {
                ecs_table_t *table = elems[t].table;
                if (elems[t].leaf) {
                    continue;
                }

                it->count = ecs_table_count(table);
                if (!it->count) {
                    continue;
                }

                it->table = table;
                it->other_table = NULL;
                it->offset = 0;

                world->event_id ++;
                flecs_set_observers_notify(it, observable, &one_id, event,
                    ecs_pair(trav, EcsWildcard), elems[t].source);
            }
        }
    }
}

void flecs_emit(
    ecs_world_t *world,
    ecs_world_t *stage,
    ecs_event_desc_t *desc)
{
    ecs_poly_assert(world, ecs_world_t);
    ecs_check(desc != NULL, ECS_INVALID_PARAMETER, NULL);
    ecs_check(desc->event != 0, ECS_INVALID_PARAMETER, NULL);
    ecs_check(desc->event != EcsWildcard, ECS_INVALID_PARAMETER, NULL);
    ecs_check(desc->ids != NULL, ECS_INVALID_PARAMETER, NULL);
    ecs_check(desc->ids->count != 0, ECS_INVALID_PARAMETER, NULL);
    ecs_check(desc->table != NULL, ECS_INVALID_PARAMETER, NULL);
    ecs_check(desc->observable != NULL, ECS_INVALID_PARAMETER, NULL);

    const ecs_type_t *ids = desc->ids;
    ecs_entity_t event = desc->event;
    ecs_table_t *table = desc->table;
    int32_t row = desc->offset;
    int32_t i, count = desc->count;
    ecs_entity_t relationship = desc->relationship;
    ecs_time_t t = {0};
    bool measure_time = world->flags & EcsWorldMeasureSystemTime;
    if (measure_time) {
        ecs_time_measure(&t);
    }

    if (!count) {
        count = ecs_table_count(table) - row;
    }

    ecs_id_t ids_cache = 0;
    void *ptrs_cache = NULL;
    ecs_size_t sizes_cache = 0;
    int32_t columns_cache = 0;
    ecs_entity_t sources_cache = 0;
    ecs_iter_t it = {
        .world = stage,
        .real_world = world,
        .table = table,
        .field_count = 1,
        .ids = &ids_cache,
        .ptrs = &ptrs_cache,
        .sizes = &sizes_cache,
        .columns = &columns_cache,
        .sources = &sources_cache,
        .other_table = desc->other_table,
        .offset = row,
        .count = count,
        .param = (void*)desc->param,
        .flags = desc->table_event ? EcsIterTableOnly : 0
    };

    world->event_id ++;

    ecs_observable_t *observable = ecs_get_observable(desc->observable);
    ecs_check(observable != NULL, ECS_INVALID_PARAMETER, NULL);

    if (!desc->relationship) {
        flecs_observers_notify(&it, observable, ids, event);
    } else {
        flecs_set_observers_notify(&it, observable, ids, event, 
            ecs_pair(relationship, EcsWildcard), 0);
    }

    if (count && !desc->table_event) {
        if (!table->observed_count) {
            goto done;
        }

        ecs_record_t **recs = ecs_vec_get_t(
            &table->data.records, ecs_record_t*, row);
        for (i = 0; i < count; i ++) {
            ecs_record_t *r = recs[i];
            if (!r) {
                /* If the event is emitted after a bulk operation, it's possible
                 * that it hasn't been populated with entities yet. */
                continue;
            }

            uint32_t flags = ECS_RECORD_TO_ROW_FLAGS(recs[i]->row);
            if (flags & EcsEntityObservedAcyclic) {
                notify_subset(world, &it, observable, ecs_vec_first_t(
                    &table->data.entities, ecs_entity_t)[row + i], event, ids);
            }
        }
    }

done:
error:
    if (measure_time) {
        world->info.emit_time_total += (ecs_ftime_t)ecs_time_measure(&t);
    }
    return;
}

void ecs_emit(
    ecs_world_t *stage,
    ecs_event_desc_t *desc)
{
    ecs_world_t *world = (ecs_world_t*)ecs_get_world(stage);
    flecs_emit(world, stage, desc);
}
