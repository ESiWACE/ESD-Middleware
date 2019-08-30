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
 */

/**
 * @file
 * @brief The scheduler receives application requests and schedules subsequent
 *        I/O requests as are necessary for metadata lookups and data
 *        reconstructions.
 *
 */

#include <esdm-internal.h>
#include <esdm.h>
#include <glib.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DEBUG_ENTER ESDM_DEBUG_COM_FMT("SCHEDULER", "", "")
#define DEBUG(fmt, ...) ESDM_DEBUG_COM_FMT("SCHEDULER", fmt, __VA_ARGS__)

static void backend_thread(io_work_t *data_p, esdm_backend_t *backend_id);

esdm_scheduler_t *esdm_scheduler_init(esdm_instance_t *esdm) {
  ESDM_DEBUG(__func__);

  esdm_scheduler_t *scheduler = NULL;
  scheduler = (esdm_scheduler_t *)malloc(sizeof(esdm_scheduler_t));

  // create thread pools per device
  // decide how many threads should be used per backend.
  const int ppn = esdm->procs_per_node;
  const int gt = esdm->total_procs;
  GError *error;
  for (int i = 0; i < esdm->modules->data_backend_count; i++) {
    esdm_backend_t *b = esdm->modules->data_backends[i];
    // in total we should not use more than max_global total threads
    int max_local = (b->config->max_threads_per_node + ppn - 1) / ppn;
    int max_global = (b->config->max_global_threads + gt - 1) / gt;

    if (b->config->data_accessibility == ESDM_ACCESSIBILITY_GLOBAL) {
      b->threads = max_local < max_global ? max_local : max_global;
    } else {
      b->threads = max_local;
    }
    DEBUG("Using %d threads for backend %s", b->threads, b->config->id);

    if (b->threads == 0) {
      b->threadPool = NULL;
    } else {
      b->threadPool = g_thread_pool_new((GFunc)(backend_thread), b, b->threads, 1, &error);
    }
  }

  esdm->scheduler = scheduler;
  return scheduler;
}

esdm_status esdm_scheduler_finalize(esdm_instance_t *esdm) {
  ESDM_DEBUG(__func__);

  if(esdm->modules){
    for (int i = 0; i < esdm->modules->data_backend_count; i++) {
      esdm_backend_t *b = esdm->modules->data_backends[i];
      if (b->threadPool) {
        g_thread_pool_free(b->threadPool, 0, 1);
      }
    }
  }

  if (esdm->scheduler) {
    free(esdm->scheduler);
    esdm->scheduler = NULL;
  }

  return ESDM_SUCCESS;
}

static void backend_thread(io_work_t *work, esdm_backend_t *backend) {
  io_request_status_t *status = work->parent;

  DEBUG("Backend thread operates on %s via %s", backend->name, backend->config->target);

  eassert(backend == work->fragment->backend);

  esdm_status ret;
  switch (work->op) {
    case (ESDM_OP_READ): {
      ret = esdm_fragment_retrieve(work->fragment);
      break;
    }
    case (ESDM_OP_WRITE): {
      ret = esdm_fragment_commit(work->fragment);
      break;
    }
    default:
      ret = ESDM_ERROR;
  }

  work->return_code = ret;

  if (work->callback) {
    work->callback(work);
  }

  g_mutex_lock(&status->mutex);
  status->pending_ops--;
  if (ret != ESDM_SUCCESS){
    status->return_code = ret;
  }
  eassert(status->pending_ops >= 0);
  if (status->pending_ops == 0) {
    g_cond_signal(&status->done_condition);
  }
  g_mutex_unlock(&status->mutex);
  //esdm_dataspace_destroy(work->fragment->dataspace);

  work->fragment->status = ESDM_DATA_NOT_LOADED;
  free(work);
}

static int64_t min_int64(int64_t a, int64_t b) {
  return a < b ? a : b;
}

static int64_t abs_int64(int64_t a) {
  return a > 0 ? a : -a;
}

/**
 * Generate some, hopefully dimensionally reduced, instructions to perform `memcpy()` calls.
 * This is a helper function that is used to implement both `esdm_dataspace_copy_data()`
 * and support `esdmI_scheduler_try_direct_io()` by determining how many `memcpy()` calls would actually be needed to perform the copy.
 *
 * @param [in] sourceSpace the data space for the source buffer
 * @param [in] destSpace the data space for the destination buffer
 * @param [out] instructionDims returns the number of dimensions over which we need to iterate, may be zero (single `memcpy()` call), or -1 (nothing to copy)
 * @param [out] sourceOffset the offset of the first data chunk to read
 * @param [out] destOffset the offset of the first data chunk to write
 * @param [out] chunkSize number of bytes to copy with each `memcpy()` call
 * @param [out] size[instructionDims] the count of slices for each of the remaining dimensions
 * @param [out] relativeSourceStride[instructionDims] relative strides for the source buffer
 * @param [out] relativeDestStride[instructionDims] relative strides for the destination buffer
 *
 * The relative strides are defined as follows:
 * If the next dimension has a size of n, the relative stride is the offset in bytes between the slice at (..., 0, n, ...) and (..., 1, 0, ...).
 * That is, after the next dimension has been incremented to the one-past-the-end element, the relative stride is simply added to get the first element of the next slice.
 *
 * Or, viewing it the other way round, we go from the normal stride to the relative stride by
 * a) converting to byte offsets, and b) by subtracting the size of each dimension from the previous stride.
 * I.e. if the normal stride is (100, 10, 1) and the size is (10, 10, 10), the relative stride is (0, 0, 1), assuming an element size of 1.
 * Likewise, if the normal stride is (1000, 42, 3) and the size is (10, 10, 10), the relative stride is (580, 12, 1).
 * If the elements were of size 8, the relative stride would be (4640, 96, 8).
 * The point of using this relative stride is, that we can remove all state from the inner loop except for two simple `char` pointers.
 */
