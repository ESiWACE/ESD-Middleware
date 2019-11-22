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
 * Original author: Huang Hua <hua.huang@seagate.com>
 * Original creation date: 13-Oct-2017
 */

/**
 * @file
 * @brief A data backend to provide Clovis compatibility.
 */

#define VERBOSE_DEBUG

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <esdm-debug.h>
#include <esdm.h>
#include <esdm-datatypes-internal.h>

#include "clovis/clovis.h"
#include "clovis/clovis_internal.h"

#include "clovis_internal.h"

#ifdef VERBOSE_DEBUG
#define DEBUG(fmt)          printf(fmt)
#define DEBUG_FMT(fmt, ...) printf(fmt, __VA_ARGS__)
#define DEBUG_ENTER         printf(">>>Entering %s:%d\n", __func__, __LINE__)
#define DEBUG_LEAVE         printf("<<<Leaving  %s:%d\n", __func__, __LINE__)
#else
#define DEBUG(fmt)          ESDM_DEBUG_COM_FMT("CLOVIS", fmt)
#define DEBUG_FMT(fmt, ...) ESDM_DEBUG_COM_FMT("CLOVIS", fmt, __VA_ARGS__)
#define DEBUG_ENTER         ESDM_DEBUG_COM_FMT("CLOVIS", ">>>Entering %s:%d\n", __func__, __LINE__)
#define DEBUG_LEAVE         ESDM_DEBUG_COM_FMT("CLOVIS", "<<<Leaving  %s:%d\n", __func__, __LINE__)
#endif

#define PAGE_4K (4096ULL)
#define BLOCKSIZE (PAGE_4K)
#define BLOCKMASK (BLOCKSIZE - 1)
#define CLOVIS_OBJ_ID "obj_id"

static struct m0_uint128 gid;
static struct m0_fid gidxfid = {
	.f_container = 0x1ULL,
	.f_key = 0x1234567812345678ULL
};

int clovis_index_create(struct m0_clovis_realm *parent, struct m0_fid *fid);

static inline esdm_backend_t_clovis_t *
eb2ebm(esdm_backend_t *eb)
{
	return container_of(eb, esdm_backend_t_clovis_t, ebm_base);
}

/*
 * Lock the whole backend to prevent concurrent access.
 * @TODO To investigate if this is really necessarily needed.
 */
static void ebm_lock(esdm_backend_t *backend)
{
	m0_mutex_lock(&eb2ebm(backend)->ebm_mutex);
}

static void ebm_unlock(esdm_backend_t *backend)
{
	m0_mutex_unlock(&eb2ebm(backend)->ebm_mutex);
}

/*
 * Convert a normal pthread into a Mero thread.
 * This is needed for Clovis operations if the thread is not created
 * by m0_thread_init();
 */
static bool convert_pthread_to_mero_thread(esdm_backend_t *backend)
{
	esdm_backend_t_clovis_t *ebm;
	struct m0_thread_tls *tls;
	struct m0_thread     *mthread;
	DEBUG_ENTER;

	ebm = eb2ebm(backend);
	tls = m0_thread_tls();
	eassert(tls == NULL);

	mthread = malloc(sizeof(struct m0_thread));
	eassert(mthread != NULL);

	memset(mthread, 0, sizeof(struct m0_thread));
	m0_thread_adopt(mthread, ebm->ebm_clovis_instance->m0c_mero);

	DEBUG_LEAVE;
	return true;
}

static void revert_mero_thread_to_pthread()
{
	DEBUG_ENTER;
	m0_thread_shun();
	DEBUG_LEAVE;
}

/**
 * @TODO: Please note, if multiple apps are running from the same node,
 * different local addresses are required.
 *  */
char *
laddr_get()
{
	FILE *output;
	char screen[4096];
	char *first_newline;
	int rc;

	output =
		popen
		("/mero.bin/bin/m0nettest -l | grep NID | egrep -e 'tcp|o2ib' | head -n 1 | awk -e '{print $3}'",
		 "r");
	if (output == NULL) {
		return NULL;
	}

	memset(screen, 0, 4096);
	rc = fread(screen, 1, 4096, output);
	pclose(output);
	if (rc > 0) {
		/* find the first '\n' char and replace it with '\0' */
		first_newline = strchr(screen, '\n');
		if (first_newline != 0)
			*first_newline = '\0';
		DEBUG_FMT("local addr = %s\n", screen);
	}
	return rc > 0 ? strdup(screen) : NULL;
}

