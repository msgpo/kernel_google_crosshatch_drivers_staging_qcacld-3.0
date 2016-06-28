/*
 * Copyright (c) 2015-2016 The Linux Foundation. All rights reserved.
 *
 * Previously licensed under the ISC license by Qualcomm Atheros, Inc.
 *
 *
 * Permission to use, copy, modify, and/or distribute this software for
 * any purpose with or without fee is hereby granted, provided that the
 * above copyright notice and this permission notice appear in all
 * copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL
 * WARRANTIES WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE
 * AUTHOR BE LIABLE FOR ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL
 * DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR
 * PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER
 * TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR
 * PERFORMANCE OF THIS SOFTWARE.
 */

/*
 * This file was originally distributed by Qualcomm Atheros, Inc.
 * under proprietary terms before Copyright ownership was assigned
 * to the Linux Foundation.
 */

/* OS abstraction libraries */
#include <qdf_nbuf.h>           /* qdf_nbuf_t, etc. */
#include <qdf_atomic.h>         /* qdf_atomic_read, etc. */
#include <qdf_util.h>           /* qdf_unlikely */

/* APIs for other modules */
#include <htt.h>                /* HTT_TX_EXT_TID_MGMT */
#include <ol_htt_tx_api.h>      /* htt_tx_desc_tid */

/* internal header files relevant for all systems */
#include <ol_txrx_internal.h>   /* TXRX_ASSERT1 */
#include <ol_tx_desc.h>         /* ol_tx_desc */
#include <ol_tx_send.h>         /* ol_tx_send */
#include <ol_txrx.h>            /* ol_txrx_get_vdev_from_vdev_id */

/* internal header files relevant only for HL systems */
#include <ol_tx_queue.h>        /* ol_tx_enqueue */

/* internal header files relevant only for specific systems (Pronto) */
#include <ol_txrx_encap.h>      /* OL_TX_ENCAP, etc */
#include <ol_tx.h>
#include <ol_cfg.h>

#define INVALID_FLOW_ID 0xFF
#define MAX_INVALID_BIN 3

#ifdef QCA_LL_TX_FLOW_GLOBAL_MGMT_POOL
#define TX_FLOW_MGMT_POOL_ID	0xEF
#define TX_FLOW_MGMT_POOL_SIZE  32

/**
 * ol_tx_register_global_mgmt_pool() - register global pool for mgmt packets
 * @pdev: pdev handler
 *
 * Return: none
 */
static void
ol_tx_register_global_mgmt_pool(struct ol_txrx_pdev_t *pdev)
{
	pdev->mgmt_pool = ol_tx_create_flow_pool(TX_FLOW_MGMT_POOL_ID,
						 TX_FLOW_MGMT_POOL_SIZE);
	if (!pdev->mgmt_pool) {
		TXRX_PRINT(TXRX_PRINT_LEVEL_ERR,
			 "Management pool creation failed\n");
	}
	return;
}

/**
 * ol_tx_deregister_global_mgmt_pool() - Deregister global pool for mgmt packets
 * @pdev: pdev handler
 *
 * Return: none
 */
static void
ol_tx_deregister_global_mgmt_pool(struct ol_txrx_pdev_t *pdev)
{
	ol_tx_delete_flow_pool(pdev->mgmt_pool, false);
	return;
}
#else
static inline void
ol_tx_register_global_mgmt_pool(struct ol_txrx_pdev_t *pdev)
{
	return;
}
static inline void
ol_tx_deregister_global_mgmt_pool(struct ol_txrx_pdev_t *pdev)
{
	return;
}
#endif

/**
 * ol_tx_register_flow_control() - Register fw based tx flow control
 * @pdev: pdev handle
 *
 * Return: none
 */
void ol_tx_register_flow_control(struct ol_txrx_pdev_t *pdev)
{
	qdf_spinlock_create(&pdev->tx_desc.flow_pool_list_lock);
	TAILQ_INIT(&pdev->tx_desc.flow_pool_list);

	if (!ol_tx_get_is_mgmt_over_wmi_enabled())
		ol_tx_register_global_mgmt_pool(pdev);
}