static void esdmI_dataspace_copy_instructions(
    esdm_dataspace_t* sourceSpace,
    esdm_dataspace_t* destSpace,
    int64_t* out_instructionDims,
    int64_t* out_chunkSize,
    int64_t* out_sourceOffset,
    int64_t* out_destOffset,
    int64_t* out_size,  //actually an array of size `*out_instructionDims`, may be NULL
    int64_t* out_relSourceStride,  //actually an array of size `*out_instructionDims`, may be NULL
    int64_t* out_relDestStride  //actually an array of size `*out_instructionDims`, may be NULL
) {
  eassert(sourceSpace->dims == destSpace->dims);
  eassert(sourceSpace->type == destSpace->type);
  eassert(out_instructionDims);
  eassert(out_chunkSize);
  eassert(out_sourceOffset);
  eassert(out_destOffset);

  uint64_t dimensions = sourceSpace->dims;
  esdmI_hypercube_t *sourceExtends, *destExtends;
  esdmI_dataspace_getExtends(sourceSpace, &sourceExtends);
  esdmI_dataspace_getExtends(destSpace, &destExtends);

  //determine the hypercube that contains the overlap between the two hypercubes
  //(the intersection of two hypercubes is also a hypercube)
  esdmI_hypercube_t* overlap = esdmI_hypercube_makeIntersection(sourceExtends, destExtends);
  if(!overlap) return;  //overlap is empty => nothing to do
  int64_t overlapOffset[dimensions];
  int64_t overlapSize[dimensions];
  esdmI_hypercube_getOffsetAndSize(overlap, overlapOffset, overlapSize);
  esdmI_hypercube_destroy(overlap);

  //in case the stride fields are set to NULL, determine the effective strides
  int64_t sourceStride[dimensions], destStride[dimensions];
  esdm_dataspace_getEffectiveStride(sourceSpace, sourceStride);
  esdm_dataspace_getEffectiveStride(destSpace, destStride);

  //determine how much data we can move with memcpy() at a time
  int64_t dataPointerOffset = 0, memcpySize = 1;  //both are counts of fundamental elements, not bytes
  bool memcpyDims[dimensions];
  memset(memcpyDims, 0, sizeof(memcpyDims));
  while(true) {
    //search for a dimension with a stride that matches our current memcpySize
    int64_t curDim;
    for(curDim = dimensions; curDim--; ) {
      if(!memcpyDims[curDim] && sourceStride[curDim] == destStride[curDim]) {
        if(abs_int64(sourceStride[curDim]) == memcpySize) break;
      }
    }
    if(curDim >= 0) {
      //found a fitting dimension, update our parameters
      memcpyDims[curDim] = true;  //remember not to loop over this dimension when copying the data
      if(sourceStride[curDim] < 0) dataPointerOffset += memcpySize*(overlapSize[curDim] - 1); //When the stride is negative, the first byte belongs to the last slice of this dimension, not the first one. Remember that.
      memcpySize *= overlapSize[curDim];
      if(overlapSize[curDim] != sourceSpace->size[curDim] || overlapSize[curDim] != destSpace->size[curDim]) break; //cannot fuse other dimensions in a memcpy() call if this dimensions does not have a perfect match between the dataspaces
    } else break; //didn't find another suitable dimension for fusing memcpy() calls
  }

  //determine the order of the remaining dimensions
  bool selectedDims[dimensions];
  memcpy(selectedDims, memcpyDims, sizeof(selectedDims));
  int64_t memcpyDim2LogicalDim[dimensions];
  for(*out_instructionDims = 0; ; (*out_instructionDims)++) {
    int64_t bestDim = -1;
    int64_t jumpFactor = 0;
    for(int64_t dim = 0; dim < dimensions; dim++) {
      if(!selectedDims[dim]) {
        int64_t curJumpFactor = min_int64(abs_int64(sourceStride[dim]), abs_int64(destStride[dim]));
        if(bestDim < 0 || jumpFactor < curJumpFactor) {
          bestDim = dim;
          jumpFactor = curJumpFactor;
        }
      }
    }
    if(bestDim >= 0) {
      selectedDims[bestDim] = true;
      memcpyDim2LogicalDim[*out_instructionDims] = bestDim;
    } else {
      break;
    }
  }

  //create the instruction data
  int64_t trashSize[dimensions], trashRelSourceStride[dimensions], trashRelDestStride[dimensions];
  if(!out_size) out_size = trashSize;
  if(!out_relSourceStride) out_relSourceStride = trashRelSourceStride;
  if(!out_relDestStride) out_relDestStride = trashRelDestStride;
  uint64_t elementSize = esdm_sizeof(sourceSpace->type);
  for(int64_t memcpyDim = 0; memcpyDim < *out_instructionDims; memcpyDim++) {
    int64_t logicalDim = memcpyDim2LogicalDim[memcpyDim];
    out_size[memcpyDim] = overlapSize[logicalDim];
    out_relSourceStride[memcpyDim] = sourceStride[logicalDim]*elementSize;
    out_relDestStride[memcpyDim] = destStride[logicalDim]*elementSize;
    if(memcpyDim) {
      //make the previous stride relative to this stride
      out_relSourceStride[memcpyDim-1] -= out_size[memcpyDim]*out_relSourceStride[memcpyDim];
      out_relDestStride[memcpyDim-1] -= out_size[memcpyDim]*out_relDestStride[memcpyDim];
    }
  }
  *out_chunkSize = memcpySize*elementSize;
  int64_t sourceIndex = 0, destIndex = 0;
  for(int64_t i = 0; i < dimensions; i++) {
    sourceIndex += (overlapOffset[i] - sourceSpace->offset[i])*sourceStride[i];
    destIndex += (overlapOffset[i] - destSpace->offset[i])*destStride[i];
  }
  *out_sourceOffset = (sourceIndex - dataPointerOffset)*elementSize;
  *out_destOffset = (destIndex - dataPointerOffset)*elementSize;
}

