/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2020 Collabora Ltd. */
/*                         @author George Kiagiadakis <george.kiagiadakis@collabora.com> */
/* SPDX-License-Identifier: MIT */

#include <pipewire/impl.h>
#include <pipewire/extensions/session-manager.h>
#include <pipewire/extensions/session-manager/introspect-funcs.h>

#include <spa/utils/result.h>
#include <spa/pod/builder.h>
#include <spa/pod/filter.h>

#define MAX_PARAMS 32

#define NAME "endpoint"

struct pw_proxy *pw_core_endpoint_export(struct pw_core *core,
		const char *type, const struct spa_dict *props, void *object,
		size_t user_data_size);

struct impl
{
	struct pw_global *global;
	struct spa_hook global_listener;

	union {
		struct pw_endpoint *endpoint;
		struct pw_resource *resource;
	};
	struct spa_hook resource_listener;
	struct spa_hook endpoint_listener;

	struct pw_endpoint_info *cached_info;
	struct spa_list cached_params;

	int ping_seq;
	bool registered;
};

struct param_data
{
	struct spa_list link;
	uint32_t id;
	struct pw_array params;
};

struct resource_data
{
	struct impl *impl;

	struct pw_resource *resource;
	struct spa_hook object_listener;

	uint32_t n_subscribe_ids;
	uint32_t subscribe_ids[32];
};

struct factory_data
{
	struct pw_impl_module *module;
	struct spa_hook module_listener;

	struct pw_impl_factory *factory;
	struct spa_hook factory_listener;

	struct pw_export_type export;
};

#define pw_endpoint_resource(r,m,v,...)      \
	pw_resource_call(r,struct pw_endpoint_events,m,v,__VA_ARGS__)

#define pw_endpoint_resource_info(r,...)        \
        pw_endpoint_resource(r,info,0,__VA_ARGS__)
#define pw_endpoint_resource_param(r,...)        \
        pw_endpoint_resource(r,param,0,__VA_ARGS__)

static int method_enum_params(void *object, int seq,
			uint32_t id, uint32_t start, uint32_t num,
			const struct spa_pod *filter)
{
	struct resource_data *d = object;
	struct impl *impl = d->impl;
	struct param_data *pdata;
	struct spa_pod *result;
	struct spa_pod *param;
	uint8_t buffer[1024];
	struct spa_pod_builder b = { 0 };
	uint32_t index;
	uint32_t next = start;
	uint32_t count = 0;

	pw_log_debug(NAME" %p: param %u %d/%d", impl, id, start, num);

	spa_list_for_each(pdata, &impl->cached_params, link) {
		if (pdata->id != id)
			continue;

		while (true) {
			index = next++;
			if (index >= pw_array_get_len(&pdata->params, void*))
				return 0;

			param = *pw_array_get_unchecked(&pdata->params, index, struct spa_pod*);

			spa_pod_builder_init(&b, buffer, sizeof(buffer));
			if (spa_pod_filter(&b, &result, param, filter) != 0)
				continue;

			pw_log_debug(NAME" %p: %d param %u", impl, seq, index);

			pw_endpoint_resource_param(d->resource, seq, id, index, next, result);

			if (++count == num)
				return 0;
		}
	}

	return 0;
}

static int method_subscribe_params(void *object, uint32_t *ids, uint32_t n_ids)
{
	struct resource_data *d = object;
	struct impl *impl = d->impl;
	uint32_t i;

	n_ids = SPA_MIN(n_ids, SPA_N_ELEMENTS(d->subscribe_ids));
	d->n_subscribe_ids = n_ids;

	for (i = 0; i < n_ids; i++) {
		d->subscribe_ids[i] = ids[i];
		pw_log_debug(NAME" %p: resource %d subscribe param %u",
			impl, pw_resource_get_id(d->resource), ids[i]);
		method_enum_params(object, 1, ids[i], 0, UINT32_MAX, NULL);
	}
	return 0;
}

static int method_set_param(void *object, uint32_t id, uint32_t flags,
			  const struct spa_pod *param)
{
	struct resource_data *d = object;
	struct impl *impl = d->impl;
	/* store only on the implementation; our cache will be updated
	   by the param event, since we are subscribed */
	pw_endpoint_set_param(impl->endpoint, id, flags, param);
	return 0;
}

