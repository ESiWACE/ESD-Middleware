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
 * @brief The scheduler receives application requests and schedules subsequent
 *        I/O requests as a are necessary for metadata lookups and data
 *        reconstructions.
 *
 */


#include <esdm.h>
#include <esdm-internal.h>

#include <stdio.h>
#include <stdlib.h>

#include <glib.h>

#define DEBUG_ENTER ESDM_DEBUG_COM_FMT("SCHEDULER", "", "")
#define DEBUG(fmt, ...) ESDM_DEBUG_COM_FMT("SCHEDULER", fmt, __VA_ARGS__)

static void backend_thread(io_work_t* data_p, esdm_backend_t* backend_id);

esdm_scheduler_t* esdm_scheduler_init(esdm_instance_t* esdm)
{
	ESDM_DEBUG(__func__);

	esdm_scheduler_t* scheduler = NULL;
	scheduler = (esdm_scheduler_t*) malloc(sizeof(esdm_scheduler_t));

  // create thread pools per device
  // decide how many threads should be used per backend.
  const int ppn = esdm->procs_per_node;
  const int gt = esdm->total_procs;
  GError * error;
	for (int i = 0; i < esdm->modules->backend_count; i++) {
		esdm_backend_t* b = esdm->modules->backends[i];
		// in total we should not use more than max_global total threads
		int max_local = (b->config->max_threads_per_node + ppn - 1) / ppn;
		int max_global = (b->config->max_global_threads + gt - 1) / gt;

    b->threads = max_local < max_global ? max_local : max_global;
		DEBUG("Using %d threads for backend %s", b->threads, b->config->id);

    if (b->threads == 0){
      b->threadPool = NULL;
    }else{
      b->threadPool = g_thread_pool_new((GFunc)(backend_thread), b, b->threads, 1, & error);
    }
  }

	return scheduler;
}

esdm_status_t esdm_scheduler_finalize(esdm_instance_t *esdm)
{
  for (int i = 0; i < esdm->modules->backend_count; i++) {
    esdm_backend_t* b = esdm->modules->backends[i];
    if(b->threadPool){
      g_thread_pool_free(b->threadPool, 0, 1);
    }
  }
	ESDM_DEBUG(__func__);
	return ESDM_SUCCESS;
}

static void backend_thread(io_work_t* work, esdm_backend_t* backend){
  io_request_status_t * status = work->parent;

  printf("backend thread operates on %s via %s \n", backend->name, backend->config->target);

  assert(backend == work->fragment->backend);

  esdm_status_t ret;
  switch(work->op){
    case(ESDM_OP_READ):{
      ret = esdm_fragment_retrieve(work->fragment);
      break;
    }
    case(ESDM_OP_WRITE):{
      ret = esdm_fragment_commit(work->fragment);
      break;
    }
  }

  work->return_code = ret;

  g_mutex_lock(& status->mutex);
  status->pending_ops--;
  assert(status->pending_ops >= 0);
  if( status->pending_ops == 0){
    g_cond_signal(& status->done_condition);
  }
  g_mutex_unlock(& status->mutex);
	esdm_dataspace_destroy(work->fragment->dataspace);
  free(work);
}