esdm_status esdm_dataspace_copy_data(esdm_dataspace_t* sourceSpace, void *voidPtrSource, esdm_dataspace_t* destSpace, void *voidPtrDest) {
  eassert(sourceSpace->dims == destSpace->dims);
  eassert(sourceSpace->type == destSpace->type);

  char* sourceData = voidPtrSource;
  char* destData = voidPtrDest;
  uint64_t dimensions = sourceSpace->dims;

  //get the instructions on how to copy the data
  int64_t instructionDims, chunkSize, sourceOffset, destOffset, size[dimensions], relSourceStride[dimensions], relDestStride[dimensions];
  esdmI_dataspace_copy_instructions(sourceSpace, destSpace, &instructionDims, &chunkSize, &sourceOffset, &destOffset, size, relSourceStride, relDestStride);

  //execute the instructions
  if(instructionDims < 0) return ESDM_SUCCESS;  //nothing to do
  sourceData += sourceOffset;
  destData += destOffset;
  int64_t counters[instructionDims];
  memset(counters, 0, sizeof(counters));
  while(true) {
    memcpy(destData, sourceData, chunkSize);

    int64_t i;
    for(i = instructionDims; i--; ) {
      sourceData += relSourceStride[i];
      destData += relDestStride[i];
      if(++(counters[i]) < size[i]) break;
      counters[i] = 0;
    }
    if(i == -1) break;
  }

  return ESDM_SUCCESS;
}

static void read_copy_callback(io_work_t *work) {
  if (work->return_code != ESDM_SUCCESS) {
    DEBUG("Error reading from fragment ", work->fragment);
    return;
  }
  esdm_dataspace_copy_data(work->fragment->dataspace, work->fragment->buf, work->data.buf_space, work->data.mem_buf);
  free(work->fragment->buf);  //TODO: Decide whether to cache the data.
  work->fragment->buf = NULL;
}

bool esdmI_scheduler_try_direct_io(esdm_fragment_t *f, void * buf, esdm_dataspace_t * da){
  eassert(f->dataspace->dims == da->dims);
  eassert(f->dataspace->type == da->type);

  int64_t instructionDims, chunkSize, sourceOffset, destOffset;
  esdmI_dataspace_copy_instructions(f->dataspace, da, &instructionDims, &chunkSize, &sourceOffset, &destOffset, NULL, NULL, NULL);
  if(instructionDims < 0) return true; //no overlap, nothing to do, we did it successfully
  if(instructionDims > 0) return false; //copy cannot be reduced to a single memcpy() call, so direct I/O is impossible

  //Ok, only a single memcpy() would be needed to move the data.
  //Determine whether the entire fragment's data would be needed.
  if(esdm_dataspace_size(f->dataspace) == chunkSize) {
    //The entire fragment's data is used in one piece.
    //Setup it's data buffers' pointer so that the backend will directly fill the correct part of the users' buffer.
    eassert(sourceOffset == 0 && "we have determined that we need the entire fragments data, so there should be no cut-off at the beginning");
    f->buf = (char*)buf + destOffset;
    return true;
  } else {
    //The fragments data is only used partially.
    //Thus, direct I/O is impossible as it would overwrite data next to the needed portion.
    return false;
  }
}