char *clovis_index_dir = "/tmp/";
struct m0_idx_dix_config dix_conf = {.kc_create_meta = false };
/**
 * @TODO Use the laddr_get() to generate a unique local address.
 * This is needed for running in MPI with the same configuration file,
 * and actually every client needs its own local address.
 */
static int
conf_parse(char *conf, esdm_backend_t_clovis_t *ebm)
{
	/*
	 * Parse the conf string into ebm->ebm_clovis_conf.
	 */
	char *clovis_local_addr;
	char *clovis_ha_addr;
	char *clovis_prof;
	char *clovis_proc_fid;
	/*char *laddr, *combined_laddr; */

	if ((clovis_local_addr = strsep(&conf, " ")) == NULL)
		return -EINVAL;
#if 0
	/* Temporarily disabled this. */
	laddr = laddr_get();
	if (laddr == NULL)
		return -EINVAL;
	/* concatenate the local address and user provided appendix */
	asprintf(&combined_laddr, "%s%s", laddr, clovis_local_addr);
	free(laddr);
	if (combined_laddr == NULL)
		return -EINVAL;
	ebm->ebm_clovis_conf.cc_local_addr = combined_laddr;
#else
	ebm->ebm_clovis_conf.cc_local_addr = strdup(clovis_local_addr);
#endif

	if ((clovis_ha_addr = strsep(&conf, " ")) == NULL)
		return -EINVAL;
	ebm->ebm_clovis_conf.cc_ha_addr = strdup(clovis_ha_addr);

	if ((clovis_prof = strsep(&conf, " ")) == NULL)
		return -EINVAL;
	ebm->ebm_clovis_conf.cc_profile = strdup(clovis_prof);

	if ((clovis_proc_fid = strsep(&conf, " ")) == NULL)
		return -EINVAL;
	ebm->ebm_clovis_conf.cc_process_fid = strdup(clovis_proc_fid);

	ebm->ebm_clovis_conf.cc_is_oostore = true;
	ebm->ebm_clovis_conf.cc_is_read_verify = false;
	ebm->ebm_clovis_conf.cc_tm_recv_queue_min_len = M0_NET_TM_RECV_QUEUE_DEF_LEN;
	ebm->ebm_clovis_conf.cc_max_rpc_msg_size = M0_RPC_DEF_MAX_RPC_MSG_SIZE;
	ebm->ebm_clovis_conf.cc_layout_id = 0;
	/* for DIX index type. */
	ebm->ebm_clovis_conf.cc_idx_service_id = M0_CLOVIS_IDX_DIX;
	ebm->ebm_clovis_conf.cc_idx_service_conf = &dix_conf;
	/* for MOCK index type. Use DIX or MOCK. */
	ebm->ebm_clovis_conf.cc_idx_service_id = M0_CLOVIS_IDX_MOCK;
	ebm->ebm_clovis_conf.cc_idx_service_conf = clovis_index_dir;

	DEBUG_FMT("local addr = %s\n", ebm->ebm_clovis_conf.cc_local_addr);
	DEBUG_FMT("ha addr    = %s\n", ebm->ebm_clovis_conf.cc_ha_addr);
	DEBUG_FMT("profile    = %s\n", ebm->ebm_clovis_conf.cc_profile);
	DEBUG_FMT("process id = %s\n", ebm->ebm_clovis_conf.cc_process_fid);
	return 0;
}

static int
esdm_backend_t_clovis_init(char *conf, esdm_backend_t *eb)
{
	esdm_backend_t_clovis_t *ebm;
	time_t t;
	unsigned int pid;
	unsigned int r;
	unsigned long long f;
	int rc;
	DEBUG_ENTER;

	ebm = eb2ebm(eb);

	rc = conf_parse(conf, ebm);
	if (rc != 0) {
		DEBUG_LEAVE;
		return rc;
	}

	/* Clovis instance */
	rc = m0_clovis_init(&ebm->ebm_clovis_instance, &ebm->ebm_clovis_conf, true);
	if (rc != 0) {
		DEBUG_LEAVE;
		DEBUG_FMT("Failed to initilise Clovis: %d\n", rc);
		return rc;
	}

	/* And finally, clovis root realm */
	m0_clovis_container_init(&ebm->ebm_clovis_container,
				 NULL,
				 &M0_CLOVIS_UBER_REALM,
				 ebm->ebm_clovis_instance);
	rc = ebm->ebm_clovis_container.co_realm.re_entity.en_sm.sm_rc;

	if (rc != 0) {
		DEBUG_LEAVE;
		DEBUG("Failed to open uber realm.\n");
		return rc;
	}

	/* FIXME this makes the gid not reused. */
	t = time(NULL);
	srand(t);
	r = rand();
	pid = getpid();
	f = (t << 16) | (r & 0xff00) | (pid & 0xff);
	gid.u_hi = f;
	gid.u_lo = 1L;
	DEBUG_FMT("GID set to: <%lx:%lx>\n", gid.u_hi, gid.u_lo);

	/* create the global mapping index */
	/* XXX NO NEED TO DO SO.
		 rc = clovis_index_create(&ebm->ebm_clovis_container.co_realm,
		 &gidxfid);
	 */

	DEBUG_LEAVE;
	return rc;
}