static int method_create_link(void *object, const struct spa_dict *props)
{
	struct resource_data *d = object;
	struct impl *impl = d->impl;
	pw_endpoint_create_link(impl->endpoint, props);
	return 0;
}

static const struct pw_endpoint_methods endpoint_methods = {
	PW_VERSION_ENDPOINT_METHODS,
	.subscribe_params = method_subscribe_params,
	.enum_params = method_enum_params,
	.set_param = method_set_param,
	.create_link = method_create_link,
};

static int global_bind(void *object, struct pw_impl_client *client,
		uint32_t permissions, uint32_t version, uint32_t id)
{
	struct impl *impl = object;
	struct pw_resource *resource;
	struct resource_data *data;

	resource = pw_resource_new(client, id, permissions,
				PW_TYPE_INTERFACE_Endpoint,
				version, sizeof(*data));
	if (resource == NULL)
		return -errno;

	data = pw_resource_get_user_data(resource);
	data->impl = impl;
	data->resource = resource;

	pw_global_add_resource(impl->global, resource);

	/* resource methods -> implementation */
	pw_resource_add_object_listener(resource,
			&data->object_listener,
			&endpoint_methods, data);

	impl->cached_info->change_mask = PW_ENDPOINT_CHANGE_MASK_ALL;
	pw_endpoint_resource_info(resource, impl->cached_info);
	impl->cached_info->change_mask = 0;

	return 0;
}

static void global_destroy(void *data)
{
	struct impl *impl = data;
	spa_hook_remove(&impl->global_listener);
	impl->global = NULL;
	if (impl->resource)
		pw_resource_destroy(impl->resource);
	free(impl);
}

static const struct pw_global_events global_events = {
	PW_VERSION_GLOBAL_EVENTS,
	.destroy = global_destroy,
};

static void impl_resource_destroy(void *data)
{
	struct impl *impl = data;
	struct param_data *pdata, *tmp;

	spa_hook_remove(&impl->resource_listener);
	spa_hook_remove(&impl->endpoint_listener);
	impl->resource = NULL;

	/* clear cache */
	if (impl->cached_info)
		pw_endpoint_info_free(impl->cached_info);
	spa_list_for_each_safe(pdata, tmp, &impl->cached_params, link) {
		struct spa_pod **pod;
		pw_array_for_each(pod, &pdata->params)
			free(*pod);
		pw_array_clear(&pdata->params);
		spa_list_remove(&pdata->link);
		free(pdata);
	}

	if (impl->global)
		pw_global_destroy(impl->global);
}

static void register_global(struct impl *impl)
{
	impl->cached_info->id = pw_global_get_id (impl->global);
	pw_resource_set_bound_id(impl->resource, impl->cached_info->id);
	pw_global_register(impl->global);
	impl->registered = true;
}

static void impl_resource_pong (void *data, int seq)
{
	struct impl *impl = data;

	/* complete registration, if this was the initial sync */
	if (!impl->registered && seq == impl->ping_seq) {
		register_global(impl);
	}
}

static const struct pw_resource_events impl_resource_events = {
	PW_VERSION_RESOURCE_EVENTS,
	.destroy = impl_resource_destroy,
	.pong = impl_resource_pong,
};

static int emit_info(void *data, struct pw_resource *resource)
{
	const struct pw_endpoint_info *info = data;
	pw_endpoint_resource_info(resource, info);
	return 0;
}

static void event_info(void *data, const struct pw_endpoint_info *info)
{
	struct impl *impl = data;
	uint32_t changed_ids[MAX_PARAMS], n_changed_ids = 0;
	uint32_t i;

	/* figure out changes to params */
	if (info->change_mask & PW_ENDPOINT_CHANGE_MASK_PARAMS) {
		for (i = 0; i < info->n_params; i++) {
			if ((!impl->cached_info ||
				info->params[i].flags != impl->cached_info->params[i].flags)
			    && info->params[i].flags & SPA_PARAM_INFO_READ)
				changed_ids[n_changed_ids++] = info->params[i].id;
		}
	}

	/* cache for new clients */
	impl->cached_info = pw_endpoint_info_update (impl->cached_info, info);

	/* notify existing clients */
	pw_global_for_each_resource(impl->global, emit_info, (void*) info);

	/* cache params & register */
	if (n_changed_ids > 0) {
		/* prepare params storage */
		for (i = 0; i < n_changed_ids; i++) {
			struct param_data *pdata = calloc(1, sizeof(struct param_data));
			pdata->id = changed_ids[i];
			pw_array_init(&pdata->params, sizeof(void*));
			spa_list_append(&impl->cached_params, &pdata->link);
		}

		/* subscribe to impl */
		pw_endpoint_subscribe_params(impl->endpoint, changed_ids, n_changed_ids);

		/* register asynchronously on the pong event */
		impl->ping_seq = pw_resource_ping(impl->resource, 0);
	}
	else if (!impl->registered) {
		register_global(impl);
	}
}