esdm_status esdm_scheduler_enqueue_read(esdm_instance_t *esdm, io_request_status_t *status, int frag_count, esdm_fragment_t **read_frag, void *buf, esdm_dataspace_t *buf_space) {
  GError *error;

  status->pending_ops += frag_count;

  for (int i = 0; i < frag_count; i++) {
    esdm_fragment_t *f = read_frag[i];
    uint64_t size = esdm_dataspace_size(f->dataspace);
    esdm_backend_t *backend_to_use = f->backend;

    io_work_t *task = (io_work_t *)malloc(sizeof(io_work_t));
    task->parent = status;
    task->op = ESDM_OP_READ;
    task->fragment = f;
    if (esdmI_scheduler_try_direct_io(f, buf, buf_space)) {
      task->callback = NULL;
    } else {
      //We cannot instruct the fragment to read the data directly into `buf` as we may only need a part of the fragment's data, and the overshoot may cause UB.
      //And we don't want to allocate more memory here than just enough to actually read the fragment's data, so we cannot use the fragment's possibly strided dataspace.
      //However, we can ensure that the fragment can read directly into our buffer by supplying an unstrided dataspace.
      esdm_dataspace_t* contiguousSpace;
      esdm_dataspace_makeContiguous(f->dataspace, &contiguousSpace);
      esdm_dataspace_destroy(f->dataspace);
      f->dataspace = contiguousSpace;

      f->buf = malloc(size);  //This buffer will be filled by some background I/O process, read_copy_callback() will only be invoked *after* that has happened.
      task->callback = read_copy_callback;
      task->data.mem_buf = buf;
      task->data.buf_space = buf_space;
    }
    if (backend_to_use->threads == 0) {
      backend_thread(task, backend_to_use);
    } else {
      g_thread_pool_push(backend_to_use->threadPool, task, &error);
    }
  }

  return ESDM_SUCCESS;
}

static esdm_status esdm_scheduler_enqueue_fill(esdm_instance_t* esdm, io_request_status_t* status, void* fillValue, void* buf, esdm_dataspace_t* bufSpace, esdmI_hypercubeList_t* fillRegion) {
  //I would really love to make the fill operation asynchronous.
  //However, it does not make any sense to use one of our backend threads, because the backends are concerned with storage, not with pure in-memory operations.
  //And I don't know about any non-backend mechanisms in ESDM yet that allow for asynchronous execution.
  //Of course, it's not such a big deal if fill value painting is performed synchronously, as it should be a memory bound operation anyways,
  //and we don't expect it to take excessive amounts of time like I/O does.
  //
  //TODO Check whether such non-backend mechanisms exist already, and decide whether we really want to make fill-value setting asynchronous.

  esdm_status ret;
  int64_t dimensions = esdm_dataspace_get_dims(bufSpace);
  esdm_type_t type = esdm_dataspace_get_type(bufSpace);

  //create a dataspace with stride zero (all logical elements are mapped to the one and only element in memory) which we use as a source in the subsequent copy operations
  esdm_dataspace_t* sourceSpace;
  int64_t size[dimensions], stride[dimensions];
  for(int64_t i = 0; i < dimensions; i++) {
    size[i] = INT64_MAX;
    stride[i] = 0;
  }
  ret = esdm_dataspace_create(dimensions, size, type, &sourceSpace);
  eassert(ret == ESDM_SUCCESS);
  ret = esdm_dataspace_set_stride(sourceSpace, stride);
  eassert(ret == ESDM_SUCCESS);

  //for each hypercube in the set, copy the fill value to the corresponding bufSpace area
  for(int64_t i = 0; i < fillRegion->count; i++) {
    esdmI_hypercube_t* curCube = fillRegion->cubes[i];
    eassert(curCube->dims == dimensions);

    //adjust the extends of the source dataspace to select which part of the buffer we want to fill
    ret = esdmI_dataspace_setExtends(sourceSpace, curCube);
    eassert(ret == ESDM_SUCCESS);

    //fill the hypercube
    esdm_dataspace_copy_data(sourceSpace, fillValue, bufSpace, buf);
    eassert(ret == ESDM_SUCCESS);
  }

  return ESDM_SUCCESS;
}

//Implementation of `esdm_scheduler_makeSplitRecommendation()` that tries to produce fragments that are about as wide as high/long/deep/... .
static esdmI_hypercubeSet_t* makeSplitRecommendation_balancedDims(esdm_dataspace_t* space, int64_t maxFragmentSize) {
  eassert(space);

  esdmI_hypercubeSet_t* result = esdmI_hypercubeSet_make();

  //determine the count of splitable dimensions (length > 1)
  uint64_t splitDims = 0;
  for(int64_t i = 0; i < space->dims; i++) {
    if(space->size[i] > 1) splitDims++;
  }
  if(!splitDims) {
    //only a single element, use a trivial recommendation
    esdmI_hypercube_t* cube;
    esdmI_dataspace_getExtends(space, &cube);
    esdmI_hypercubeSet_add(result, cube);
    esdmI_hypercube_destroy(cube);
    return result;
  }

  //determine the split factors per dimension
  double targetEdgeLength = pow(maxFragmentSize/(double)esdm_sizeof(esdm_dataspace_get_type(space)), 1.0/splitDims);
  int64_t splitFactors[space->dims];
  memset(splitFactors, 0, sizeof(splitFactors));
  for(int64_t i = 0; i < space->dims; i++) {
    splitFactors[i] = (int64_t)ceil(space->size[i]/targetEdgeLength);
    eassert(splitFactors[i] >= 1);
    eassert(splitFactors[i] <= space->size[i]);
  }

  //create the split hypercubes
  int64_t splitCoords[space->dims];
  memset(splitCoords, 0, sizeof(splitCoords));
  esdmI_hypercube_t* curCube = esdmI_hypercube_makeDefault(space->dims);
  while(true) {
    //set the current ranges
    for(int64_t i = 0; i < space->dims; i++) {
      curCube->ranges[i] = (esdmI_range_t){
        .start = space->offset[i] + splitCoords[i]*space->size[i]/splitFactors[i],
        .end = space->offset[i] + (splitCoords[i] + 1)*space->size[i]/splitFactors[i]
      };
    }

    esdmI_hypercubeSet_add(result, curCube);

    //update the split coords
    int64_t updateDim;
    for(updateDim = space->dims; updateDim--; ) {
      if(++(splitCoords[updateDim]) < splitFactors[updateDim]) break;
      splitCoords[updateDim] = 0;
    }
    if(updateDim < 0) break;
  }
  return result;
}