/**
 * ol_tx_deregister_flow_control() - Deregister fw based tx flow control
 * @pdev: pdev handle
 *
 * Return: none
 */
void ol_tx_deregister_flow_control(struct ol_txrx_pdev_t *pdev)
{
	int i = 0;
	struct ol_tx_flow_pool_t *pool = NULL;

	if (!ol_tx_get_is_mgmt_over_wmi_enabled())
		ol_tx_deregister_global_mgmt_pool(pdev);

	qdf_spin_lock_bh(&pdev->tx_desc.flow_pool_list_lock);
	while (!TAILQ_EMPTY(&pdev->tx_desc.flow_pool_list)) {
		pool = TAILQ_FIRST(&pdev->tx_desc.flow_pool_list);
		if (!pool)
			break;
		qdf_spin_unlock_bh(&pdev->tx_desc.flow_pool_list_lock);
		TXRX_PRINT(TXRX_PRINT_LEVEL_ERR,
			"flow pool list is not empty %d!!!\n", i++);
		if (i == 1)
			ol_tx_dump_flow_pool_info();
		ol_tx_delete_flow_pool(pool, true);
		qdf_spin_lock_bh(&pdev->tx_desc.flow_pool_list_lock);
	}
	qdf_spin_unlock_bh(&pdev->tx_desc.flow_pool_list_lock);
	qdf_spinlock_destroy(&pdev->tx_desc.flow_pool_list_lock);
}

/**
 * ol_tx_dump_flow_pool_info() - dump global_pool and flow_pool info
 *
 * Return: none
 */
void ol_tx_dump_flow_pool_info(void)
{
	struct ol_txrx_pdev_t *pdev = cds_get_context(QDF_MODULE_ID_TXRX);
	struct ol_tx_flow_pool_t *pool = NULL;
	struct ol_tx_flow_pool_t tmp_pool;


	TXRX_PRINT(TXRX_PRINT_LEVEL_ERR, "Global Pool");
	TXRX_PRINT(TXRX_PRINT_LEVEL_ERR, "Total %d :: Available %d",
		pdev->tx_desc.pool_size, pdev->tx_desc.num_free);
	TXRX_PRINT(TXRX_PRINT_LEVEL_ERR, "Invalid flow_pool %d",
		pdev->tx_desc.num_invalid_bin);
	TXRX_PRINT(TXRX_PRINT_LEVEL_ERR, "No of pool map received %d",
		pdev->pool_stats.pool_map_count);
	TXRX_PRINT(TXRX_PRINT_LEVEL_ERR, "No of pool unmap received %d",
		pdev->pool_stats.pool_unmap_count);
	TXRX_PRINT(TXRX_PRINT_LEVEL_ERR,
		"Pkt dropped due to unavailablity of pool %d",
		pdev->pool_stats.pkt_drop_no_pool);

	/*
	 * Nested spin lock.
	 * Always take in below order.
	 * flow_pool_list_lock -> flow_pool_lock
	 */
	qdf_spin_lock_bh(&pdev->tx_desc.flow_pool_list_lock);
	TAILQ_FOREACH(pool, &pdev->tx_desc.flow_pool_list,
					 flow_pool_list_elem) {
		qdf_spin_lock_bh(&pool->flow_pool_lock);
		qdf_mem_copy(&tmp_pool, pool, sizeof(tmp_pool));
		qdf_spin_unlock_bh(&pool->flow_pool_lock);
		qdf_spin_unlock_bh(&pdev->tx_desc.flow_pool_list_lock);
		TXRX_PRINT(TXRX_PRINT_LEVEL_ERR, "\n");
		TXRX_PRINT(TXRX_PRINT_LEVEL_ERR,
			"Flow_pool_id %d :: status %d",
			tmp_pool.flow_pool_id, tmp_pool.status);
		TXRX_PRINT(TXRX_PRINT_LEVEL_ERR,
			"Total %d :: Available %d :: Deficient %d",
			tmp_pool.flow_pool_size, tmp_pool.avail_desc,
			tmp_pool.deficient_desc);
		TXRX_PRINT(TXRX_PRINT_LEVEL_ERR,
			"Start threshold %d :: Stop threshold %d",
			 tmp_pool.start_th, tmp_pool.stop_th);
		TXRX_PRINT(TXRX_PRINT_LEVEL_ERR,
			"Member flow_id  %d :: flow_type %d",
			tmp_pool.member_flow_id, tmp_pool.flow_type);
		TXRX_PRINT(TXRX_PRINT_LEVEL_ERR,
			"Pkt dropped due to unavailablity of descriptors %d",
			tmp_pool.pkt_drop_no_desc);
		qdf_spin_lock_bh(&pdev->tx_desc.flow_pool_list_lock);
	}
	qdf_spin_unlock_bh(&pdev->tx_desc.flow_pool_list_lock);

	return;
}