struct param_event_args
{
	uint32_t id, index, next;
	const struct spa_pod *param;
};

static int emit_param(void *_data, struct pw_resource *resource)
{
	struct param_event_args *args = _data;
	struct resource_data *data;
	uint32_t i;

	data = pw_resource_get_user_data(resource);
	for (i = 0; i < data->n_subscribe_ids; i++) {
		if (data->subscribe_ids[i] == args->id) {
			pw_endpoint_resource_param(resource, 1,
				args->id, args->index, args->next, args->param);
		}
	}
	return 0;
}

static void event_param(void *data, int seq,
		       uint32_t id, uint32_t index, uint32_t next,
		       const struct spa_pod *param)
{
	struct impl *impl = data;
	struct param_data *pdata;
	struct spa_pod **pod;
	struct param_event_args args = { id, index, next, param };

	/* cache for new requests */
	spa_list_for_each(pdata, &impl->cached_params, link) {
		if (pdata->id != id)
			continue;

		if (!pw_array_check_index(&pdata->params, index, void*)) {
			while (pw_array_get_len(&pdata->params, void*) <= index)
				pw_array_add_ptr(&pdata->params, NULL);
		}

		pod = pw_array_get_unchecked(&pdata->params, index, struct spa_pod*);
		free(*pod);
		*pod = spa_pod_copy(param);
	}

	/* notify existing clients */
	pw_global_for_each_resource(impl->global, emit_param, &args);
}

static const struct pw_endpoint_events endpoint_events = {
	PW_VERSION_ENDPOINT_EVENTS,
	.info = event_info,
	.param = event_param,
};

static void *endpoint_new(struct pw_context *context,
			  struct pw_resource *resource,
			  struct pw_properties *properties)
{
	struct impl *impl;
	char serial_str[32];
	struct spa_dict_item items[1] = {
		SPA_DICT_ITEM_INIT(PW_KEY_OBJECT_SERIAL, serial_str),
	};
	struct spa_dict extra_props = SPA_DICT_INIT_ARRAY(items);
	static const char * const keys[] = {
		PW_KEY_OBJECT_SERIAL,
		NULL
	};

	impl = calloc(1, sizeof(*impl));
	if (impl == NULL) {
		pw_properties_free(properties);
		return NULL;
	}

	impl->global = pw_global_new(context,
			PW_TYPE_INTERFACE_Endpoint,
			PW_VERSION_ENDPOINT,
			properties,
			global_bind, impl);
	if (impl->global == NULL) {
		free(impl);
		return NULL;
	}
	impl->resource = resource;

	spa_scnprintf(serial_str, sizeof(serial_str), "%"PRIu64,
			pw_global_get_serial(impl->global));
	pw_global_update_keys(impl->global, &extra_props, keys);

	spa_list_init(&impl->cached_params);

	/* handle destroy events */
	pw_global_add_listener(impl->global,
			&impl->global_listener,
			&global_events, impl);
	pw_resource_add_listener(impl->resource,
			&impl->resource_listener,
			&impl_resource_events, impl);

	/* handle implementation events -> cache + client resources */
	pw_endpoint_add_listener(impl->endpoint,
			&impl->endpoint_listener,
			&endpoint_events, impl);

	/* global is not registered here on purpose;
	   we first cache info + params and then expose the global */

	return impl;
}