esdm_status_t esdm_scheduler_enqueue(esdm_instance_t *esdm, io_request_status_t * status, io_operation_t op, esdm_dataset_t *dataset, void *buf,  esdm_dataspace_t* space){
    GError * error;
    //Gather I/O recommendations
    //esdm_performance_recommendation(esdm, NULL, NULL);    // e.g., split, merge, replication?
    //esdm_layout_recommendation(esdm, NULL, NULL);		  // e.g., merge, split, transform?

		// how big is one sub-hypercube? we call it y axis for the easier reading
    uint64_t one_y_size = esdm_buffer_offset_first_dimension(space, 1);
		if (one_y_size == 0){
			return ESDM_SUCCESS;
		}

		uint64_t y_count = space->size[0];
		uint64_t per_backend[esdm->modules->backend_count];

		memset(per_backend, 0, sizeof(per_backend));

		while(y_count > 0){
			for (int i = 0; i < esdm->modules->backend_count; i++) {
				status->pending_ops++;
				esdm_backend_t* b = esdm->modules->backends[i];
				// how many of these fit into our buffer
				uint64_t backend_y_per_buffer = b->config->max_fragment_size / one_y_size;
				if (backend_y_per_buffer == 0){
					backend_y_per_buffer = 1;
				}
				if (backend_y_per_buffer >= y_count){
					per_backend[i] += y_count;
					y_count = 0;
					break;
				}else{
					per_backend[i] += backend_y_per_buffer;
					y_count -= backend_y_per_buffer;
				}
			}
		}
		ESDM_DEBUG_FMT("Will submit %d operations and for backend0: %d y-blocks", status->pending_ops, per_backend[0]);

		uint64_t offset_y = 0;
		int64_t dim[space->dimensions];
		int64_t offset[space->dimensions];
		memset(offset, 0, space->dimensions * sizeof(int64_t));
		memcpy(dim, space->size, space->dimensions * sizeof(int64_t));

    for (int i = 0; i < esdm->modules->backend_count; i++) {
			esdm_backend_t* b = esdm->modules->backends[i];
			// how many of these fit into our buffer
			uint64_t backend_y_per_buffer = b->config->max_fragment_size / one_y_size;

			uint64_t y_total_access = per_backend[i];
			while(y_total_access > 0){
				uint64_t y_to_access = y_total_access > backend_y_per_buffer ? backend_y_per_buffer : y_total_access ;
				y_total_access -= y_to_access;

				dim[0] = y_to_access;
				offset[0] = offset_y;

	      io_work_t * task = (io_work_t*) malloc(sizeof(io_work_t));
				esdm_dataspace_t* subspace = esdm_dataspace_subspace(space, 2, dim, offset);

	      task->parent = status;
	      task->op = op;
	      task->fragment = esdm_fragment_create(dataset, subspace, (char*) buf + offset_y * one_y_size);
	      task->fragment->backend = b;
	      if (b->threads == 0){
	        backend_thread(task, b);
	      }else{
	        g_thread_pool_push(b->threadPool, task, & error);
	      }

				offset_y += y_to_access;
			}
    }

    // now enqueue the operations
    return ESDM_SUCCESS;
}

esdm_status_t esdm_scheduler_status_init(io_request_status_t * status){
  g_mutex_init(& status->mutex);
  g_cond_init(& status->done_condition);
  status->pending_ops = 0;
  return ESDM_SUCCESS;
}

esdm_status_t esdm_scheduler_status_finalize(io_request_status_t * status){
  g_mutex_clear(& status->mutex);
  g_cond_clear(& status->done_condition);
  return ESDM_SUCCESS;
}

esdm_status_t esdm_scheduler_wait(io_request_status_t * status){
    g_mutex_lock(& status->mutex);
    if (status->pending_ops){
      g_cond_wait(& status->done_condition, & status->mutex);
    }
    g_mutex_unlock(& status->mutex);
    return ESDM_SUCCESS;
}

esdm_status_t esdm_scheduler_process_blocking(esdm_instance_t *esdm, io_operation_t op, esdm_dataset_t *dataset, void *buf,  esdm_dataspace_t* subspace){
	ESDM_DEBUG(__func__);

  io_request_status_t status;

	esdm_status_t ret;

  ret = esdm_scheduler_status_init(& status);
  assert( ret == ESDM_SUCCESS );

  ret = esdm_scheduler_enqueue(esdm, & status, op, dataset, buf, subspace);
  assert( ret == ESDM_SUCCESS );

  ret = esdm_scheduler_wait(& status);
  assert( ret == ESDM_SUCCESS );
  ret = esdm_scheduler_status_finalize(& status);
  assert( ret == ESDM_SUCCESS );
	return ESDM_SUCCESS;
}





esdm_status_t esdm_backend_io(esdm_backend_t* backend, esdm_fragment_t* fragment, esdm_metadata_t* metadata)
{
	ESDM_DEBUG(__func__);

	return ESDM_SUCCESS;
}
