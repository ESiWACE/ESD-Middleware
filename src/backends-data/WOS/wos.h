/* This file is part of ESDM.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with ESDM.  If not, see <http://www.gnu.org/licenses/>.
 *
 */
#ifndef ESDM_BACKENDS_WOS_H
#define ESDM_BACKENDS_WOS_H

#include <esdm-internal.h>
#include <backends-data/dynamic-perf-model/lat-thr.h>

#include "wrapper/wos_wrapper.h"

// Internal functions used by this backend.
typedef struct {

	esdm_config_backend_t *config;
	const char *target;
	esdm_dynamic_perf_model_lat_thp_t perf_model;

	t_WosClusterPtr *wos_cluster;	// Pointer to WOS cluster
	t_WosPolicy *wos_policy;	// Policy
	t_WosObjPtr *wos_meta_obj;	// Used for internal purposes to store metadata info
	t_WosOID **oid_list;	// List of object ids
	uint64_t *size_list;	// List of object sizes

} wos_backend_data_t;
typedef wos_backend_data_t esdm_backend_wos_t;

/**
* Initializes the plugin. In particular this involves:
*
*	* Load configuration of this backend
*	* Load and potenitally calibrate performance model
*
*	* Connect with support services e.g. for technical metadata
*	* Setup directory structures used by this wos specific backend
*
*	* Popopulate esdm_backend struct and callbacks required for registration
*
* @return pointer to backend struct
*/

int wos_get_param(const char *conf, char **output, const char *param);

int wos_get_host(const char *conf, char **host);

int wos_get_policy(const char *conf, char **policy);

void wos_delete_oid_list(esdm_backend_wos_t * ebm);

int wos_object_list_encode(t_WosOID ** oid_list, char **out_object_id);

int esdm_backend_wos_init(const char *conf, esdm_backend * eb);

int esdm_backend_wos_fini(esdm_backend * eb);

int esdm_backend_wos_alloc(esdm_backend * eb, int n_dims, int *dims_size, esdm_datatype_t type, char **out_object_id, char **out_wos_metadata);

int esdm_backend_wos_open(esdm_backend * eb, char *object_id, void **obj_handle);

int esdm_backend_wos_delete(esdm_backend * eb, void *obj_handle);

int esdm_backend_wos_write(esdm_backend * eb, void *obj_handle, uint64_t start, uint64_t count, esdm_datatype_t type, void *data);

int esdm_backend_wos_read(esdm_backend * eb, void *obj_handle, uint64_t start, uint64_t count, esdm_datatype_t type, void *data);

int esdm_backend_wos_close(esdm_backend * eb, void *obj_handle);

int wos_backend_performance_check(esdm_backend *eb, int data_size, float *out_time);

int wos_backend_performance_estimate(esdm_backend * eb, esdm_fragment_t * fragment, float *out_time);

int esdm_backend_wos_fragment_retrieve(esdm_backend * backend, esdm_fragment_t * fragment, json_t * metadata);

int esdm_backend_wos_fragment_update(esdm_backend * backend, esdm_fragment_t * fragment);

int esdm_backend_wos_fragment_delete(esdm_backend * backend, esdm_fragment_t * fragment, json_t * metadata);

int esdm_backend_wos_fragment_mkfs(esdm_backend * backend, int enforce_format);

esdm_backend *wos_backend_init(esdm_config_backend_t * config);

extern esdm_backend_wos_t esdm_backend_wos;

#endif