/**
 * ol_tx_clear_flow_pool_stats() - clear flow pool statistics
 *
 * Return: none
 */
void ol_tx_clear_flow_pool_stats(void)
{
	struct ol_txrx_pdev_t *pdev = cds_get_context(QDF_MODULE_ID_TXRX);

	if (!pdev) {
		TXRX_PRINT(TXRX_PRINT_LEVEL_ERR, "%s: pdev is null\n",
						 __func__);
		return;
	}
	qdf_mem_zero(&pdev->pool_stats, sizeof(pdev->pool_stats));
}

/**
 * ol_tx_move_desc_n() - Move n descriptors from src_pool to dst_pool.
 * @src_pool: source pool
 * @dst_pool: destination pool
 * @desc_move_count: descriptor move count
 *
 * Return: actual descriptors moved
 */
static int ol_tx_move_desc_n(struct ol_tx_flow_pool_t *src_pool,
		      struct ol_tx_flow_pool_t *dst_pool,
		      int desc_move_count)
{
	uint16_t count = 0, i;
	struct ol_tx_desc_t *tx_desc;
	union ol_tx_desc_list_elem_t *temp_list = NULL;

	/* Take descriptors from source pool and put it in temp_list */
	qdf_spin_lock_bh(&src_pool->flow_pool_lock);
	for (i = 0; i < desc_move_count; i++) {
		tx_desc = ol_tx_get_desc_flow_pool(src_pool);
		((union ol_tx_desc_list_elem_t *)tx_desc)->next = temp_list;
		temp_list = (union ol_tx_desc_list_elem_t *)tx_desc;

	}
	qdf_spin_unlock_bh(&src_pool->flow_pool_lock);

	/* Take descriptors from temp_list and put it in destination pool */
	qdf_spin_lock_bh(&dst_pool->flow_pool_lock);
	for (i = 0; i < desc_move_count; i++) {
		if (dst_pool->deficient_desc)
			dst_pool->deficient_desc--;
		else
			break;
		tx_desc = &temp_list->tx_desc;
		temp_list = temp_list->next;
		ol_tx_put_desc_flow_pool(dst_pool, tx_desc);
		count++;
	}
	qdf_spin_unlock_bh(&dst_pool->flow_pool_lock);

	/* If anything is there in temp_list put it back to source pool */
	qdf_spin_lock_bh(&src_pool->flow_pool_lock);
	while (temp_list) {
		tx_desc = &temp_list->tx_desc;
		temp_list = temp_list->next;
		ol_tx_put_desc_flow_pool(src_pool, tx_desc);
	}
	qdf_spin_unlock_bh(&src_pool->flow_pool_lock);

	return count;
}


/**
 * ol_tx_distribute_descs_to_deficient_pools() - Distribute descriptors
 * @src_pool: source pool
 *
 * Distribute all descriptors of source pool to all
 * deficient pools as per flow_pool_list.
 *
 * Return: 0 for sucess
 */
