/*
 * Copyright © 2014 Broadcom
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#include "uapi/drm/vc4_drm.h"
#include "vc4_drv.h"

static uint32_t
bo_page_index(size_t size)
{
	return (size / PAGE_SIZE) - 1;
}

static struct list_head *
vc4_get_cache_list_for_size(struct drm_device *dev, size_t size)
{
	struct vc4_dev *vc4 = to_vc4_dev(dev);
	uint32_t page_index = bo_page_index(size);

	if (vc4->bo_cache.size_list_size <= page_index) {
		uint32_t new_size = max(vc4->bo_cache.size_list_size * 2,
					page_index + 1);
		struct list_head *new_list;
		uint32_t i;

		new_list = kmalloc(new_size * sizeof(struct list_head),
				   GFP_KERNEL);
		if (!new_list)
			return NULL;

		/* Rebase the old cached BO lists to their new list
		 * head locations.
		 */
		for (i = 0; i < vc4->bo_cache.size_list_size; i++) {
			struct list_head *old_list = &vc4->bo_cache.size_list[i];
			if (list_empty(old_list))
				INIT_LIST_HEAD(&new_list[i]);
			else
				list_replace(old_list, &new_list[i]);
		}
		/* And initialize the brand new BO list heads. */
		for (i = vc4->bo_cache.size_list_size; i < new_size; i++)
			INIT_LIST_HEAD(&new_list[i]);

		kfree(vc4->bo_cache.size_list);
		vc4->bo_cache.size_list = new_list;
		vc4->bo_cache.size_list_size = new_size;
	}

	return &vc4->bo_cache.size_list[page_index];
}

struct vc4_bo *
vc4_bo_create(struct drm_device *dev, size_t size)
{
	struct vc4_dev *vc4 = to_vc4_dev(dev);
	uint32_t page_index = bo_page_index(size);
	struct vc4_bo *bo = NULL;
	struct drm_gem_cma_object *cma_obj;

	/* First, try to get a vc4_bo from the kernel BO cache. */
	if (vc4->bo_cache.size_list_size > page_index) {
		if (!list_empty(&vc4->bo_cache.size_list[page_index])) {
			bo = list_first_entry(&vc4->bo_cache.size_list[page_index],
					      struct vc4_bo, size_head);
			list_del(&bo->size_head);
			list_del(&bo->unref_head);
		}
	}
	if (bo) {
		kref_init(&bo->base.base.refcount);
		return bo;
	}

	/* Otherwise, make a new BO. */
	cma_obj = drm_gem_cma_create(dev, size);
	if (IS_ERR(cma_obj))
		return NULL;
	else
		return to_vc4_bo(&cma_obj->base);
}

int
vc4_dumb_create(struct drm_file *file_priv,
		struct drm_device *dev,
		struct drm_mode_create_dumb *args)
{
	int min_pitch = DIV_ROUND_UP(args->width * args->bpp, 8);
	struct vc4_bo *bo = NULL;
	int ret;

	if (args->pitch < min_pitch)
		args->pitch = min_pitch;

	if (args->size < args->pitch * args->height)
		args->size = args->pitch * args->height;

	mutex_lock(&dev->struct_mutex);
	bo = vc4_bo_create(dev, roundup(args->size, PAGE_SIZE));
	mutex_unlock(&dev->struct_mutex);
	if (!bo)
		return -ENOMEM;

	ret = drm_gem_handle_create(file_priv, &bo->base.base, &args->handle);
	drm_gem_object_unreference_unlocked(&bo->base.base);

	return ret;
}

static void
vc4_bo_cache_free_old(struct drm_device *dev)
{
	struct vc4_dev *vc4 = to_vc4_dev(dev);
	unsigned long expire_time = jiffies - msecs_to_jiffies(1000);

	while (!list_empty(&vc4->bo_cache.time_list)) {
		struct vc4_bo *bo = list_last_entry(&vc4->bo_cache.time_list,
						    struct vc4_bo, unref_head);
		if (time_before(expire_time, bo->free_time)) {
			mod_timer(&vc4->bo_cache.time_timer,
				  round_jiffies_up(jiffies +
						   msecs_to_jiffies(1000)));
			return;
		}

		list_del(&bo->unref_head);
		list_del(&bo->size_head);
		drm_gem_cma_free_object(&bo->base.base);
	}
}

/* Called on the last userspace/kernel unreference of the BO.  Returns
 * it to the BO cache if possible, otherwise frees it.
 *
 * Note that this is called with the struct_mutex held.
 */
void
vc4_free_object(struct drm_gem_object *gem_bo)
{
	struct drm_device *dev = gem_bo->dev;
	struct vc4_dev *vc4 = to_vc4_dev(dev);
	struct vc4_bo *bo = to_vc4_bo(gem_bo);
	struct list_head *cache_list;

	/* If the object references someone else's memory, we can't cache it.
	 */
	if (gem_bo->import_attach) {
		drm_gem_cma_free_object(gem_bo);
		return;
	}

	/* Don't cache if it was publicly named. */
	if (gem_bo->name) {
		drm_gem_cma_free_object(gem_bo);
		return;
	}

	cache_list = vc4_get_cache_list_for_size(dev, gem_bo->size);
	if (!cache_list) {
		drm_gem_cma_free_object(gem_bo);
		return;
	}

	bo->free_time = jiffies;
	list_add(&bo->size_head, cache_list);
	list_add(&bo->unref_head, &vc4->bo_cache.time_list);

	vc4_bo_cache_free_old(dev);
}

static void
vc4_bo_cache_time_work(struct work_struct *work)
{
	struct vc4_dev *vc4 =
		container_of(work, struct vc4_dev, bo_cache.time_work);
	struct drm_device *dev = vc4->dev;

	mutex_lock(&dev->struct_mutex);
	vc4_bo_cache_free_old(dev);
	mutex_unlock(&dev->struct_mutex);
}

static void
vc4_bo_cache_time_timer(unsigned long data)
{
	struct drm_device *dev = (struct drm_device *)data;
	struct vc4_dev *vc4 = to_vc4_dev(dev);

	schedule_work(&vc4->bo_cache.time_work);
}

void
vc4_bo_cache_init(struct drm_device *dev)
{
	struct vc4_dev *vc4 = to_vc4_dev(dev);

	INIT_LIST_HEAD(&vc4->bo_cache.time_list);

	INIT_WORK(&vc4->bo_cache.time_work, vc4_bo_cache_time_work);
	setup_timer(&vc4->bo_cache.time_timer,
		    vc4_bo_cache_time_timer,
		    (unsigned long) dev);
}

