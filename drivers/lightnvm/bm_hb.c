/*
 * Copyright: Matias Bjorling <mb@lightnvm.io>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License version
 * 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * Implementation of a block manager for hybrid open-channel SSD.
 */

#include "bm_hb.h"

static void hb_blocks_free(struct nvm_dev *dev)
{
	struct bm_hb *bm = dev->bmp;
	struct nvm_lun *lun;
	int i;

	bm_for_each_lun(dev, bm, lun, i) {
		if (!lun->blocks)
			break;
		vfree(lun->blocks);
	}
}

static void hb_luns_free(struct nvm_dev *dev)
{
	struct bm_hb *bm = dev->bmp;

	kfree(bm->luns);
}

static int hb_luns_init(struct nvm_dev *dev, struct bm_hb *bm)
{
	struct nvm_lun *lun;
	struct nvm_id_chnl *chnl;
	int i;

	bm->luns = kcalloc(dev->nr_luns, sizeof(struct nvm_lun), GFP_KERNEL);
	if (!bm->luns)
		return -ENOMEM;

	bm_for_each_lun(dev, bm, lun, i) {
		chnl = &dev->identity.chnls[i];
		pr_info("bm_hb: p %u qsize %u gr %u ge %u begin %llu end %llu\n",
			i, chnl->queue_size, chnl->gran_read, chnl->gran_erase,
			chnl->laddr_begin, chnl->laddr_end);

		spin_lock_init(&lun->lock);

		INIT_LIST_HEAD(&lun->free_list);
		INIT_LIST_HEAD(&lun->used_list);
		INIT_LIST_HEAD(&lun->bb_list);

		lun->id = i;
		lun->dev = dev;
		lun->chnl = chnl;
		lun->reserved_blocks = 2; /* for GC only */
		lun->nr_blocks =
				(chnl->laddr_end - chnl->laddr_begin + 1) /
				(chnl->gran_erase / chnl->gran_read);
		lun->nr_free_blocks = lun->nr_blocks;
		lun->nr_pages_per_blk = chnl->gran_erase / chnl->gran_write *
					(chnl->gran_write / dev->sector_size);

		if (lun->nr_pages_per_blk > dev->max_pages_per_blk)
			dev->max_pages_per_blk = lun->nr_pages_per_blk;

		dev->total_pages += lun->nr_blocks * lun->nr_pages_per_blk;
		dev->total_blocks += lun->nr_blocks;

		if (lun->nr_pages_per_blk >
				MAX_INVALID_PAGES_STORAGE * BITS_PER_LONG) {
			pr_err("bm_hb: number of pages per block too high.");
			return -EINVAL;
		}
	}

	return 0;
}

static int hb_block_bb(u32 lun_id, void *bb_bitmap, unsigned int nr_blocks,
								void *private)
{
	struct bm_hb *bm = private;
	struct nvm_lun *lun = &bm->luns[lun_id];
	struct nvm_block *block;
	int i;

	if (unlikely(bitmap_empty(bb_bitmap, nr_blocks)))
		return 0;

	i = -1;
	while ((i = find_next_bit(bb_bitmap, nr_blocks, i + 1)) <
			nr_blocks) {
		block = &lun->blocks[i];
		if (!block) {
			pr_err("bm_hb: BB data is out of bounds!\n");
			return -EINVAL;
		}
		list_move_tail(&block->list, &lun->bb_list);
	}

	return 0;
}

static int hb_block_map(u64 slba, u64 nlb, u64 *entries, void *private)
{
	struct nvm_dev *dev = private;
	struct bm_hb *bm = dev->bmp;
	sector_t max_pages = dev->total_pages * (dev->sector_size >> 9);
	u64 elba = slba + nlb;
	struct nvm_lun *lun;
	struct nvm_block *blk;
	sector_t total_pgs_per_lun = /* each lun have the same configuration */
		   bm->luns[0].nr_blocks * bm->luns[0].nr_pages_per_blk;
	u64 i;
	int lun_id;

	if (unlikely(elba > dev->total_pages)) {
		pr_err("bm_hb: L2P data from device is out of bounds!\n");
		return -EINVAL;
	}

	for (i = 0; i < nlb; i++) {
		u64 pba = le64_to_cpu(entries[i]);

		if (unlikely(pba >= max_pages && pba != U64_MAX)) {
			pr_err("bm_hb: L2P data entry is out of bounds!\n");
			return -EINVAL;
		}

		/* Address zero is a special one. The first page on a disk is
		 * protected. As it often holds internal device boot
		 * information. */
		if (!pba)
			continue;

		/* resolve block from physical address */
		lun_id = pba / total_pgs_per_lun;
		lun = &bm->luns[lun_id];

		/* Calculate block offset into lun */
		pba = pba - (total_pgs_per_lun * lun_id);
		blk = &lun->blocks[pba / lun->nr_pages_per_blk];

		if (!blk->type) {
			/* at this point, we don't know anything about the
			 * block. It's up to the FTL on top to re-etablish the
			 * block state */
			list_move_tail(&blk->list, &lun->used_list);
			blk->type = 1;
			lun->nr_free_blocks--;
		}
	}

	return 0;
}