static int
esdm_backend_t_clovis_fini(esdm_backend_t *eb)
{
	esdm_backend_t_clovis_t *ebm;
	DEBUG_ENTER;

	ebm = eb2ebm(eb);
	m0_clovis_fini(ebm->ebm_clovis_instance, true);
	free((char *)ebm->ebm_clovis_conf.cc_local_addr);
	free((char *)ebm->ebm_clovis_conf.cc_ha_addr);
	free((char *)ebm->ebm_clovis_conf.cc_profile);
	free((char *)ebm->ebm_clovis_conf.cc_process_fid);
	free(ebm);

	DEBUG_LEAVE;
	return 0;
}

static void
open_entity(struct m0_clovis_obj *obj)
{
	struct m0_clovis_entity *entity = &obj->ob_entity;
	struct m0_clovis_op *ops[1] = { NULL };
	DEBUG_ENTER;

	m0_clovis_entity_open(entity, &ops[0]);
	m0_clovis_op_launch(ops, 1);
	m0_clovis_op_wait(ops[0], M0_BITS(M0_CLOVIS_OS_FAILED,
			  M0_CLOVIS_OS_STABLE), M0_TIME_NEVER);	//m0_time_from_now(3,0));
	m0_clovis_op_fini(ops[0]);
	m0_clovis_op_free(ops[0]);
	ops[0] = NULL;
	DEBUG_LEAVE;
}

static int
create_object(esdm_backend_t_clovis_t *ebm, struct m0_uint128 id)
{
	int rc = 0;
	struct m0_clovis_obj obj;
	struct m0_clovis_op *ops[1] = { NULL };
	DEBUG_ENTER;

	memset(&obj, 0, sizeof(struct m0_clovis_obj));

	m0_clovis_obj_init(&obj, &ebm->ebm_clovis_container.co_realm,
			   &id,
			   m0_clovis_layout_id(ebm->ebm_clovis_instance));

	open_entity(&obj);

	m0_clovis_entity_create(NULL, &obj.ob_entity, &ops[0]);

	m0_clovis_op_launch(ops, ARRAY_SIZE(ops));

	rc = m0_clovis_op_wait(ops[0], M0_BITS(M0_CLOVIS_OS_FAILED,
			       M0_CLOVIS_OS_STABLE), M0_TIME_NEVER);	//m0_time_from_now(3,0));

	m0_clovis_op_fini(ops[0]);
	m0_clovis_op_free(ops[0]);
	m0_clovis_obj_fini(&obj);

	DEBUG_LEAVE;
	return rc;
}

static struct m0_uint128
object_id_alloc()
{
	/* gid.u_hi keeps unchanged in a one session. */
	gid.u_lo++;
	return gid;
}

static char *
object_id_encode(const struct m0_uint128 *obj_id)
{
	/* "oid=<0x1234567812345678:0x1234567812345678>" */
	char *json = NULL;
	struct m0_fid fid;

	fid.f_container = obj_id->u_hi;
	fid.f_key = obj_id->u_lo;

	asprintf(&json, "oid=" FID_F, FID_P(&fid));
	return json;
}

static int
object_id_decode(const char *oid_json, struct m0_uint128 *obj_id)
{
	struct m0_fid fid;
	int rc;
	const char *oid = strchr(oid_json, '=');

	if (oid == NULL)
		return -EINVAL;

	oid++;
	rc = m0_fid_sscanf(oid, &fid);
	if (rc != 0)
		return rc;

	obj_id->u_hi = fid.f_container;
	obj_id->u_lo = fid.f_key;

	return 0;
}