int
ol_tx_distribute_descs_to_deficient_pools(struct ol_tx_flow_pool_t *src_pool)
{
	struct ol_txrx_pdev_t *pdev = cds_get_context(QDF_MODULE_ID_TXRX);
	struct ol_tx_flow_pool_t *dst_pool = NULL;
	uint16_t desc_count = src_pool->avail_desc;
	uint16_t desc_move_count = 0;

	if (!pdev) {
		TXRX_PRINT(TXRX_PRINT_LEVEL_ERR,
		   "%s: pdev is NULL\n", __func__);
		return -EINVAL;
	}
	qdf_spin_lock_bh(&pdev->tx_desc.flow_pool_list_lock);
	TAILQ_FOREACH(dst_pool, &pdev->tx_desc.flow_pool_list,
					 flow_pool_list_elem) {
		qdf_spin_lock_bh(&dst_pool->flow_pool_lock);
		if (dst_pool->deficient_desc) {
			desc_move_count =
				(dst_pool->deficient_desc > desc_count) ?
					desc_count : dst_pool->deficient_desc;
			qdf_spin_unlock_bh(&dst_pool->flow_pool_lock);
			desc_move_count = ol_tx_move_desc_n(src_pool,
						dst_pool, desc_move_count);
			desc_count -= desc_move_count;
			qdf_spin_lock_bh(&dst_pool->flow_pool_lock);
		}
		qdf_spin_unlock_bh(&dst_pool->flow_pool_lock);
		if (desc_count == 0)
			break;
	}
	qdf_spin_unlock_bh(&pdev->tx_desc.flow_pool_list_lock);

	return 0;
}


/**
 * ol_tx_create_flow_pool() - create flow pool
 * @flow_pool_id: flow pool id
 * @flow_pool_size: flow pool size
 *
 * Return: flow_pool pointer / NULL for error
 */
struct ol_tx_flow_pool_t *ol_tx_create_flow_pool(uint8_t flow_pool_id,
						 uint16_t flow_pool_size)
{
	struct ol_txrx_pdev_t *pdev = cds_get_context(QDF_MODULE_ID_TXRX);
	struct ol_tx_flow_pool_t *pool;
	uint16_t size = 0, i;
	struct ol_tx_desc_t *tx_desc;
	union ol_tx_desc_list_elem_t *temp_list = NULL;
	uint32_t stop_threshold =
			ol_cfg_get_tx_flow_stop_queue_th(pdev->ctrl_pdev);
	uint32_t start_threshold = stop_threshold +
			ol_cfg_get_tx_flow_start_queue_offset(pdev->ctrl_pdev);

	if (!pdev) {
		TXRX_PRINT(TXRX_PRINT_LEVEL_ERR,
		   "%s: pdev is NULL\n", __func__);
		return NULL;
	}

	pool = qdf_mem_malloc(sizeof(*pool));
	if (!pool) {
		TXRX_PRINT(TXRX_PRINT_LEVEL_ERR,
		   "%s: malloc failed\n", __func__);
		return NULL;
	}

	pool->flow_pool_id = flow_pool_id;
	pool->flow_pool_size = flow_pool_size;
	pool->status = FLOW_POOL_ACTIVE_UNPAUSED;
	pool->start_th = (start_threshold * flow_pool_size)/100;
	pool->stop_th = (stop_threshold * flow_pool_size)/100;
	qdf_spinlock_create(&pool->flow_pool_lock);

	/* Take TX descriptor from global_pool and put it in temp_list*/
	qdf_spin_lock_bh(&pdev->tx_mutex);
	if (pdev->tx_desc.num_free >= pool->flow_pool_size)
		size = pool->flow_pool_size;
	else
		size = pdev->tx_desc.num_free;

	for (i = 0; i < size; i++) {
		tx_desc = ol_tx_get_desc_global_pool(pdev);
		tx_desc->pool = pool;
		((union ol_tx_desc_list_elem_t *)tx_desc)->next = temp_list;
		temp_list = (union ol_tx_desc_list_elem_t *)tx_desc;

	}
	qdf_spin_unlock_bh(&pdev->tx_mutex);

	/* put temp_list to flow_pool */
	pool->freelist = temp_list;
	pool->avail_desc = size;
	pool->deficient_desc = pool->flow_pool_size - pool->avail_desc;

	/* Add flow_pool to flow_pool_list */
	qdf_spin_lock_bh(&pdev->tx_desc.flow_pool_list_lock);
	TAILQ_INSERT_TAIL(&pdev->tx_desc.flow_pool_list, pool,
			 flow_pool_list_elem);
	qdf_spin_unlock_bh(&pdev->tx_desc.flow_pool_list_lock);

	return pool;
}