static int hb_blocks_init(struct nvm_dev *dev, struct bm_hb *bm)
{
	struct nvm_lun *lun;
	struct nvm_block *block;
	sector_t lun_iter, block_iter, cur_block_id = 0;
	int ret;

	bm_for_each_lun(dev, bm, lun, lun_iter) {
		lun->blocks = vzalloc(sizeof(struct nvm_block) *
						lun->nr_blocks);
		if (!lun->blocks)
			return -ENOMEM;

		lun_for_each_block(lun, block, block_iter) {
			spin_lock_init(&block->lock);
			INIT_LIST_HEAD(&block->list);

			block->lun = lun;
			block->id = cur_block_id++;

			/* First block is reserved for device */
			if (unlikely(lun_iter == 0 && block_iter == 0))
				continue;

			list_add_tail(&block->list, &lun->free_list);
		}

		if (dev->ops->get_bb_tbl) {
			ret = dev->ops->get_bb_tbl(dev->q, lun->id,
			lun->nr_blocks, hb_block_bb, bm);
			if (ret)
				pr_err("bm_hb: could not read BB table\n");
		}
	}

	if (dev->ops->get_l2p_tbl) {
		ret = dev->ops->get_l2p_tbl(dev->q, 0, dev->total_pages,
							hb_block_map, dev);
		if (ret) {
			pr_err("bm_hb: could not read L2P table.\n");
			pr_warn("bm_hb: default block initialization");
		}
	}

	return 0;
}

static int hb_register(struct nvm_dev *dev)
{
	struct bm_hb *bm;
	int ret;

	if (!dev->features.rsp & NVM_RSP_L2P)
		return 0;

	bm = kzalloc(sizeof(struct bm_hb), GFP_KERNEL);
	if (!bm)
		return -ENOMEM;

	dev->bmp = bm;

	ret = hb_luns_init(dev, bm);
	if (ret) {
		pr_err("bm_hb: could not initialize luns\n");
		goto err;
	}

	ret = hb_blocks_init(dev, bm);
	if (ret) {
		pr_err("bm_hb: could not initialize blocks\n");
		goto err;
	}

	return 1;
err:
	kfree(bm);
	return ret;
}

static void hb_unregister(struct nvm_dev *dev)
{
	hb_blocks_free(dev);
	hb_luns_free(dev);
	kfree(dev->bmp);
	dev->bmp = NULL;
}

static void nvm_reset_block(struct nvm_lun *lun, struct nvm_block *block)
{
	spin_lock(&block->lock);
	bitmap_zero(block->invalid_pages, lun->nr_pages_per_blk);
	block->next_page = 0;
	block->nr_invalid_pages = 0;
	atomic_set(&block->data_cmnt_size, 0);
	spin_unlock(&block->lock);
}

static struct nvm_block *hb_get_blk(struct nvm_dev *dev, struct nvm_lun *lun,
							unsigned long flags)
{
	struct nvm_block *block = NULL;
	int is_gc = flags & NVM_IOTYPE_GC;

	BUG_ON(!lun);

	spin_lock(&lun->lock);

	if (list_empty(&lun->free_list)) {
		pr_err_ratelimited("bm_hb: lun %u have no free pages available",
								lun->id);
		spin_unlock(&lun->lock);
		goto out;
	}

	while (!is_gc && lun->nr_free_blocks < lun->reserved_blocks) {
		spin_unlock(&lun->lock);
		goto out;
	}

	block = list_first_entry(&lun->free_list, struct nvm_block, list);
	list_move_tail(&block->list, &lun->used_list);

	lun->nr_free_blocks--;

	spin_unlock(&lun->lock);

	nvm_reset_block(lun, block);

out:
	return block;
}

static void hb_put_blk(struct nvm_dev *dev, struct nvm_block *blk)
{
	struct nvm_lun *lun = blk->lun;

	spin_lock(&lun->lock);

	list_move_tail(&blk->list, &lun->free_list);
	lun->nr_free_blocks++;

	spin_unlock(&lun->lock);
}

static int hb_submit_io(struct nvm_dev *dev, struct nvm_rq *rqd)
{
	if (!dev->ops->submit_io)
		return 0;

	return dev->ops->submit_io(dev->q, rqd);
}

static void hb_end_io(struct nvm_rq *rqd, int error)
{
	struct nvm_tgt_instance *ins = rqd->ins;

	ins->tt->end_io(rqd, error);
}

static int hb_erase_blk(struct nvm_dev *dev, struct nvm_block *blk)
{
	if (!dev->ops->erase_block)
		return 0;

	return dev->ops->erase_block(dev->q, blk->id);
}

static struct nvm_lun *hb_get_luns(struct nvm_dev *dev, int begin, int end)
{
	struct bm_hb *bm = dev->bmp;

	return bm->luns + begin;
}

static void hb_free_blocks_print(struct nvm_dev *dev)
{
	struct bm_hb *bm = dev->bmp;
	struct nvm_lun *lun;
	unsigned int i;

	bm_for_each_lun(dev, bm, lun, i)
		pr_info("%s: lun%8u\t%u\n", dev->name, i, lun->nr_free_blocks);
}

static struct nvm_bm_type bm_hb = {
	.name		= "hb",

	.register_bm	= hb_register,
	.unregister_bm	= hb_unregister,

	.get_blk	= hb_get_blk,
	.put_blk	= hb_put_blk,

	.submit_io	= hb_submit_io,
	.end_io		= hb_end_io,
	.erase_blk	= hb_erase_blk,

	.get_luns	= hb_get_luns,
	.free_blocks_print = hb_free_blocks_print,
};

static int __init hb_module_init(void)
{
	return nvm_register_bm(&bm_hb);
}

static void hb_module_exit(void)
{
	nvm_unregister_bm(&bm_hb);
}

module_init(hb_module_init);
module_exit(hb_module_exit);
MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("Block manager for Hybrid Open-Channel SSDs");