static char *
object_meta_encode(const struct m0_uint128 *obj_id)
{
	return NULL;
}

static int
esdm_backend_t_clovis_alloc(esdm_backend_t *eb,
			    int n_dims,
			    int *dims_size,
			    esdm_type_t type,
			    char *md1,
			    char *md2,
			    char **out_object_id,
			    char **out_mero_metadata)
{
	esdm_backend_t_clovis_t *ebm;
	struct m0_uint128 obj_id;
	int rc;
	DEBUG_ENTER;

	ebm = eb2ebm(eb);

	/* First step: alloc a new fid for this new object. */
	obj_id = object_id_alloc();
	DEBUG_FMT("new obj id = <%lx:%lx>\n", FID_P((struct m0_fid *)&obj_id));

	/* Then create object */
	rc = create_object(ebm, obj_id);
	if (rc == 0) {
		/* encode this obj_id into string */
		*out_object_id     = object_id_encode(&obj_id);
		*out_mero_metadata = object_meta_encode(&obj_id);
	}
	DEBUG_LEAVE;
	return rc;
}

static int
esdm_backend_t_clovis_open(esdm_backend_t *eb,
			   const char *object_id,
			   void **obj_handle)
{
	esdm_backend_t_clovis_t *ebm;
	struct m0_clovis_obj *obj;
	struct m0_uint128 obj_id;
	int rc = 0;
	DEBUG_ENTER;

	ebm = eb2ebm(eb);

	/* convert from json string to object id. */
	object_id_decode(object_id, &obj_id);

	obj = malloc(sizeof (struct m0_clovis_obj));
	if (obj == NULL) {
		DEBUG_LEAVE;
		return -ENOMEM;
	}

	memset(obj, 0, sizeof(struct m0_clovis_obj));
	m0_clovis_obj_init(obj,
			   &ebm->ebm_clovis_container.co_realm,
			   &obj_id,
			   m0_clovis_layout_id(ebm->ebm_clovis_instance));

	open_entity(obj);
	*obj_handle = obj;

	DEBUG_LEAVE;
	return rc;
}

static int
esdm_backend_t_clovis_rdwr(esdm_backend_t *eb,
			   void *obj_handle,
			   uint64_t start,
			   uint64_t count,
			   void *data,
			   int rdwr_op)
{
	struct m0_clovis_obj *obj = (struct m0_clovis_obj *)obj_handle;
	uint64_t i;
	struct m0_clovis_op *ops[1] = { NULL };
	struct m0_indexvec ext;
	struct m0_bufvec data_buf;
	struct m0_bufvec attr_buf;
	uint64_t clovis_block_count;
	uint64_t clovis_block_size;
	int rc;

	eassert((start & BLOCKMASK) == 0);
	eassert(((start + count) & BLOCKMASK) == 0);
	eassert(rdwr_op == M0_CLOVIS_OC_READ || rdwr_op == M0_CLOVIS_OC_WRITE);

	/*
	 * read the extended region and copy the data from new buffers.
	 */

	clovis_block_size = BLOCKSIZE;
	clovis_block_count = count / clovis_block_size;

	/* we want to read <clovis_block_count> from @start of the object */
	rc = m0_indexvec_alloc(&ext, clovis_block_count);
	if (rc != 0)
		return rc;

	/*
	 * this allocates <clovis_block_count> empty buffers for data.
	 */
	rc = m0_bufvec_empty_alloc(&data_buf, clovis_block_count);
	if (rc != 0) {
		m0_indexvec_free(&ext);
		return rc;
	}

	rc = m0_bufvec_alloc(&attr_buf, clovis_block_count, 1);
	if (rc != 0) {
		m0_indexvec_free(&ext);
		m0_bufvec_free2(&data_buf);
		return rc;
	}

	for (i = 0; i < clovis_block_count; i++) {
		ext.iv_index[i] = start + clovis_block_size * i;
		ext.iv_vec.v_count[i] = clovis_block_size;

		data_buf.ov_buf[i] = data + clovis_block_size * i;
		data_buf.ov_vec.v_count[i] = clovis_block_size;

		/* we don't want any attributes */
		attr_buf.ov_vec.v_count[i] = 0;
	}

	/* Create the read request */
	m0_clovis_obj_op(obj, rdwr_op, &ext, &data_buf, &attr_buf, 0, &ops[0]);
	M0_ASSERT(rc == 0);
	M0_ASSERT(ops[0] != NULL);
	M0_ASSERT(ops[0]->op_sm.sm_rc == 0);

	m0_clovis_op_launch(ops, 1);

	/* wait */
	rc = m0_clovis_op_wait(ops[0],
			 M0_BITS(M0_CLOVIS_OS_FAILED,
				 M0_CLOVIS_OS_STABLE), M0_TIME_NEVER);
	M0_ASSERT(rc == 0);
	M0_ASSERT(ops[0]->op_sm.sm_state == M0_CLOVIS_OS_STABLE);
	M0_ASSERT(ops[0]->op_sm.sm_rc == 0);

	/* fini and release */
	m0_clovis_op_fini(ops[0]);
	m0_clovis_op_free(ops[0]);

	m0_indexvec_free(&ext);
	m0_bufvec_free2(&data_buf);
	m0_bufvec_free(&attr_buf);

	return rc;
}