struct dimInfo_t {
  int64_t dimension;
  int64_t absStride;
  bool splitDim;
};

static int compare_dimInfo(const void* a, const void* b) {
  const struct dimInfo_t* dimA = a, *dimB = b;
  return dimA->absStride > dimB->absStride ? 1 :
         dimA->absStride < dimB->absStride ? -1 : 0;
}

//Implementation of `esdm_scheduler_makeSplitRecommendation()` that splits only the dimensions with the largest strides.
static esdmI_hypercubeSet_t* makeSplitRecommendation_contiguousFragments(esdm_dataspace_t* space, int64_t maxFragmentSize) {
  eassert(space);

  esdmI_hypercubeSet_t* result = esdmI_hypercubeSet_make();
  esdmI_hypercube_t* extends;
  esdmI_dataspace_getExtends(space, &extends);

  if(maxFragmentSize > 0 && esdm_dataspace_size(space) < (uint64_t)maxFragmentSize) {
    //fast path: just return the full extends if the dataspace fits into the maxFragmentSize
    esdmI_hypercubeSet_add(result, extends);
  } else {
    //inquire some info about the dataspace
    int64_t dimensions = space->dims, elementSize = esdm_sizeof(esdm_dataspace_get_type(space));
    int64_t stride[dimensions], offset[dimensions], size[dimensions];
    esdm_dataspace_getEffectiveStride(space, stride);
    esdmI_hypercube_getOffsetAndSize(extends, offset, size);

    //sort the dimensions by their locality
    struct dimInfo_t dimInfo[dimensions];
    for(int64_t i = 0; i < dimensions; i++) {
      dimInfo[i] = (struct dimInfo_t){
        .dimension = i,
        .absStride = abs_int64(stride[i]),
        .splitDim = true
      };
    };
    qsort(dimInfo, dimensions, sizeof(*dimInfo), compare_dimInfo);

    //search for the first dimension that exceeds the maxFragmentSize
    int64_t splitDim = 0, fragmentSize = elementSize;
    for(; splitDim < dimensions; splitDim++) {
      if(fragmentSize*size[dimInfo[splitDim].dimension] > maxFragmentSize) break; //This is the dimension we need to split.
    }
    eassert(splitDim < dimensions); //should be guaranteed by the fast path above
    int64_t splitSlices = (size[dimInfo[splitDim].dimension] + maxFragmentSize - 1)/maxFragmentSize;  //the amount of slices of the split dimension

    //create the hypercubes for the fragments
    int64_t fragmentCoords[dimensions];
    memset(fragmentCoords, 0, sizeof(fragmentCoords));
    while(fragmentCoords[splitDim] < splitSlices) {
      //compute the cubes' ranges from its fragment coordinates
      extends->ranges[dimInfo[splitDim].dimension] = (esdmI_range_t){
        .start = offset[dimInfo[splitDim].dimension] + fragmentCoords[splitDim]*size[dimInfo[splitDim].dimension]/splitSlices,
        .end = offset[dimInfo[splitDim].dimension] + (fragmentCoords[splitDim] + 1)*size[dimInfo[splitDim].dimension]/splitSlices
      };
      for(int64_t i = splitDim + 1; i < dimensions; i++) {
        extends->ranges[dimInfo[i].dimension] = (esdmI_range_t){
          .start = offset[dimInfo[i].dimension] + fragmentCoords[i],
          .end = offset[dimInfo[i].dimension] + fragmentCoords[i] + 1
        };
      }

      esdmI_hypercubeSet_add(result, extends);  //add the fragment to the result set

      //advance coords to the next fragment
      int64_t changeDim;
      for(changeDim = dimensions; changeDim-- > splitDim + 1; ) {
        if(++fragmentCoords[changeDim] < size[dimInfo[changeDim].dimension]) break;
        fragmentCoords[changeDim] = 0;
      }
      if(changeDim == splitDim) ++fragmentCoords[changeDim];
    }
  }

  esdmI_hypercube_destroy(extends);
  return result;
}


//Decide how the given dataset should be split into fragments to get sensible fragment sizes.
//Returns a hypercube set with one hypercube for each fragment that should be generated.
esdmI_hypercubeSet_t* esdm_scheduler_makeSplitRecommendation(esdm_dataspace_t* space, esdm_backend_t* backend) {
  switch(backend->config->fragmentation_method) {
    case ESDMI_FRAGMENTATION_METHOD_EQUALIZED:
      return makeSplitRecommendation_balancedDims(space, backend->config->max_fragment_size);
    case ESDMI_FRAGMENTATION_METHOD_CONTIGUOUS:
      return makeSplitRecommendation_contiguousFragments(space, backend->config->max_fragment_size);
  }
  fprintf(stderr, "fatal error: memory corruption detected: backend->config->fragmentation contains broken data\n");
  abort();
}