/**
 * ol_tx_delete_flow_pool() - delete flow pool
 * @pool: flow pool pointer
 * @force: free pool forcefully
 *
 * Delete flow_pool if all tx descriptors are available.
 * Otherwise put it in FLOW_POOL_INVALID state.
 * If force is set then pull all available descriptors to
 * global pool.
 *
 * Return: 0 for success or error
 */
int ol_tx_delete_flow_pool(struct ol_tx_flow_pool_t *pool, bool force)
{
	struct ol_txrx_pdev_t *pdev = cds_get_context(QDF_MODULE_ID_TXRX);
	uint16_t i, size;
	union ol_tx_desc_list_elem_t *temp_list = NULL;
	struct ol_tx_desc_t *tx_desc = NULL;

	if (!pool) {
		TXRX_PRINT(TXRX_PRINT_LEVEL_ERR,
		   "%s: pool is NULL\n", __func__);
		return -ENOMEM;
	}

	qdf_spin_lock_bh(&pdev->tx_desc.flow_pool_list_lock);
	TAILQ_REMOVE(&pdev->tx_desc.flow_pool_list, pool, flow_pool_list_elem);
	qdf_spin_unlock_bh(&pdev->tx_desc.flow_pool_list_lock);

	qdf_spin_lock_bh(&pool->flow_pool_lock);
	if (pool->avail_desc == pool->flow_pool_size || force == true)
		pool->status = FLOW_POOL_INACTIVE;
	else
		pool->status = FLOW_POOL_INVALID;

	/* Take all free descriptors and put it in temp_list */
	temp_list = pool->freelist;
	size = pool->avail_desc;
	pool->freelist = NULL;
	pool->avail_desc = 0;

	if (pool->status == FLOW_POOL_INACTIVE) {
		qdf_spin_unlock_bh(&pool->flow_pool_lock);
		/* Free flow_pool */
		qdf_spinlock_destroy(&pool->flow_pool_lock);
		qdf_mem_free(pool);
	} else { /* FLOW_POOL_INVALID case*/
		pool->flow_pool_size -= size;
		pool->flow_pool_id = INVALID_FLOW_ID;
		qdf_spin_unlock_bh(&pool->flow_pool_lock);

		pdev->tx_desc.num_invalid_bin++;
		TXRX_PRINT(TXRX_PRINT_LEVEL_ERR,
			"%s: invalid pool created %d\n",
			 __func__, pdev->tx_desc.num_invalid_bin);
		if (pdev->tx_desc.num_invalid_bin > MAX_INVALID_BIN)
			ASSERT(0);

		qdf_spin_lock_bh(&pdev->tx_desc.flow_pool_list_lock);
		TAILQ_INSERT_TAIL(&pdev->tx_desc.flow_pool_list, pool,
				 flow_pool_list_elem);
		qdf_spin_unlock_bh(&pdev->tx_desc.flow_pool_list_lock);
	}

	/* put free descriptors to global pool */
	qdf_spin_lock_bh(&pdev->tx_mutex);
	for (i = 0; i < size; i++) {
		tx_desc = &temp_list->tx_desc;
		temp_list = temp_list->next;

		ol_tx_put_desc_global_pool(pdev, tx_desc);
	}
	qdf_spin_unlock_bh(&pdev->tx_mutex);

	return 0;
}


/**
 * ol_tx_free_invalid_flow_pool() - free invalid pool
 * @pool: pool
 *
 * Return: 0 for success or failure
 */