static int
esdm_backend_t_clovis_write(esdm_backend_t *eb,
			    void *obj_handle,
			    uint64_t start,
			    uint64_t count,
			    void *data)
{
	int rc;
	eassert((start & BLOCKMASK) == 0);
	eassert(((start + count) & BLOCKMASK) == 0);
	rc = esdm_backend_t_clovis_rdwr(eb, obj_handle, start, count,
					data, M0_CLOVIS_OC_WRITE);
	return rc;
}

static int
esdm_backend_t_clovis_read(esdm_backend_t *eb,
			   void *obj_handle,
			   uint64_t start,
			   uint64_t count,
			   void *data)
{
	int rc;
	eassert((start & BLOCKMASK) == 0);
	eassert(((start + count) & BLOCKMASK) == 0);
	rc = esdm_backend_t_clovis_rdwr(eb, obj_handle, start, count,
					data, M0_CLOVIS_OC_READ);
	return rc;
}

static int
esdm_backend_t_clovis_close(esdm_backend_t *eb, void *obj_handle)
{
	struct m0_clovis_obj *obj;
	int rc = 0;
	DEBUG_ENTER;

	obj = (struct m0_clovis_obj *)obj_handle;
	m0_clovis_obj_fini(obj);
	free(obj);

	DEBUG_LEAVE;
	return rc;
}

static int
esdm_backend_t_clovis_performance_estimate(esdm_backend_t  *b,
					   esdm_fragment_t *fragment,
					   float           *out_time)
{
	DEBUG_ENTER;
	DEBUG_LEAVE;
	return 0;
}

static int
index_op_tail(struct m0_clovis_entity *ce, struct m0_clovis_op *op, int rc)
{
	if (rc == 0) {
		m0_clovis_op_launch(&op, 1);
		rc = m0_clovis_op_wait(op,
				       M0_BITS(M0_CLOVIS_OS_FAILED,
				       M0_CLOVIS_OS_STABLE),
				       M0_TIME_NEVER);
		DEBUG_FMT("operation (%d) rc: %i\n", op->op_code, op->op_rc);
	}
	else
		DEBUG_FMT("operation (%d) fail rc: %i\n", op->op_code, rc);
	m0_clovis_op_fini(op);
	m0_clovis_op_free(op);
	m0_clovis_entity_fini(ce);
	return rc;
}

int
clovis_index_create(struct m0_clovis_realm *parent, struct m0_fid *fid)
{
	struct m0_clovis_op *op = NULL;
	struct m0_clovis_idx idx;
	int rc = 0;

	m0_clovis_idx_init(&idx, parent, (struct m0_uint128 *)fid);
	rc = m0_clovis_entity_create(NULL, &idx.in_entity, &op);
	rc = index_op_tail(&idx.in_entity, op, rc);
	return rc;
}

static int
index_op(struct m0_clovis_realm *parent,
	 struct m0_fid *fid,
	 enum m0_clovis_idx_opcode opcode,
	 struct m0_bufvec *keys, struct m0_bufvec *vals)
{
	struct m0_clovis_idx idx;
	struct m0_clovis_op *op = NULL;
	int32_t *rcs;
	int rc;

	M0_ASSERT(keys != NULL);
	M0_ASSERT(keys->ov_vec.v_nr != 0);
	M0_ALLOC_ARR(rcs, keys->ov_vec.v_nr);
	if (rcs == NULL)
		return -ENOMEM;
	m0_clovis_idx_init(&idx, parent, (struct m0_uint128 *)fid);
	rc = m0_clovis_idx_op(&idx, opcode, keys, vals, rcs, 0, &op);
	rc = index_op_tail(&idx.in_entity, op, rc);
	m0_free(rcs);
	return rc;
}