static void *create_object(void *data,
			   struct pw_resource *resource,
			   const char *type,
			   uint32_t version,
			   struct pw_properties *properties,
			   uint32_t new_id)
{
	struct factory_data *d = data;
	struct pw_resource *impl_resource;
	struct pw_impl_client *client = pw_resource_get_client(resource);
	void *result;
	int res;

	impl_resource = pw_resource_new(client, new_id, PW_PERM_ALL, type, version, 0);
	if (impl_resource == NULL) {
		res = -errno;
		goto error_resource;
	}

	pw_resource_install_marshal(impl_resource, true);

	if (properties == NULL)
		properties = pw_properties_new(NULL, NULL);
	if (properties == NULL) {
		res = -ENOMEM;
		goto error_endpoint;
	}

	pw_properties_setf(properties, PW_KEY_CLIENT_ID, "%d",
			pw_impl_client_get_info(client)->id);
	pw_properties_setf(properties, PW_KEY_FACTORY_ID, "%d",
			pw_impl_factory_get_info(d->factory)->id);

	result = endpoint_new(pw_impl_client_get_context(client), impl_resource, properties);
	if (result == NULL) {
		res = -errno;
		goto error_endpoint;
	}
	return result;

error_resource:
	pw_log_error("can't create resource: %s", spa_strerror(res));
	pw_resource_errorf_id(resource, new_id, res, "can't create resource: %s", spa_strerror(res));
	goto error_exit;
error_endpoint:
	pw_log_error("can't create endpoint: %s", spa_strerror(res));
	pw_resource_errorf_id(resource, new_id, res, "can't create endpoint: %s", spa_strerror(res));
	goto error_exit_free;

error_exit_free:
	pw_resource_remove(impl_resource);
error_exit:
	errno = -res;
	return NULL;
}

static const struct pw_impl_factory_implementation impl_factory = {
	PW_VERSION_IMPL_FACTORY_IMPLEMENTATION,
	.create_object = create_object,
};

static void factory_destroy(void *data)
{
	struct factory_data *d = data;
	spa_hook_remove(&d->factory_listener);
	d->factory = NULL;
	if (d->module)
		pw_impl_module_destroy(d->module);
}

static const struct pw_impl_factory_events factory_events = {
	PW_VERSION_IMPL_FACTORY_EVENTS,
	.destroy = factory_destroy,
};

static void module_destroy(void *data)
{
	struct factory_data *d = data;

	spa_hook_remove(&d->module_listener);
	spa_list_remove(&d->export.link);

	d->module = NULL;
	if (d->factory)
		pw_impl_factory_destroy(d->factory);
}

static void module_registered(void *data)
{
	struct factory_data *d = data;
	struct pw_impl_module *module = d->module;
	struct pw_impl_factory *factory = d->factory;
	struct spa_dict_item items[1];
	char id[16];
	int res;

	snprintf(id, sizeof(id), "%d", pw_impl_module_get_info(module)->id);
	items[0] = SPA_DICT_ITEM_INIT(PW_KEY_MODULE_ID, id);
	pw_impl_factory_update_properties(factory, &SPA_DICT_INIT(items, 1));

	if ((res = pw_impl_factory_register(factory, NULL)) < 0) {
		pw_log_error(NAME" %p: can't register factory: %s", factory, spa_strerror(res));
	}
}

static const struct pw_impl_module_events module_events = {
	PW_VERSION_IMPL_MODULE_EVENTS,
	.destroy = module_destroy,
	.registered = module_registered,
};

int endpoint_factory_init(struct pw_impl_module *module)
{
	struct pw_context *context = pw_impl_module_get_context(module);
	struct pw_impl_factory *factory;
	struct factory_data *data;
	int res;

	factory = pw_context_create_factory(context,
				 "endpoint",
				 PW_TYPE_INTERFACE_Endpoint,
				 PW_VERSION_ENDPOINT,
				 NULL,
				 sizeof(*data));
	if (factory == NULL)
		return -errno;

	data = pw_impl_factory_get_user_data(factory);
	data->factory = factory;
	data->module = module;

	pw_impl_factory_set_implementation(factory, &impl_factory, data);

	data->export.type = PW_TYPE_INTERFACE_Endpoint;
	data->export.func = pw_core_endpoint_export;
	if ((res = pw_context_register_export_type(context, &data->export)) < 0)
		goto error;

	pw_impl_factory_add_listener(factory, &data->factory_listener, &factory_events, data);
	pw_impl_module_add_listener(module, &data->module_listener, &module_events, data);

	return 0;
error:
	pw_impl_factory_destroy(data->factory);
	return res;
}