// Split the given dataspace into sub-hypercubes, one for each given backend, matching the size of the sub-hypercubes to the estimated throughput of the respective backend.
//
// `out_backendExtends` is a pointer to an uninitialized array of hypercube pointers on entry,
// this function will either create a hypercube for each entry or set it to NULL to signal that the respective backend should not be used.
static void splitToBackends(esdm_dataspace_t* space, int64_t backendCount, esdm_backend_t** backends, esdmI_hypercube_t** out_backendExtends) {
  eassert(space);
  eassert(backendCount > 0);
  eassert(backends);
  eassert(out_backendExtends);

  //get some input data
  float* weights = malloc(backendCount*sizeof(*weights));
  for(int64_t i = 0; i < backendCount; i++) weights[i] = backends[i]->callbacks.estimate_throughput(backends[i]);

  int64_t dims = esdm_dataspace_get_dims(space);
  int64_t stride[dims];
  esdm_dataspace_getEffectiveStride(space, stride);

  esdmI_hypercube_t* totalExtends;
  int ret = esdmI_dataspace_getExtends(space, &totalExtends);
  eassert(ret == ESDM_SUCCESS);

  //determine which dimension to split
  int64_t bestDim = -1;
  int64_t bestStride = 0;
  for(int64_t i = 0; i < dims; i++) {
    int64_t absStride = abs_int64(stride[i]);
    if(absStride >= bestStride && space->size[i] > 1) {
      bestStride = absStride;
      bestDim = i;
    }
  }

  if(bestDim >= 0) {
    //got a splitable dimension

    //determine the ranges for the different backends
    for(int64_t i = 1; i < backendCount; i++) weights[i] += weights[i-1]; //make weights cumulative
    float totalWeight = weights[backendCount-1];
    int64_t* bounds = malloc((backendCount + 1)*sizeof(*bounds));
    bounds[0] = totalExtends->ranges[bestDim].start;
    bounds[backendCount] = totalExtends->ranges[bestDim].end;
    int64_t size = esdmI_range_size(totalExtends->ranges[bestDim]);
    for(int64_t i = 1; i < backendCount; i++) bounds[i] = (int64_t)roundf(weights[i-1]*size/totalWeight) + bounds[0];

    //create the respective hypercubes
    for(int64_t i = 0; i < backendCount; i++) {
      eassert(bounds[i] <= bounds[i+1]);
      if(bounds[i] == bounds[i+1]) {
        out_backendExtends = NULL;
      } else {
        esdmI_hypercube_t* curCube = esdmI_hypercube_makeCopy(totalExtends);
        curCube->ranges[bestDim] = (esdmI_range_t){
          .start = bounds[i],
          .end = bounds[i+1]
        };
        out_backendExtends[i] = curCube;
      }
    }

    //cleanup
    free(bounds);
  } else {
    //no suitable split dim found, assign the entire dataspace to the fastest backend
    int64_t bestBackend = -1;
    float bestWeight = 0;
    for(int64_t i = 0; i < backendCount; i++) {
      if(bestWeight <= weights[i]) {
        bestWeight = weights[i];
        bestBackend = i;
      }
    }
    eassert(bestBackend >= 0);

    //return the result
    memset(out_backendExtends, 0, backendCount*sizeof(*out_backendExtends));
    out_backendExtends[bestBackend] = totalExtends;
    totalExtends = NULL;  //don't destroy it, we've passed it on to the caller
  }

  //cleanup
  free(weights);
  if(totalExtends) esdmI_hypercube_destroy(totalExtends);
}