static int
clovis_index_put(struct m0_clovis_realm *parent,
		 struct m0_fid *fid,
		 struct m0_bufvec *keys, struct m0_bufvec *vals)
{
	int rc = 0;

	M0_PRE(fid != NULL);
	M0_PRE(keys != NULL);
	M0_PRE(vals != NULL);

	rc = index_op(parent, fid, M0_CLOVIS_IC_PUT, keys, vals);
	DEBUG_FMT("put done: %i\n", rc);

	return rc;
}

static int
clovis_index_get(struct m0_clovis_realm *parent,
		 struct m0_fid *fid,
		 struct m0_bufvec *keys,
		 struct m0_bufvec *vals)
{
	int rc;

	M0_PRE(fid != NULL);
	M0_PRE(keys != NULL && keys->ov_vec.v_nr == 1);
	M0_PRE(vals != NULL && vals->ov_vec.v_nr == 1);

	rc = index_op(parent, fid, M0_CLOVIS_IC_GET, keys, vals);
	if (rc == 0) {
		if (vals->ov_buf[0] == NULL)
			rc = -ENODATA;
	}
	DEBUG_FMT("get done: %i\n", rc);
	return rc;
}

static int
bufvec_fill(const char *value, struct m0_bufvec *vals)
{
	int rc;

	rc = m0_bufvec_empty_alloc(vals, 1);
	if (rc < 0)
		return rc;

	if (value != NULL) {
		vals->ov_buf[0] = strdup(value);
		vals->ov_vec.v_count[0] = strlen(value);
	}

	return rc;
}

static int
mapping_get(esdm_backend_t *backend, const char *name, char **obj_id)
{
	esdm_backend_t_clovis_t *ebm = eb2ebm(backend);
	int rc;
	struct m0_bufvec key;
	struct m0_bufvec val;

	rc = bufvec_fill(name, &key) ? : bufvec_fill(NULL, &val);

	if (rc == 0) {
		rc = clovis_index_get(&ebm->ebm_clovis_container.co_realm,
				      &gidxfid, &key, &val);
		if (rc == 0) {
			/* malloc & copy & setting trailing zero. */
			int datalen = val.ov_vec.v_count[0];
			char *res = malloc(datalen + 1);
			if (res != NULL) {
	memcpy(res, val.ov_buf[0], datalen);
	res[datalen] = 0;
	*obj_id = res;
			}
			else
	rc = -ENOMEM;
		}
	}

	m0_bufvec_free(&key);
	m0_bufvec_free(&val);

	return rc;
}

static int
mapping_insert(esdm_backend_t *backend, const char *name, const char *obj_id)
{
	esdm_backend_t_clovis_t *ebm = eb2ebm(backend);
	int rc;
	struct m0_bufvec key;
	struct m0_bufvec val;

	rc = bufvec_fill(name, &key) ? : bufvec_fill(obj_id, &val);

	if (rc == 0) {
		rc = clovis_index_put(&ebm->ebm_clovis_container.co_realm,
				      &gidxfid, &key, &val);
	}

	m0_bufvec_free(&key);
	m0_bufvec_free(&val);

	return rc;
}

