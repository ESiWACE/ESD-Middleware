/* This file is part of ESDM.
 *
 * This program is is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with ESDM.  If not, see <http://www.gnu.org/licenses/>.
 */


/**
 * @file
 * @brief Entry point for ESDM API Implementation
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <esdm.h>
#include <esdm-internal.h>



// TODO: Decide on initialization mechanism.
static int is_initialized = 0;


esdm_instance_t esdm = {
	.procs_per_node = 1,
	.total_procs = 1,
	.config = NULL,
};

esdm_status esdm_set_procs_per_node(int procs){
	assert(procs > 0);
	esdm.procs_per_node = procs;
	return ESDM_SUCCESS;
}

esdm_status esdm_set_total_procs(int procs){
	assert(procs > 0);
	esdm.total_procs = procs;
	return ESDM_SUCCESS;
}


esdm_status esdm_load_config_str(const char * str){
	assert(str != NULL);
	esdm.config = esdm_config_init_from_str(str);
	return ESDM_SUCCESS;
}



/*
void esdm_atexit() {
	esdm_finalize();
}
*/

esdm_dataspace_t * esdm_dataset_get_dataspace(esdm_dataset_t *dset){
	return dset->dataspace;
}


/**
 * Initialize ESDM:
 *	- allocate data structures for ESDM
 *	- allocate memory for node local caches
 *	- initialize submodules
 *	- initialize threadpool
 *
 * @return status
 */
esdm_status esdm_init()
{
	ESDM_DEBUG("Init");

	if (!is_initialized) {
		ESDM_DEBUG("Initializing ESDM");


		//int status = atexit(esdm_atexit);


		// find configuration
		if ( !esdm.config )
			esdm_config_init(&esdm);

		// optional modules (e.g. data and metadata backends)
		esdm_modules_init(&esdm);

		// core components
		esdm_layout_init(&esdm);
		esdm_performance_init(&esdm);
		esdm_scheduler_init(&esdm);

		ESDM_DEBUG_COM_FMT("ESDM", " esdm = {config = %p, modules = %p, scheduler = %p, layout = %p, performance = %p}\n",
						  (void*)esdm.config,
						  (void*)esdm.modules,
						  (void*)esdm.scheduler,
						  (void*)esdm.layout,
						  (void*)esdm.performance);

		is_initialized = 1;

		ESDM_DEBUG("ESDM initialized and ready!");
	}

	return ESDM_SUCCESS;
}




/**
 * Initialize backend by invoking mkfs callback for matching target
 *
 * @param [in] enforce_format  force reformatting existing system (may result in data loss)
 * @param [in] target  target descriptor
 *
 * @return Status
 */
esdm_status esdm_mkfs(int enforce_format, data_accessibility_t target){
	if(! is_initialized){
		return ESDM_ERROR;
	}
	int ret;
	int ret_final = ESDM_SUCCESS;
	if(esdm.modules->metadata->config->data_accessibility == target){
		ret = esdm.modules->metadata->callbacks.mkfs(esdm.modules->metadata, enforce_format);
		if (ret != ESDM_SUCCESS){
			ret_final = ret;
		}
	}

	for(int i=0; i < esdm.modules->backend_count; i++){
		if(esdm.modules->backends[i]->config->data_accessibility == target){
			ret = esdm.modules->backends[i]->callbacks.mkfs(esdm.modules->backends[i], enforce_format);
			if (ret != ESDM_SUCCESS){
				ret_final = ret;
			}
		}
	}
	return ret_final;
}


/**
 * Display status information for objects stored in ESDM.
 *
 * @param [in] desc	Name or descriptor of object.
 *
 * @return Status
 */
esdm_status esdm_finalize()
{
	ESDM_DEBUG(__func__);


	// ESDM data data structures that require proper cleanup..
	// in particular this effects data and cache state which is not yet persistent

	esdm_scheduler_finalize(&esdm);
	esdm_performance_finalize(&esdm);
	esdm_layout_finalize(&esdm);
	esdm_modules_finalize(&esdm);

	return ESDM_SUCCESS;
}








///////////////////////////////////////////////////////////////////////////////
// Public API: POSIX Legacy Compaitbility /////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////

/**
 * Display status information for objects stored in ESDM.
 *
 * @param [in]	desc	name or descriptor of object
 * @param [out]	result	where to write result of query
 *
 * @return Status
 */
esdm_status esdm_stat(char *desc, char *result)
{
	ESDM_DEBUG(__func__);

	esdm_init();

	esdm_layout_stat(desc);

	return ESDM_SUCCESS;
}


/**
 * Create a new object.
 *
 * @param [in]	desc		string object identifier
 * @param [in]	mode		mode flags for creation
 * @param [out] container	pointer to new container
 *
 * @return Status
 */
esdm_status esdm_create(char *name, int mode, esdm_container **container, esdm_dataset_t **dataset)
{
	ESDM_DEBUG(__func__);

	esdm_init();


	int64_t bounds[1] = {0};
	esdm_dataspace_t *dataspace = esdm_dataspace_create(1 /* 1D */ , bounds, SMD_DTYPE_INT8);

	*container = esdm_container_create(name);
	*dataset = esdm_dataset_create(*container, "bytestream", dataspace);

	printf("Dataset 'bytestream' creation: %p\n", (void*) *dataset);

	esdm_dataset_commit(*dataset);
	esdm_container_commit(*container);

	return ESDM_SUCCESS;
}


/**
 * Open a existing object.
 *
 * TODO: decide if also useable to create?
 *
 * @param [in] desc		string object identifier
 * @param [in] mode		mode flags for open/creation
 *
 * @return Status
 */
esdm_status esdm_open(char *name, int mode)
{
	ESDM_DEBUG(__func__);

	esdm_init();

	return ESDM_SUCCESS;
}


/**
 * Write data  of size starting from offset.
 *
 * @param [in] buf	The pointer to a contiguous memory region that shall be written
 * @param [in] dset	TODO, currently a stub, we assume it has been identified/created before...., json description?
 * @param [in] dims	The number of dimensions, needed for size and offset
 * @param [in] size	...
 *
 * @return Status
 */

esdm_status esdm_write(esdm_dataset_t *dataset, void *buf, esdm_dataspace_t* subspace)
{
	ESDM_DEBUG(__func__);

	return esdm_scheduler_process_blocking(& esdm, ESDM_OP_WRITE, dataset, buf, subspace);
}



/**
 * Reads a data fragment described by desc to the dataset dset.
 *
 * @param [out] buf	The pointer to a contiguous memory region that shall be written
 * @param [in] dset	TODO, currently a stub, we assume it has been identified/created before.... , json description?
 * @param [in] dims	The number of dimensions, needed for size and offset
 * @param [in] size	...
 *
 * @return Status
 */
esdm_status esdm_read(esdm_dataset_t *dataset, void *buf, esdm_dataspace_t* subspace)
{
	ESDM_DEBUG("");

	return esdm_scheduler_process_blocking(& esdm, ESDM_OP_READ, dataset, buf, subspace);
}



/**
 * Close opened object.
 *
 * @param [in] desc		String Object Identifier
 *
 * @return Status
 */
esdm_status esdm_close(void *desc)
{
	ESDM_DEBUG(__func__);


	return ESDM_SUCCESS;
}



/**
 * Ensure all remaining data is syncronized with backends.
 * If not called at the end of an application, ESDM can not guarantee all data
 * was written.
 *
 * @return Status
 */
esdm_status esdm_sync()
{
	ESDM_DEBUG(__func__);
	return ESDM_SUCCESS;
}