esdm_status esdm_scheduler_enqueue_write(esdm_instance_t *esdm, io_request_status_t *status, esdm_dataset_t *dataset, void *buf, esdm_dataspace_t *space) {
  GError *error;
  //Gather I/O recommendations
  //esdm_performance_recommendation(esdm, NULL, NULL);    // e.g., split, merge, replication?
  //esdm_layout_recommendation(esdm, NULL, NULL);		  // e.g., merge, split, transform?
  int64_t backendCount;
  esdm_backend_t** backends = esdm_modules_makeBackendRecommendation(esdm->modules, space, &backendCount, NULL);
  eassert(backends);
  esdmI_hypercube_t** backendExtends = malloc(backendCount*sizeof(*backendExtends));
  splitToBackends(space, backendCount, backends, backendExtends);
  for(int64_t backendIndex = 0; backendIndex < backendCount; backendIndex++) {
    esdmI_hypercube_t* curExtends = backendExtends[backendIndex];
    if(curExtends) {
      esdm_backend_t* curBackend = backends[backendIndex];

      //get a list of hypercubes to write to this backend
      esdm_dataspace_t* backendSpace;
      esdm_status ret = esdmI_dataspace_createFromHypercube(curExtends, esdm_dataspace_get_type(space), &backendSpace);
      eassert(ret == ESDM_SUCCESS);
      ret = esdm_dataspace_copyDatalayout(backendSpace, space);
      eassert(ret == ESDM_SUCCESS);
      esdmI_hypercubeSet_t* cubes = esdm_scheduler_makeSplitRecommendation(backendSpace, curBackend);
      esdmI_hypercubeList_t* cubeList = esdmI_hypercubeSet_list(cubes);
      esdm_dataspace_destroy(backendSpace);

      //create a fragment and task for each of the hypercubes in the list
      int64_t dim[space->dims], offset[space->dims];
      for(int64_t i = 0; i < cubeList->count; i++) {
        status->pending_ops++;

        esdmI_hypercube_getOffsetAndSize(cubeList->cubes[i], offset, dim);

        esdm_dataspace_t* subspace;
        esdm_status ret = esdmI_dataspace_createFromHypercube(cubeList->cubes[i], esdm_dataspace_get_type(space), &subspace);
        eassert(ret == ESDM_SUCCESS);
        ret = esdm_dataspace_copyDatalayout(subspace, space);
        eassert(ret == ESDM_SUCCESS);
        esdm_fragment_t* fragment;
        ret = esdmI_fragment_create(dataset, subspace, (char*)buf + esdm_dataspace_elementOffset(space, offset), &fragment);
        eassert(ret == ESDM_SUCCESS);
        fragment->backend = curBackend;

        io_work_t* task = malloc(sizeof(*task));
        *task = (io_work_t){
          .fragment = fragment,
          .op = ESDM_OP_WRITE,
          .return_code = ESDM_SUCCESS,
          .parent = status,
          .callback = NULL,
          .data = {NULL, NULL}
        };

        if (curBackend->threads == 0) {
          backend_thread(task, curBackend);
        } else {
          g_thread_pool_push(curBackend->threadPool, task, &error);
        }

        //update the statistics
        esdm->writeStats.fragments++;
        esdm->writeStats.bytesIo += esdm_dataspace_size(subspace);
      }

      esdmI_hypercubeSet_destroy(cubes);
      esdmI_hypercube_destroy(curExtends);
    }
  }

  //update the statistics
  esdm->writeStats.requests++;
  esdm->writeStats.bytesUser += esdm_dataspace_size(space);

  //cleanup
  free(backendExtends);
  free(backends);
  return ESDM_SUCCESS;
}

esdm_status esdm_scheduler_status_init(io_request_status_t *status) {
  g_mutex_init(&status->mutex);
  g_cond_init(&status->done_condition);
  status->pending_ops = 0;
  status->return_code = ESDM_SUCCESS;
  return ESDM_SUCCESS;
}

esdm_status esdm_scheduler_status_finalize(io_request_status_t *status) {
  g_mutex_clear(&status->mutex);
  g_cond_clear(&status->done_condition);
  return ESDM_SUCCESS;
}

esdm_status esdm_scheduler_wait(io_request_status_t *status) {
  g_mutex_lock(&status->mutex);
  if (status->pending_ops) {
    g_cond_wait(&status->done_condition, &status->mutex);
  }
  g_mutex_unlock(&status->mutex);
  return ESDM_SUCCESS;
}

static bool fragmentsCoverSpace(esdm_dataspace_t* space, int64_t fragmentCount, esdm_fragment_t** fragments, esdmI_hypercubeSet_t** out_uncoveredRegion) {
  eassert(space);
  eassert(fragments);
  eassert(out_uncoveredRegion);

  esdmI_hypercube_t* curCube;
  *out_uncoveredRegion = esdmI_hypercubeSet_make();

  esdmI_dataspace_getExtends(space, &curCube);
  esdmI_hypercubeSet_add(*out_uncoveredRegion, curCube);
  esdmI_hypercube_destroy(curCube);

  for(int64_t i = 0; i < fragmentCount; i++) {
    esdmI_dataspace_getExtends(fragments[i]->dataspace, &curCube);
    esdmI_hypercubeSet_subtract(*out_uncoveredRegion, curCube);
    esdmI_hypercube_destroy(curCube);
  }

  return esdmI_hypercubeSet_isEmpty(*out_uncoveredRegion);
}