static int
esdm_backend_t_clovis_fragment_retrieve(esdm_backend_t  *backend,
					esdm_fragment_t *fragment)
{
	int   rc = 0;
	bool  mthreaded = false;

	DEBUG_ENTER;

	if (backend == NULL || fragment == NULL || fragment->buf == NULL) {
		DEBUG_LEAVE;
		return ESDM_ERROR;
	}

	/* Check if id is valid */
	if (fragment->id == NULL) {
		DEBUG("INVALID metadata. No object ID is found.\n");
		return ESDM_ERROR;
	}

	if ((fragment->bytes & BLOCKMASK) != 0) {
		DEBUG_FMT("size=%lu is not BLOCK aligned\n", fragment->bytes);
		return ESDM_ERROR;
	}

	mthreaded = convert_pthread_to_mero_thread(backend);
	ebm_lock(backend);

	DEBUG_FMT("Retrieving from f=%p id=%s size=%lu\n", fragment, fragment->id, fragment->bytes);

	void *obj_handle = NULL;
	/* 2. open object with its object id. */
	rc = esdm_backend_t_clovis_open(backend, fragment->id, &obj_handle);
	DEBUG_FMT("Object Opened: obj=%p rc=%d\n", obj_handle, rc);
	if (rc == 0) {
		void *readBuffer;

		if (fragment->dataspace->stride) {
			/*
			 * The data is not contiguous in memory.
			 * Let's allocate contiguous memory to retrieve data.
			 */
			readBuffer = malloc(fragment->bytes);
			eassert(readBuffer);
		} else {
			readBuffer = fragment->buf;
		}
		/* 3. read from this object. */
		rc = esdm_backend_t_clovis_read(backend,
						obj_handle,
						0,
						fragment->bytes,
						readBuffer);

		if ((rc == 0) && fragment->dataspace->stride) {
			/* Data is ready. Copy to the requested buffer*/
			esdm_dataspace_t* contiguousSpace;
			esdm_dataspace_makeContiguous(fragment->dataspace,
						      &contiguousSpace);
			esdm_dataspace_copy_data(contiguousSpace,
						 readBuffer,
						 fragment->dataspace,
						 fragment->buf);
			esdm_dataspace_destroy(contiguousSpace);
		}
		if (fragment->dataspace->stride) {
			/* This memory is allocated previously */
			free(readBuffer);
		}
		/* 4. close this object. */
		esdm_backend_t_clovis_close(backend, obj_handle);
	}

	ebm_unlock(backend);
	if (mthreaded)
		revert_mero_thread_to_pthread();
	DEBUG_LEAVE;
	return rc == 0 ? ESDM_SUCCESS : ESDM_ERROR;
}

static int
esdm_backend_t_clovis_fragment_update(esdm_backend_t  *backend,
				      esdm_fragment_t *fragment)
{
	char *obj_id     = NULL;
	char *obj_meta   = NULL;
	int   rc         = ESDM_SUCCESS;
	void *writeBuffer = NULL;
	bool  mthreaded = false;
	DEBUG_ENTER;


	if ((fragment->bytes & BLOCKMASK) != 0) {
		DEBUG_FMT("size=%lu is not BLOCK aligned\n", fragment->bytes);
		DEBUG_LEAVE;
		return ESDM_ERROR;
	}

	mthreaded = convert_pthread_to_mero_thread(backend);
	ebm_lock(backend);

	DEBUG_FMT("f=%p stride=%p id=%s\n", fragment, fragment->dataspace->stride, fragment->id);
	if (fragment->dataspace->stride) {
		/*
		 * The fragment appears to have a non-trivial stride.
		 * Linearize the fragment's data before writing.
		 */
		writeBuffer = malloc(fragment->bytes);
		eassert(writeBuffer != NULL);
		esdm_dataspace_t* contiguousSpace;
		esdm_dataspace_makeContiguous(fragment->dataspace,
					      &contiguousSpace);
		esdm_dataspace_copy_data(fragment->dataspace,
					 fragment->buf,
					 contiguousSpace,
					 writeBuffer);
		esdm_dataspace_destroy(contiguousSpace);

	} else {
		/*
		 * the fragment is a dense array in C index order,
		 * write it without any conversions.
		 */
		writeBuffer = fragment->buf;
	}

	/* 1. create a new object for this fragment if it doesn't exist. */
	if (fragment->id == NULL) {
		rc = esdm_backend_t_clovis_alloc(backend,
						 fragment->dataspace->dims,
						 NULL,
						 0,
						 NULL,
						 NULL,
						 &obj_id,
						 &obj_meta);
		if (rc != 0)
			goto err;

		/*
		 * Store this object's ID into fragment metadata. This metadata
		 * will be used later in retrieve() to read data from.
		 */
		fragment->id = strdup(obj_id);
	}


	DEBUG_FMT("Updating to %s (size=%lu)\n", fragment->id, fragment->bytes);
	// 2. open object with its object_id.
	void *obj_handle = NULL;
	rc = esdm_backend_t_clovis_open(backend, fragment->id, &obj_handle);
	DEBUG_FMT("open obj=%p rc=%d\n", obj_handle, rc);
	if (rc == 0) {
		// 3. write to this object.
		rc = esdm_backend_t_clovis_write(backend,
						 obj_handle,
						 0,
						 fragment->bytes,
						 writeBuffer);

		// 4. close this object.
		esdm_backend_t_clovis_close(backend, obj_handle);
	}

err:
	ebm_unlock(backend);
	if (mthreaded)
		revert_mero_thread_to_pthread();
	free(obj_id);
	free(obj_meta);
	if (fragment->dataspace->stride) {
		/* writeBuffer is allocated previously in this function. */
		free(writeBuffer);
	}
	DEBUG_LEAVE;
	return (rc == 0) ? ESDM_SUCCESS : ESDM_ERROR;
}