int ol_tx_free_invalid_flow_pool(struct ol_tx_flow_pool_t *pool)
{
	struct ol_txrx_pdev_t *pdev = cds_get_context(QDF_MODULE_ID_TXRX);

	if ((!pdev) || (!pool) || (pool->status != FLOW_POOL_INVALID)) {
		TXRX_PRINT(TXRX_PRINT_LEVEL_ERR,
		   "%s: Invalid pool/pdev\n", __func__);
		return -EINVAL;
	}

	/* direclty distribute to other deficient pools */
	ol_tx_distribute_descs_to_deficient_pools(pool);

	qdf_spin_lock_bh(&pool->flow_pool_lock);
	pool->flow_pool_size = pool->avail_desc;
	qdf_spin_unlock_bh(&pool->flow_pool_lock);

	pdev->tx_desc.num_invalid_bin--;
	TXRX_PRINT(TXRX_PRINT_LEVEL_ERR,
		"%s: invalid pool deleted %d\n",
		 __func__, pdev->tx_desc.num_invalid_bin);

	return ol_tx_delete_flow_pool(pool, false);
}

/**
 * ol_tx_get_flow_pool() - get flow_pool from flow_pool_id
 * @flow_pool_id: flow pool id
 *
 * Return: flow_pool ptr / NULL if not found
 */
struct ol_tx_flow_pool_t *ol_tx_get_flow_pool(uint8_t flow_pool_id)
{
	struct ol_txrx_pdev_t *pdev = cds_get_context(QDF_MODULE_ID_TXRX);
	struct ol_tx_flow_pool_t *pool = NULL;
	bool is_found = false;

	qdf_spin_lock_bh(&pdev->tx_desc.flow_pool_list_lock);
	TAILQ_FOREACH(pool, &pdev->tx_desc.flow_pool_list,
					 flow_pool_list_elem) {
		qdf_spin_lock_bh(&pool->flow_pool_lock);
		if (pool->flow_pool_id == flow_pool_id) {
			qdf_spin_unlock_bh(&pool->flow_pool_lock);
			is_found = true;
			break;
		}
		qdf_spin_unlock_bh(&pool->flow_pool_lock);
	}
	qdf_spin_unlock_bh(&pdev->tx_desc.flow_pool_list_lock);

	if (is_found == false)
		pool = NULL;

	return pool;

}


/**
 * ol_tx_flow_pool_vdev_map() - Map flow_pool with vdev
 * @pool: flow_pool
 * @vdev_id: flow_id /vdev_id
 *
 * Return: none
 */
void ol_tx_flow_pool_vdev_map(struct ol_tx_flow_pool_t *pool,
			      uint8_t vdev_id)
{
	ol_txrx_vdev_handle vdev;

	vdev = ol_txrx_get_vdev_from_vdev_id(vdev_id);
	if (!vdev) {
		TXRX_PRINT(TXRX_PRINT_LEVEL_ERR,
		   "%s: invalid vdev_id %d\n",
		   __func__, vdev_id);
		return;
	}

	vdev->pool = pool;
	qdf_spin_lock_bh(&pool->flow_pool_lock);
	pool->member_flow_id = vdev_id;
	qdf_spin_unlock_bh(&pool->flow_pool_lock);

	return;
}

/**
 * ol_tx_flow_pool_vdev_unmap() - Unmap flow_pool from vdev
 * @pool: flow_pool
 * @vdev_id: flow_id /vdev_id
 *
 * Return: none
 */
void ol_tx_flow_pool_vdev_unmap(struct ol_tx_flow_pool_t *pool,
				uint8_t vdev_id)
{
	ol_txrx_vdev_handle vdev;

	vdev = ol_txrx_get_vdev_from_vdev_id(vdev_id);
	if (!vdev) {
		TXRX_PRINT(TXRX_PRINT_LEVEL_ERR,
		   "%s: invalid vdev_id %d\n",
		   __func__, vdev_id);
		return;
	}

	vdev->pool = NULL;
	qdf_spin_lock_bh(&pool->flow_pool_lock);
	pool->member_flow_id = INVALID_FLOW_ID;
	qdf_spin_unlock_bh(&pool->flow_pool_lock);

	return;
}