//Find and remove all fragments that either do not intersect with the bounds at all, or are fully covered by other fragments.
//This implementation tries to minimize the total amount of data that needs to be read, but may yield a nonoptimal solution because it has a probabilistic component.
static void removeRedundantFragments(esdmI_hypercube_t* bounds, int* inout_fragmentCount, esdm_fragment_t** fragments) {
  eassert(bounds);
  eassert(inout_fragmentCount);
  eassert(fragments);

  //compute the bounded extends for all the fragments, filter out the ones that do not intersect with the bounds at all, and create a hypercube list of the bounded extends
  esdmI_hypercubeList_t list;
  list.cubes = malloc(*inout_fragmentCount*sizeof(*list.cubes));
  int64_t fragmentCount = 0;
  for(int64_t i = 0; i < *inout_fragmentCount; i++) {
    esdmI_hypercube_t* extends, *boundedExtends;
    esdmI_dataspace_getExtends(fragments[i]->dataspace, &extends);
    if((boundedExtends = esdmI_hypercube_makeIntersection(bounds, extends))) {
      list.cubes[fragmentCount] = boundedExtends;
      fragments[fragmentCount++] = fragments[i];
    }
    esdmI_hypercube_destroy(extends);
  }
  list.count = fragmentCount;

  if(fragmentCount) { //the subset selection code would not be able to select a fragment subset if there is no fragment at all, so we avoid executing it in this case
    //get a few nonredundant subsets of fragments
    int64_t subsetCount = 10;
    uint8_t (*subsets)[fragmentCount] = malloc(subsetCount*sizeof(*subsets));
    esdmI_hypercubeList_nonredundantSubsets(&list, &subsetCount, subsets);

    //select the subset with the smallest total fragment size
    int64_t bestSize = INT64_MAX, bestIndex = -1;
    for(int64_t i = 0; i < subsetCount; i++) {
      int64_t curSize = 0;
      for(int64_t j = 0; j < fragmentCount; j++) {
        if(subsets[i][j]) curSize += esdm_dataspace_element_count(fragments[j]->dataspace);
      }
      if(curSize <= bestSize) {
        bestSize = curSize;
        bestIndex = i;
      }
    }
    eassert(bestIndex >= 0);

    //filter the list of fragments by the selected subset
    fragmentCount = 0;
    for(int64_t i = 0; i < list.count; i++) {
      if(subsets[bestIndex][i]) fragments[fragmentCount++] = fragments[i];
    }

    //cleanup
    free(subsets);
  }

  //cleanup and return data
  for(int64_t i = 0; i < list.count; i++) esdmI_hypercube_destroy(list.cubes[i]);
  free(list.cubes);
  *inout_fragmentCount = fragmentCount;
}

esdm_status esdm_scheduler_write_blocking(esdm_instance_t *esdm, esdm_dataset_t *dataset, void *buf, esdm_dataspace_t *subspace) {
  ESDM_DEBUG(__func__);

  io_request_status_t status;

  esdm_status ret = esdm_scheduler_status_init(&status);
  eassert(ret == ESDM_SUCCESS);

  ret = esdm_scheduler_enqueue_write(esdm, &status, dataset, buf, subspace);
  if( ret != ESDM_SUCCESS){
    return ret;
  }
  ret = esdm_scheduler_wait(&status);
  eassert(ret == ESDM_SUCCESS);

  ret = esdm_scheduler_status_finalize(&status);
  eassert(ret == ESDM_SUCCESS);
  return status.return_code;
}

esdm_status esdm_scheduler_read_blocking(esdm_instance_t *esdm, esdm_dataset_t *dataset, void *buf, esdm_dataspace_t *subspace, esdmI_hypercubeSet_t** out_fillRegion) {
  ESDM_DEBUG(__func__);

  io_request_status_t status;

  esdm_status ret = esdm_scheduler_status_init(&status);
  eassert(ret == ESDM_SUCCESS);

  esdm_fragment_t **read_frag = NULL;
  int frag_count;
  ret = esdmI_dataset_lookup_fragments(dataset, subspace, & frag_count, &read_frag);
  eassert(ret == ESDM_SUCCESS);
  esdmI_hypercube_t* readExtends;
  esdmI_dataspace_getExtends(subspace, &readExtends);
  removeRedundantFragments(readExtends, &frag_count, read_frag);
  DEBUG("fragments to read: %d", frag_count);

  //check whether we have all the requested data
  esdmI_hypercubeSet_t* uncovered;
  if(!fragmentsCoverSpace(subspace, frag_count, read_frag, &uncovered)) {
    esdm_type_t type = esdm_dataspace_get_type(subspace);
    eassert(type == esdm_dataset_get_type(dataset));  //TODO handle the case that the two types don't match
    char fillValue[esdm_sizeof(type)];
    ret = esdm_dataset_get_fill_value(dataset, fillValue);
    if(ret == ESDM_SUCCESS) {
      //we have a fill value, so we continue to read, fill the uncovered parts with the fill value, and signal back to the user how much uncovered data we filled
      ret = esdm_scheduler_enqueue_fill(esdm, &status, fillValue, buf, subspace, esdmI_hypercubeSet_list(uncovered));
    } else {
      ret = ESDM_INCOMPLETE_DATA; //no fill value set, so we error out
    }
  }

  if(ret == ESDM_SUCCESS) {
    //all preliminaries successful, commit to reading
    ret = esdm_scheduler_enqueue_read(esdm, &status, frag_count, read_frag, buf, subspace);
    eassert(ret == ESDM_SUCCESS);

    ret = esdm_scheduler_wait(&status);
    eassert(ret == ESDM_SUCCESS);

    ret = esdm_scheduler_status_finalize(&status);
    eassert(ret == ESDM_SUCCESS);

    ret = status.return_code;

    //update the statistics
    esdm->readStats.requests++;
    esdm->readStats.fragments += frag_count;
    esdm->readStats.bytesUser += esdm_dataspace_size(subspace);
    for(int64_t i = 0; i < frag_count; i++) {
      esdm->readStats.bytesIo += esdm_dataspace_size(read_frag[i]->dataspace);
    }
  }

  //cleanup, must not happen before we wait for the background processes to finish their tasks
  if(out_fillRegion) {  //either return the fill region to the user or destroy it
    *out_fillRegion = uncovered;
  } else {
    esdmI_hypercubeSet_destroy(uncovered);
  }
  free(read_frag);

  return ret;
}