static int
esdm_backend_t_clovis_mkfs(esdm_backend_t *backend, int enforce_format)
{
	DEBUG_ENTER;
	if (!backend) {
		DEBUG_LEAVE;
		return ESDM_ERROR;
	}

	DEBUG_LEAVE;
	return ESDM_SUCCESS;
}

static float
esdm_backend_t_estimate_throughput(esdm_backend_t *b)
{
	DEBUG_ENTER;

	DEBUG_LEAVE;
	return 0.0;
}

static int
esdm_backend_t_fragment_create(esdm_backend_t *b, esdm_fragment_t *fragment)
{
	DEBUG_ENTER;

	DEBUG_LEAVE;
	return 0;
}

///////////////////////////////////////////////////////////////////////////////
// ESDM Module Registration ///////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////

esdm_backend_t_clovis_t esdm_backend_t_clovis = {
	.ebm_base = {
		 .name = "CLOVIS",
		 .type = ESDM_MODULE_DATA,
		 .version = "0.0.1",
		 .data = NULL,
		 .blocksize = BLOCKSIZE,
		 .callbacks = {
			 .finalize                 = esdm_backend_t_clovis_fini,
			 .performance_estimate     = esdm_backend_t_clovis_performance_estimate,
			 .estimate_throughput      = esdm_backend_t_estimate_throughput,
			 .fragment_create          = esdm_backend_t_fragment_create,
			 .fragment_retrieve        = esdm_backend_t_clovis_fragment_retrieve,
			 .fragment_update          = esdm_backend_t_clovis_fragment_update,
			 .fragment_delete          = NULL,
			 .fragment_metadata_create = NULL,
			 .fragment_metadata_load   = NULL,
			 .fragment_metadata_free   = NULL,
			 .mkfs                     = esdm_backend_t_clovis_mkfs,
			 .fsck                     = NULL
		},
	},
	.ebm_ops = {
		.esdm_backend_t_init      = esdm_backend_t_clovis_init,
		.esdm_backend_t_fini      = esdm_backend_t_clovis_fini,

		.esdm_backend_t_obj_alloc = esdm_backend_t_clovis_alloc,
		.esdm_backend_t_obj_open  = esdm_backend_t_clovis_open,
		.esdm_backend_t_obj_write = esdm_backend_t_clovis_write,
		.esdm_backend_t_obj_read  = esdm_backend_t_clovis_read,
		.esdm_backend_t_obj_close = esdm_backend_t_clovis_close,
		.mapping_get              = mapping_get,
		.mapping_insert           = mapping_insert,
	},
};

esdm_backend_t *
clovis_backend_init(esdm_config_backend_t *config)
{
	esdm_backend_t_clovis_t *ceb;
	char *target = NULL;
	int rc;

	DEBUG_ENTER;

	if (!config || !config->type || strcasecmp(config->type, "CLOVIS")
			|| !config->target) {
		DEBUG("Wrong configuration\n");
		return NULL;
	}
	ceb = malloc(sizeof esdm_backend_t_clovis);
	memcpy(ceb, &esdm_backend_t_clovis, sizeof(esdm_backend_t_clovis));

	DEBUG_FMT("backend type   = %s\n", config->type);
	DEBUG_FMT("backend id     = %s\n", config->id);
	DEBUG_FMT("backend target = %s\n", config->target);

	/*
	 *          "local_addr ha_addr profile process_fid"
	 * target = ":12345:33:103 172.16.154.130@tcp:12345:34:1 <0x7000000000000001:0> <0x7200000000000001:64>";
	 * Now please note the local_addr does not include the ipaddr.
	 */
	target = strdup(config->target);
	if (target == NULL)
		return NULL;

	rc = esdm_backend_t_clovis_init(target, &ceb->ebm_base);
	free(target);
	m0_mutex_init(&ceb->ebm_mutex);

	DEBUG_LEAVE;

	if (rc != 0)
		return NULL;
	else
		return &ceb->ebm_base;
}