/**
 * ol_tx_flow_pool_map_handler() - Map flow_id with pool of descriptors
 * @flow_id: flow id
 * @flow_type: flow type
 * @flow_pool_id: pool id
 * @flow_pool_size: pool size
 *
 * Process below target to host message
 * HTT_T2H_MSG_TYPE_FLOW_POOL_MAP
 *
 * Return: none
 */
void ol_tx_flow_pool_map_handler(uint8_t flow_id, uint8_t flow_type,
				 uint8_t flow_pool_id, uint16_t flow_pool_size)
{
	struct ol_txrx_pdev_t *pdev = cds_get_context(QDF_MODULE_ID_TXRX);
	struct ol_tx_flow_pool_t *pool;
	uint8_t pool_create = 0;
	enum htt_flow_type type = flow_type;


	TXRX_PRINT(TXRX_PRINT_LEVEL_INFO1,
		"%s: flow_id %d flow_type %d flow_pool_id %d flow_pool_size %d\n",
		__func__, flow_id, flow_type, flow_pool_id, flow_pool_size);

	if (qdf_unlikely(!pdev)) {
		TXRX_PRINT(TXRX_PRINT_LEVEL_ERR,
			"%s: pdev is NULL", __func__);
		return;
	}
	pdev->pool_stats.pool_map_count++;

	pool = ol_tx_get_flow_pool(flow_pool_id);
	if (!pool) {
		pool = ol_tx_create_flow_pool(flow_pool_id, flow_pool_size);
		if (pool == NULL) {
			TXRX_PRINT(TXRX_PRINT_LEVEL_ERR,
				   "%s: creation of flow_pool %d size %d failed\n",
				   __func__, flow_pool_id, flow_pool_size);
			return;
		}
		pool_create = 1;
	}

	switch (type) {

	case FLOW_TYPE_VDEV:
		ol_tx_flow_pool_vdev_map(pool, flow_id);
		break;
	default:
		if (pool_create)
			ol_tx_delete_flow_pool(pool, false);
		TXRX_PRINT(TXRX_PRINT_LEVEL_ERR,
		   "%s: flow type %d not supported !!!\n",
		   __func__, type);
		break;
	}

	return;
}

/**
 * ol_tx_flow_pool_unmap_handler() - Unmap flow_id from pool of descriptors
 * @flow_id: flow id
 * @flow_type: flow type
 * @flow_pool_id: pool id
 *
 * Process below target to host message
 * HTT_T2H_MSG_TYPE_FLOW_POOL_UNMAP
 *
 * Return: none
 */
void ol_tx_flow_pool_unmap_handler(uint8_t flow_id, uint8_t flow_type,
							  uint8_t flow_pool_id)
{
	struct ol_txrx_pdev_t *pdev = cds_get_context(QDF_MODULE_ID_TXRX);
	struct ol_tx_flow_pool_t *pool;
	enum htt_flow_type type = flow_type;

	TXRX_PRINT(TXRX_PRINT_LEVEL_INFO1,
		"%s: flow_id %d flow_type %d flow_pool_id %d\n",
		__func__, flow_id, flow_type, flow_pool_id);

	if (qdf_unlikely(!pdev)) {
		TXRX_PRINT(TXRX_PRINT_LEVEL_ERR,
			"%s: pdev is NULL", __func__);
		return;
	}
	pdev->pool_stats.pool_unmap_count++;

	pool = ol_tx_get_flow_pool(flow_pool_id);
	if (!pool) {
		TXRX_PRINT(TXRX_PRINT_LEVEL_ERR,
		   "%s: flow_pool not available flow_pool_id %d\n",
		   __func__, type);
		return;
	}

	switch (type) {

	case FLOW_TYPE_VDEV:
		ol_tx_flow_pool_vdev_unmap(pool, flow_id);
		break;
	default:
		TXRX_PRINT(TXRX_PRINT_LEVEL_ERR,
		   "%s: flow type %d not supported !!!\n",
		   __func__, type);
		return;
	}

	/* only delete if all descriptors are available */
	ol_tx_delete_flow_pool(pool, false);

	return;
}


