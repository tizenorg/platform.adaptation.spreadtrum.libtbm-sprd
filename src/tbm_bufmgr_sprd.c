/**************************************************************************

libtbm_sprd

Copyright 2012 Samsung Electronics co., Ltd. All Rights Reserved.

Contact: SooChan Lim <sc1.lim@samsung.com>, Sangjin Lee <lsj119@samsung.com>

Permission is hereby granted, free of charge, to any person obtaining a
copy of this software and associated documentation files (the
"Software"), to deal in the Software without restriction, including
without limitation the rights to use, copy, modify, merge, publish,
distribute, sub license, and/or sell copies of the Software, and to
permit persons to whom the Software is furnished to do so, subject to
the following conditions:

The above copyright notice and this permission notice (including the
next paragraph) shall be included in all copies or substantial portions
of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT.
IN NO EVENT SHALL PRECISION INSIGHT AND/OR ITS SUPPLIERS BE LIABLE FOR
ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

**************************************************************************/

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#if HAVE_UDEV
#include <libudev.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>
#include <xf86drm.h>
#include <tbm_bufmgr.h>
#include <tbm_bufmgr_backend.h>
#include <drm/sprd_drm.h>
#include <pthread.h>
#include <tbm_surface.h>
#include <tbm_drm_helper.h>

#define DEBUG
#include "tbm_bufmgr_tgl.h"

//#define USE_CONTIG_ONLY
#define USE_DMAIMPORT

#define TBM_COLOR_FORMAT_COUNT 8

#ifdef DEBUG
#define LOG_TAG    "TBM_BACKEND"
#include <dlog.h>
static int bDebug = 0;

#define SPRD_DRM_NAME "sprd"

char *
target_name()
{
	FILE *f;
	char *slash;
	static int     initialized = 0;
	static char app_name[128];

	if ( initialized )
		return app_name;

	/* get the application name */
	f = fopen("/proc/self/cmdline", "r");

	if ( !f ) {
		return 0;
	}

	memset(app_name, 0x00, sizeof(app_name));

	if ( fgets(app_name, 100, f) == NULL ) {
		fclose(f);
		return 0;
	}

	fclose(f);

	if ( (slash = strrchr(app_name, '/')) != NULL ) {
		memmove(app_name, slash + 1, strlen(slash));
	}

	initialized = 1;

	return app_name;
}
#define TBM_SPRD_LOG(fmt, args...)        LOGE("\033[31m"  "[%s]" fmt "\033[0m", target_name(), ##args)
#define DBG(fmt, args...)        if(bDebug&01) LOGE("[%s]" fmt, target_name(), ##args)
#else
#define TBM_SPRD_LOG(...)
#define DBG(...)
#endif

#define SIZE_ALIGN( value, base ) (((value) + ((base) - 1)) & ~((base) - 1))

#define TBM_SURFACE_ALIGNMENT_PLANE (64)
#define TBM_SURFACE_ALIGNMENT_PITCH_RGB (128)
#define TBM_SURFACE_ALIGNMENT_PITCH_YUV (16)


/* check condition */
#define SPRD_RETURN_IF_FAIL(cond) {\
    if (!(cond)) {\
        TBM_SPRD_LOG ("[%s] : '%s' failed.\n", __FUNCTION__, #cond);\
        return;\
    }\
}
#define SPRD_RETURN_VAL_IF_FAIL(cond, val) {\
    if (!(cond)) {\
        TBM_SPRD_LOG ("[%s] : '%s' failed.\n", __FUNCTION__, #cond);\
        return val;\
    }\
}

struct dma_buf_info {
	unsigned long    size;
	unsigned int    fence_supported;
	unsigned int    padding;
};

#define DMA_BUF_ACCESS_READ        0x1
#define DMA_BUF_ACCESS_WRITE        0x2
#define DMA_BUF_ACCESS_DMA        0x4
#define DMA_BUF_ACCESS_MAX        0x8

#define DMA_FENCE_LIST_MAX         5

struct dma_buf_fence {
	unsigned long        ctx;
	unsigned int        type;
};

#define DMABUF_IOCTL_BASE    'F'
#define DMABUF_IOWR(nr, type)    _IOWR(DMABUF_IOCTL_BASE, nr, type)

#define DMABUF_IOCTL_GET_INFO    DMABUF_IOWR(0x00, struct dma_buf_info)
#define DMABUF_IOCTL_GET_FENCE    DMABUF_IOWR(0x01, struct dma_buf_fence)
#define DMABUF_IOCTL_PUT_FENCE    DMABUF_IOWR(0x02, struct dma_buf_fence)

/* tgl key values */
#define GLOBAL_KEY   ((unsigned int)(-1))
/* TBM_CACHE */
#define TBM_SPRD_CACHE_INV       0x01 /**< cache invalidate  */
#define TBM_SPRD_CACHE_CLN       0x02 /**< cache clean */
#define TBM_SPRD_CACHE_ALL       0x10 /**< cache all */
#define TBM_SPRD_CACHE_FLUSH     (TBM_SPRD_CACHE_INV|TBM_SPRD_CACHE_CLN) /**< cache flush  */
#define TBM_SPRD_CACHE_FLUSH_ALL (TBM_SPRD_CACHE_FLUSH|TBM_SPRD_CACHE_ALL)	/**< cache flush all */

enum {
	DEVICE_NONE = 0,
	DEVICE_CA,					/* cache aware device */
	DEVICE_CO					/* cache oblivious device */
};

typedef union _tbm_bo_cache_state tbm_bo_cache_state;

union _tbm_bo_cache_state {
	unsigned int val;
	struct {
		unsigned int cntFlush: 16;	/*Flush all index for sync */
		unsigned int isCached: 1;
		unsigned int isDirtied: 2;
	} data;
};

typedef struct _tbm_bufmgr_sprd *tbm_bufmgr_sprd;
typedef struct _tbm_bo_sprd *tbm_bo_sprd;

typedef struct _sprd_private {
	int ref_count;
	struct _tbm_bo_sprd *bo_priv;
} PrivGem;

/* tbm buffor object for sprd */
struct _tbm_bo_sprd {
	int fd;

	unsigned int name;    /* FLINK ID */

	unsigned int gem;     /* GEM Handle */

	unsigned int dmabuf;  /* fd for dmabuf */

	void *pBase;          /* virtual address */

	unsigned int size;

	unsigned int flags_sprd;
	unsigned int flags_tbm;

	PrivGem *private;

	pthread_mutex_t mutex;
	struct dma_buf_fence dma_fence[DMA_FENCE_LIST_MAX];
	int device;
	int opt;

	tbm_bo_cache_state cache_state;
	unsigned int map_cnt;
};

/* tbm bufmgr private for sprd */
struct _tbm_bufmgr_sprd {
	int fd;
	void *hashBos;

	int use_dma_fence;

	int tgl_fd;

	void *bind_display;

	char *device_name;
};

char *STR_DEVICE[] = {
	"DEF",
	"CPU",
	"2D",
	"3D",
	"MM"
};

char *STR_OPT[] = {
	"NONE",
	"RD",
	"WR",
	"RDWR"
};


uint32_t tbm_sprd_color_format_list[TBM_COLOR_FORMAT_COUNT] = { TBM_FORMAT_RGBA8888,
								TBM_FORMAT_BGRA8888,
								TBM_FORMAT_RGBX8888,
								TBM_FORMAT_RGB888,
								TBM_FORMAT_NV12,
								TBM_FORMAT_NV21,
								TBM_FORMAT_YUV420,
								TBM_FORMAT_YVU420
							      };

static inline int
_tgl_init(int fd, unsigned int key)
{
	struct tgl_attribute attr;
	int err;

	attr.key = key;
	attr.timeout_ms = 1000;

	err = ioctl(fd, TGL_IOC_INIT_LOCK, &attr);
	if (err) {
		TBM_SPRD_LOG("[libtbm-sprd:%d] error(%s) %s:%d key:%d\n",
			     getpid(), strerror(errno), __func__, __LINE__, key);
		return 0;
	}

	return 1;
}

static inline int
_tgl_destroy(int fd, unsigned int key)
{
	int err;

	err = ioctl(fd, TGL_IOC_DESTROY_LOCK, key);
	if (err) {
		TBM_SPRD_LOG("[libtbm-sprd:%d] "
			     "error(%s) %s:%d key:%d\n",
			     getpid(), strerror(errno), __func__, __LINE__, key);
		return 0;
	}

	return 1;
}

static inline int
_tgl_lock(int fd, unsigned int key)
{
	int err;

	err = ioctl(fd, TGL_IOC_LOCK_LOCK, key);
	if (err) {
		TBM_SPRD_LOG("[libtbm-sprd:%d] "
			     "error(%s) %s:%d key:%d\n",
			     getpid(), strerror(errno), __func__, __LINE__, key);
		return 0;
	}

	return 1;
}

static inline int
_tgl_unlock(int fd, unsigned int key)
{
	int err;

	err = ioctl(fd, TGL_IOC_UNLOCK_LOCK, key);
	if (err) {
		TBM_SPRD_LOG("[libtbm-sprd:%d] "
			     "error(%s) %s:%d key:%d\n",
			     getpid(), strerror(errno), __func__, __LINE__, key);
		return 0;
	}

	return 1;
}

static inline int
_tgl_set_data(int fd, unsigned int key, unsigned int val)
{
	int err;
	struct tgl_user_data arg;

	arg.key = key;
	arg.data1 = val;
	err = ioctl(fd, TGL_IOC_SET_DATA, &arg);
	if (err) {
		TBM_SPRD_LOG("[libtbm-sprd:%d] "
			     "error(%s) %s:%d key:%d\n",
			     getpid(), strerror(errno), __func__, __LINE__, key);
		return 0;
	}

	return 1;
}

static inline unsigned int
_tgl_get_data(int fd, unsigned int key, unsigned int *locked)
{
	int err;
	struct tgl_user_data arg = { 0, };

	arg.key = key;
	err = ioctl(fd, TGL_IOC_GET_DATA, &arg);
	if (err) {
		TBM_SPRD_LOG("[libtbm-sprd:%d] "
			     "error(%s) %s:%d key:%d\n",
			     getpid(), strerror(errno), __func__, __LINE__, key);
		return 0;
	}

	if (locked)
		*locked = arg.locked;

	return arg.data1;
}

static int
_tbm_sprd_open_drm()
{
	int fd = -1;

	fd = drmOpen(SPRD_DRM_NAME, NULL);
	if (fd < 0) {
		TBM_SPRD_LOG ("[libtbm-sprd:%d] "
			      "warning %s:%d fail to open drm\n",
			      getpid(), __FUNCTION__, __LINE__);
	}

#ifdef HAVE_UDEV
	if (fd < 0) {
		struct udev *udev = NULL;
		struct udev_enumerate *e = NULL;
		struct udev_list_entry *entry = NULL;
		struct udev_device *device = NULL, *drm_device = NULL, *device_parent = NULL;
		const char *filepath;
		struct stat s;
		int fd = -1;
		int ret;

		TBM_SPRD_LOG ("[libtbm-sprd:%d] "
			      "%s:%d search drm-device by udev\n",
			      getpid(), __FUNCTION__, __LINE__);

		udev = udev_new();
		if (!udev) {
			TBM_SPRD_LOG("udev_new() failed.\n");
			return -1;
		}

		e = udev_enumerate_new(udev);
		udev_enumerate_add_match_subsystem(e, "drm");
		udev_enumerate_add_match_sysname(e, "card[0-9]*");
		udev_enumerate_scan_devices(e);

		udev_list_entry_foreach(entry, udev_enumerate_get_list_entry(e)) {
			device = udev_device_new_from_syspath(udev_enumerate_get_udev(e),
							      udev_list_entry_get_name(entry));
			device_parent = udev_device_get_parent(device);
			/* Not need unref device_parent. device_parent and device have same refcnt */
			if (device_parent) {
				if (strcmp(udev_device_get_sysname(device_parent), "sprd-drm") == 0) {
					drm_device = device;
					DBG("[%s] Found render device: '%s' (%s)\n",
					    target_name(),
					    udev_device_get_syspath(drm_device),
					    udev_device_get_sysname(device_parent));
					break;
				}
			}
			udev_device_unref(device);
		}

		udev_enumerate_unref(e);

		/* Get device file path. */
		filepath = udev_device_get_devnode(drm_device);
		if (!filepath) {
			TBM_SPRD_LOG("udev_device_get_devnode() failed.\n");
			udev_device_unref(drm_device);
			udev_unref(udev);
			return -1;
		}

		/* Open DRM device file and check validity. */
		fd = open(filepath, O_RDWR | O_CLOEXEC);
		if (fd < 0) {
			TBM_SPRD_LOG("open(%s, O_RDWR | O_CLOEXEC) failed.\n");
			udev_device_unref(drm_device);
			udev_unref(udev);
			return -1;
		}

		ret = fstat(fd, &s);
		if (ret) {
			TBM_SPRD_LOG("fstat() failed %s.\n");
			udev_device_unref(drm_device);
			udev_unref(udev);
			return -1;
		}

		udev_device_unref(drm_device);
		udev_unref(udev);
	}
#endif

	return fd;
}

static int
_sprd_bo_cache_flush (tbm_bo bo, int flags)
{
	tbm_bufmgr_sprd bufmgr_sprd = (tbm_bufmgr_sprd)tbm_backend_get_bufmgr_priv(bo);
	SPRD_RETURN_VAL_IF_FAIL (bufmgr_sprd != NULL, 0);

	/* cache flush is managed by kernel side when using dma-fence. */
	if (bufmgr_sprd->use_dma_fence)
		return 1;

	SPRD_RETURN_VAL_IF_FAIL (bo != NULL, 0);

	tbm_bo_sprd bo_sprd;

	bo_sprd = (tbm_bo_sprd)tbm_backend_get_bo_priv(bo);
	SPRD_RETURN_VAL_IF_FAIL (bo_sprd != NULL, 0);

#ifdef USE_CACHE
	struct drm_sprd_gem_cache_op cache_op = {0, };
	int ret;

	/* if bo_sprd is null, do cache_flush_all */
	if (bo_sprd) {
		cache_op.flags = 0;
		cache_op.usr_addr = (uint64_t)((uint32_t)bo_sprd->pBase);
		cache_op.size = bo_sprd->size;
	} else {
		flags = TBM_SPRD_CACHE_FLUSH_ALL;
		cache_op.flags = 0;
		cache_op.usr_addr = 0;
		cache_op.size = 0;
	}

	if (flags & TBM_SPRD_CACHE_INV) {
		if (flags & TBM_SPRD_CACHE_ALL)
			cache_op.flags |= SPRD_DRM_CACHE_INV_ALL;
		else
			cache_op.flags |= SPRD_DRM_CACHE_INV_RANGE;
	}

	if (flags & TBM_SPRD_CACHE_CLN) {
		if (flags & TBM_SPRD_CACHE_ALL)
			cache_op.flags |= SPRD_DRM_CACHE_CLN_ALL;
		else
			cache_op.flags |= SPRD_DRM_CACHE_CLN_RANGE;
	}

	if (flags & TBM_SPRD_CACHE_ALL)
		cache_op.flags |= SPRD_DRM_ALL_CACHES_CORES;

	ret = drmCommandWriteRead (bufmgr_sprd->fd, DRM_SPRD_GEM_CACHE_OP, &cache_op,
				   sizeof(cache_op));
	if (ret) {
		TBM_SPRD_LOG ("[libtbm-sprd:%d] "
			      "error %s:%d fail to flush the cache.\n",
			      getpid(), __FUNCTION__, __LINE__);
		return 0;
	}
#endif

	return 1;
}

static int
_bo_init_cache_state(tbm_bufmgr_sprd bufmgr_sprd, tbm_bo_sprd bo_sprd)
{
	tbm_bo_cache_state cache_state;

	SPRD_RETURN_VAL_IF_FAIL (bo_sprd != NULL, 0);
	SPRD_RETURN_VAL_IF_FAIL (bufmgr_sprd != NULL, 0);

	_tgl_init(bufmgr_sprd->tgl_fd, bo_sprd->name);

	cache_state.data.isDirtied = DEVICE_NONE;
	cache_state.data.isCached = 0;
	cache_state.data.cntFlush = 0;

	_tgl_set_data(bufmgr_sprd->tgl_fd, bo_sprd->name, cache_state.val);

	return 1;
}

static int
_bo_set_cache_state(tbm_bo bo, int device, int opt)
{
	tbm_bo_sprd bo_sprd;
	tbm_bufmgr_sprd bufmgr_sprd;
	char need_flush = 0;
	unsigned short cntFlush = 0;

	bo_sprd = (tbm_bo_sprd)tbm_backend_get_bo_priv(bo);
	SPRD_RETURN_VAL_IF_FAIL (bo_sprd != NULL, 0);

	bufmgr_sprd = (tbm_bufmgr_sprd)tbm_backend_get_bufmgr_priv(bo);
	SPRD_RETURN_VAL_IF_FAIL (bufmgr_sprd != NULL, 0);

	if (bo_sprd->flags_sprd & SPRD_BO_NONCACHABLE)
		return 1;

	/* get cache state of a bo */
	bo_sprd->cache_state.val = _tgl_get_data(bufmgr_sprd->tgl_fd, bo_sprd->name, NULL);

	/* get global cache flush count */
	cntFlush = (unsigned short)_tgl_get_data(bufmgr_sprd->tgl_fd, GLOBAL_KEY, NULL);

	if (opt == TBM_DEVICE_CPU) {
		if (bo_sprd->cache_state.data.isDirtied == DEVICE_CO &&
		    bo_sprd->cache_state.data.isCached)
			need_flush = TBM_SPRD_CACHE_INV;

		bo_sprd->cache_state.data.isCached = 1;
		if (opt & TBM_OPTION_WRITE)
			bo_sprd->cache_state.data.isDirtied = DEVICE_CA;
		else {
			if (bo_sprd->cache_state.data.isDirtied != DEVICE_CA)
				bo_sprd->cache_state.data.isDirtied = DEVICE_NONE;
		}
	} else {
		if (bo_sprd->cache_state.data.isDirtied == DEVICE_CA &&
		    bo_sprd->cache_state.data.isCached &&
		    bo_sprd->cache_state.data.cntFlush == cntFlush)
			need_flush = TBM_SPRD_CACHE_CLN | TBM_SPRD_CACHE_ALL;

		if (opt & TBM_OPTION_WRITE)
			bo_sprd->cache_state.data.isDirtied = DEVICE_CO;
		else {
			if (bo_sprd->cache_state.data.isDirtied != DEVICE_CO)
				bo_sprd->cache_state.data.isDirtied = DEVICE_NONE;
		}
	}

	if (need_flush) {
		if (need_flush & TBM_SPRD_CACHE_ALL)
			_tgl_set_data(bufmgr_sprd->tgl_fd, GLOBAL_KEY, (unsigned int)(++cntFlush));

		/* call cache flush */
		_sprd_bo_cache_flush (bo, need_flush);

		DBG("[libtbm:%d] \tcache(%d,%d)....flush:0x%x, cntFlush(%d)\n",
		    getpid(),
		    bo_sprd->cache_state.data.isCached,
		    bo_sprd->cache_state.data.isDirtied,
		    need_flush,
		    cntFlush);
	}

	return 1;
}

static int
_bo_save_cache_state(tbm_bo bo)
{
	unsigned short cntFlush = 0;
	tbm_bo_sprd bo_sprd;
	tbm_bufmgr_sprd bufmgr_sprd;

	bo_sprd = (tbm_bo_sprd)tbm_backend_get_bo_priv(bo);
	SPRD_RETURN_VAL_IF_FAIL (bo_sprd != NULL, 0);

	bufmgr_sprd = (tbm_bufmgr_sprd)tbm_backend_get_bufmgr_priv(bo);
	SPRD_RETURN_VAL_IF_FAIL (bufmgr_sprd != NULL, 0);

	/* get global cache flush count */
	cntFlush = (unsigned short)_tgl_get_data(bufmgr_sprd->tgl_fd, GLOBAL_KEY, NULL);

	/* save global cache flush count */
	bo_sprd->cache_state.data.cntFlush = cntFlush;
	_tgl_set_data(bufmgr_sprd->tgl_fd, bo_sprd->name, bo_sprd->cache_state.val);

	return 1;
}

static void
_bo_destroy_cache_state(tbm_bo bo)
{
	tbm_bo_sprd bo_sprd;
	tbm_bufmgr_sprd bufmgr_sprd;

	bo_sprd = (tbm_bo_sprd)tbm_backend_get_bo_priv(bo);
	SPRD_RETURN_IF_FAIL (bo_sprd != NULL);

	bufmgr_sprd = (tbm_bufmgr_sprd)tbm_backend_get_bufmgr_priv(bo);
	SPRD_RETURN_IF_FAIL (bufmgr_sprd != NULL);

	_tgl_destroy(bufmgr_sprd->tgl_fd, bo_sprd->name);
}

#ifndef USE_CONTIG_ONLY
static unsigned int
_get_sprd_flag_from_tbm (unsigned int ftbm)
{
	unsigned int flags = 0;

	/*
	 * TBM_BO_DEFAULT  => ION_HEAP_ID_MASK_SYSTEM
	 * TBM_BO_SCANOUT => ION_HEAP_ID_MASK_MM
	 * TBM_BO_VENDOR => ION_HEAP_ID_MASK_OVERLAY
	 * To be updated appropriately once DRM-GEM supports different heap id masks.
	 * */

	if (ftbm & TBM_BO_SCANOUT) {
		flags = SPRD_BO_CONTIG;
	} else {
		flags = SPRD_BO_NONCONTIG | SPRD_BO_DEV_SYSTEM;
	}

	if (ftbm & TBM_BO_WC)
		flags |= SPRD_BO_WC;
	else if (ftbm & TBM_BO_NONCACHABLE)
		flags |= SPRD_BO_NONCACHABLE;

	return flags;
}

static unsigned int
_get_tbm_flag_from_sprd (unsigned int fsprd)
{
	unsigned int flags = 0;

	if (fsprd & SPRD_BO_NONCONTIG)
		flags |= TBM_BO_DEFAULT;
	else
		flags |= TBM_BO_SCANOUT;

	if (fsprd & SPRD_BO_WC)
		flags |= TBM_BO_WC;
	else if (fsprd & SPRD_BO_CACHABLE)
		flags |= TBM_BO_DEFAULT;
	else
		flags |= TBM_BO_NONCACHABLE;

	return flags;
}
#endif

static unsigned int
_get_name (int fd, unsigned int gem)
{
	struct drm_gem_flink arg = {0,};

	arg.handle = gem;
	if (drmIoctl (fd, DRM_IOCTL_GEM_FLINK, &arg)) {
		TBM_SPRD_LOG ("[libtbm-sprd:%d] "
			      "error %s:%d fail to get flink gem=%d\n",
			      getpid(), __FUNCTION__, __LINE__, gem);
		return 0;
	}

	return (unsigned int)arg.name;
}

static tbm_bo_handle
_sprd_bo_handle (tbm_bo_sprd bo_sprd, int device)
{
	tbm_bo_handle bo_handle;
	memset (&bo_handle, 0x0, sizeof (uint64_t));

	switch (device) {
	case TBM_DEVICE_DEFAULT:
	case TBM_DEVICE_2D:
		bo_handle.u32 = (uint32_t)bo_sprd->gem;
		break;
	case TBM_DEVICE_CPU:
		if (!bo_sprd->pBase) {
			struct drm_sprd_gem_mmap arg = {0,};

			arg.handle = bo_sprd->gem;
			arg.size = bo_sprd->size;
			if (drmCommandWriteRead (bo_sprd->fd, DRM_SPRD_GEM_MMAP, &arg, sizeof(arg))) {
				TBM_SPRD_LOG ("[libtbm-sprd:%d] "
					      "error %s:%d Cannot usrptr gem=%d\n",
					      getpid(), __FUNCTION__, __LINE__, bo_sprd->gem);
				return (tbm_bo_handle) NULL;
			}
			bo_sprd->pBase = (void *)((uint32_t)arg.mapped);
		}

		bo_handle.ptr = (void *)bo_sprd->pBase;
		break;
	case TBM_DEVICE_3D:
#ifdef USE_DMAIMPORT
		if (!bo_sprd->dmabuf) {
			struct drm_prime_handle arg = {0, };
			arg.handle = bo_sprd->gem;
			if (drmIoctl (bo_sprd->fd, DRM_IOCTL_PRIME_HANDLE_TO_FD, &arg)) {
				TBM_SPRD_LOG ("[libtbm-sprd:%d] "
					      "error %s:%d Cannot dmabuf=%d\n",
					      getpid(), __FUNCTION__, __LINE__, bo_sprd->gem);
				return (tbm_bo_handle) NULL;
			}
			bo_sprd->dmabuf = arg.fd;
		}

		bo_handle.u32 = (uint32_t)bo_sprd->dmabuf;

#endif
		break;

	case TBM_DEVICE_MM:
#ifdef USE_HEAP_ID
		//TODO : Add ioctl for GSP MAP once available.
		DBG ("[libtbm-sprd:%d] %s In case TBM_DEVICE_MM:  \n", getpid(),
		     __FUNCTION_);
		_

#else
		if (!bo_sprd->dmabuf) {
			struct drm_prime_handle arg = {0, };

			arg.handle = bo_sprd->gem;
			if (drmIoctl (bo_sprd->fd, DRM_IOCTL_PRIME_HANDLE_TO_FD, &arg)) {
				TBM_SPRD_LOG ("[libtbm-sprd:%d] "
					      "error %s:%d Cannot dmabuf=%d\n",
					      getpid(), __FUNCTION__, __LINE__, bo_sprd->gem);
				return (tbm_bo_handle) NULL;
			}
			bo_sprd->dmabuf = arg.fd;
		}

		bo_handle.u32 = (uint32_t)bo_sprd->dmabuf;
#endif
		break;
	default:
		bo_handle.ptr = (void *) NULL;
		break;
	}

	return bo_handle;
}

static int
tbm_sprd_bo_size (tbm_bo bo)
{
	SPRD_RETURN_VAL_IF_FAIL (bo != NULL, 0);

	tbm_bo_sprd bo_sprd;

	bo_sprd = (tbm_bo_sprd)tbm_backend_get_bo_priv(bo);

	return bo_sprd->size;
}

static void *
tbm_sprd_bo_alloc (tbm_bo bo, int size, int flags)
{
	SPRD_RETURN_VAL_IF_FAIL (bo != NULL, 0);

	tbm_bo_sprd bo_sprd;
	tbm_bufmgr_sprd bufmgr_sprd;
	unsigned int sprd_flags;

	bufmgr_sprd = (tbm_bufmgr_sprd)tbm_backend_get_bufmgr_priv(bo);
	SPRD_RETURN_VAL_IF_FAIL (bufmgr_sprd != NULL, 0);

	bo_sprd = calloc (1, sizeof(struct _tbm_bo_sprd));
	if (!bo_sprd) {
		TBM_SPRD_LOG ("[libtbm-sprd:%d] "
			      "error %s:%d fail to allocate the bo private\n",
			      getpid(), __FUNCTION__, __LINE__);
		return 0;
	}

#ifdef USE_CONTIG_ONLY
	flags = TBM_BO_SCANOUT;
	sprd_flags = SPRD_BO_CONTIG;
#else
	sprd_flags = _get_sprd_flag_from_tbm (flags);
	if ((flags & TBM_BO_SCANOUT) &&
	    size <= 4 * 1024) {
		sprd_flags |= SPRD_BO_NONCONTIG;
	}
#endif // USE_CONTIG_ONLY
	struct drm_sprd_gem_create arg = {0, };
	arg.size = size;
	arg.flags = sprd_flags;
	if (drmCommandWriteRead(bufmgr_sprd->fd, DRM_SPRD_GEM_CREATE, &arg,
				sizeof(arg))) {
		TBM_SPRD_LOG ("[libtbm-sprd:%d] "
			      "error %s:%d Cannot create bo(flag:%x, size:%d)\n",
			      getpid(), __FUNCTION__, __LINE__, arg.flags, (unsigned int)arg.size);
		free (bo_sprd);
		return 0;
	}

	bo_sprd->fd = bufmgr_sprd->fd;
	bo_sprd->gem = arg.handle;
	bo_sprd->size = size;
	bo_sprd->flags_tbm = flags;
	bo_sprd->flags_sprd = sprd_flags;
	bo_sprd->name = _get_name (bo_sprd->fd, bo_sprd->gem);

	if (!_bo_init_cache_state(bufmgr_sprd, bo_sprd)) {
		TBM_SPRD_LOG ("error fail init cache state(%d)\n", bo_sprd->name);
		free (bo_sprd);
		return 0;
	}

	pthread_mutex_init(&bo_sprd->mutex, NULL);

	if (bufmgr_sprd->use_dma_fence
	    && !bo_sprd->dmabuf) {
		struct drm_prime_handle arg = {0, };

		arg.handle = bo_sprd->gem;
		if (drmIoctl (bo_sprd->fd, DRM_IOCTL_PRIME_HANDLE_TO_FD, &arg)) {
			TBM_SPRD_LOG ("[libtbm-sprd:%d] "
				      "error %s:%d Cannot dmabuf=%d\n",
				      getpid(), __FUNCTION__, __LINE__, bo_sprd->gem);
			free (bo_sprd);
			return 0;
		}
		bo_sprd->dmabuf = arg.fd;
	}

	/* add bo to hash */
	PrivGem *privGem = calloc (1, sizeof(PrivGem));
	privGem->ref_count = 1;
	privGem->bo_priv = bo_sprd;
	if (drmHashInsert(bufmgr_sprd->hashBos, bo_sprd->name, (void *)privGem) < 0) {
		TBM_SPRD_LOG ("[libtbm-sprd:%d] "
			      "error %s:%d Cannot insert bo to Hash(%d)\n",
			      getpid(), __FUNCTION__, __LINE__, bo_sprd->name);
	}

	DBG ("[libtbm-sprd:%d] %s size:%d, gem:%d(%d), flags:%d(%d)\n", getpid(),
	     __FUNCTION__, bo_sprd->size,
	     bo_sprd->gem, bo_sprd->name,
	     flags, sprd_flags);

	return (void *)bo_sprd;
}

static void
tbm_sprd_bo_free(tbm_bo bo)
{
	tbm_bo_sprd bo_sprd;
	tbm_bufmgr_sprd bufmgr_sprd;

	if (!bo)
		return;

	bufmgr_sprd = (tbm_bufmgr_sprd)tbm_backend_get_bufmgr_priv(bo);
	SPRD_RETURN_IF_FAIL (bufmgr_sprd != NULL);

	bo_sprd = (tbm_bo_sprd)tbm_backend_get_bo_priv(bo);
	SPRD_RETURN_IF_FAIL (bo_sprd != NULL);

	DBG ("[libtbm-sprd:%d] %s size:%d, gem:%d(%d)\n",
	     getpid(), __FUNCTION__, bo_sprd->size, bo_sprd->gem, bo_sprd->name);

	if (bo_sprd->pBase) {
		if (munmap(bo_sprd->pBase, bo_sprd->size) == -1) {
			TBM_SPRD_LOG ("[libtbm-sprd:%d] "
				      "error %s:%d\n",
				      getpid(), __FUNCTION__, __LINE__);
		}
	}

	/* close dmabuf */
	if (bo_sprd->dmabuf) {
		close (bo_sprd->dmabuf);
		bo_sprd->dmabuf = 0;
	}

	/* delete bo from hash */
	PrivGem *privGem = NULL;
	int ret;

	ret = drmHashLookup (bufmgr_sprd->hashBos, bo_sprd->name, (void **)&privGem);
	if (ret == 0) {
		privGem->ref_count--;
		if (privGem->ref_count == 0) {
			drmHashDelete (bufmgr_sprd->hashBos, bo_sprd->name);
			free (privGem);
			privGem = NULL;
		}
	} else {
		TBM_SPRD_LOG ("[libtbm-sprd:%d] "
			      "warning %s:%d Cannot find bo to Hash(%d), ret=%d\n",
			      getpid(), __FUNCTION__, __LINE__, bo_sprd->name, ret);
	}

	_bo_destroy_cache_state(bo);

	/* Free gem handle */
	struct drm_gem_close arg = {0, };
	memset (&arg, 0, sizeof(arg));
	arg.handle = bo_sprd->gem;
	if (drmIoctl (bo_sprd->fd, DRM_IOCTL_GEM_CLOSE, &arg)) {
		TBM_SPRD_LOG ("[libtbm-sprd:%d] "
			      "error %s:%d\n",
			      getpid(), __FUNCTION__, __LINE__);
	}

	free (bo_sprd);
}


static void *
tbm_sprd_bo_import (tbm_bo bo, unsigned int key)
{
	SPRD_RETURN_VAL_IF_FAIL (bo != NULL, 0);

	tbm_bufmgr_sprd bufmgr_sprd;
	tbm_bo_sprd bo_sprd;
	PrivGem *privGem = NULL;
	int ret;

	bufmgr_sprd = (tbm_bufmgr_sprd)tbm_backend_get_bufmgr_priv(bo);
	SPRD_RETURN_VAL_IF_FAIL (bufmgr_sprd != NULL, 0);

	ret = drmHashLookup (bufmgr_sprd->hashBos, key, (void **)&privGem);
	if (ret == 0) {
		privGem->ref_count++;
		return privGem->bo_priv;
	}

	struct drm_gem_open arg = {0, };
	struct drm_sprd_gem_info info = {0, };

	arg.name = key;
	if (drmIoctl(bufmgr_sprd->fd, DRM_IOCTL_GEM_OPEN, &arg)) {
		TBM_SPRD_LOG ("[libtbm-sprd:%d] "
			      "error %s:%d Cannot open gem name=%d\n",
			      getpid(), __FUNCTION__, __LINE__, key);
		return 0;
	}

	info.handle = arg.handle;
	if (drmCommandWriteRead(bufmgr_sprd->fd,
				DRM_SPRD_GEM_GET,
				&info,
				sizeof(struct drm_sprd_gem_info))) {
		TBM_SPRD_LOG ("[libtbm-sprd:%d] "
			      "error %s:%d Cannot get gem info=%d\n",
			      getpid(), __FUNCTION__, __LINE__, key);
		return 0;
	}

	bo_sprd = calloc (1, sizeof(struct _tbm_bo_sprd));
	if (!bo_sprd) {
		TBM_SPRD_LOG ("[libtbm-sprd:%d] "
			      "error %s:%d fail to allocate the bo private\n",
			      getpid(), __FUNCTION__, __LINE__);
		return 0;
	}

	bo_sprd->fd = bufmgr_sprd->fd;
	bo_sprd->gem = arg.handle;
	bo_sprd->size = arg.size;
	bo_sprd->flags_sprd = info.flags;
	bo_sprd->name = key;
#ifdef USE_CONTIG_ONLY
	bo_sprd->flags_sprd = SPRD_BO_CONTIG;
	bo_sprd->flags_tbm |= TBM_BO_SCANOUT;
#else
	bo_sprd->flags_tbm = _get_tbm_flag_from_sprd (bo_sprd->flags_sprd);
#endif

	if (!_tgl_init(bufmgr_sprd->tgl_fd, bo_sprd->name)) {
		TBM_SPRD_LOG ("error fail tgl init(%d)\n", bo_sprd->name);
		free (bo_sprd);
		return 0;
	}

	if (!bo_sprd->dmabuf) {
		struct drm_prime_handle arg = {0, };

		arg.handle = bo_sprd->gem;
		if (drmIoctl (bo_sprd->fd, DRM_IOCTL_PRIME_HANDLE_TO_FD, &arg)) {
			TBM_SPRD_LOG ("[libtbm-sprd:%d] "
				      "error %s:%d Cannot dmabuf=%d\n",
				      getpid(), __FUNCTION__, __LINE__, bo_sprd->gem);
			free (bo_sprd);
			return 0;
		}
		bo_sprd->dmabuf = arg.fd;
	}

	/* add bo to hash */
	privGem = calloc (1, sizeof(PrivGem));
	privGem->ref_count = 1;
	privGem->bo_priv = bo_sprd;
	if (drmHashInsert (bufmgr_sprd->hashBos, bo_sprd->name, (void *)privGem) < 0) {
		TBM_SPRD_LOG ("[libtbm-sprd:%d] "
			      "error %s:%d Cannot insert bo to Hash(%d)\n",
			      getpid(), __FUNCTION__, __LINE__, bo_sprd->name);
	}

	DBG ("[libtbm-sprd:%d] %s size:%d, gem:%d(%d), flags:%d(%d)\n", getpid(),
	     __FUNCTION__, bo_sprd->size,
	     bo_sprd->gem, bo_sprd->name,
	     bo_sprd->flags_tbm, bo_sprd->flags_sprd);

	return (void *)bo_sprd;
}

static void *
tbm_sprd_bo_import_fd (tbm_bo bo, tbm_fd key)
{
	SPRD_RETURN_VAL_IF_FAIL (bo != NULL, 0);

	tbm_bufmgr_sprd bufmgr_sprd;
	tbm_bo_sprd bo_sprd;
	PrivGem *privGem = NULL;
	int ret;
	int name;

	bufmgr_sprd = (tbm_bufmgr_sprd)tbm_backend_get_bufmgr_priv(bo);
	SPRD_RETURN_VAL_IF_FAIL (bufmgr_sprd != NULL, 0);

	//getting handle from fd
	unsigned int gem = 0;
	struct drm_prime_handle arg = {0, };

	arg.fd = key;
	arg.flags = 0;
	if (drmIoctl (bufmgr_sprd->fd, DRM_IOCTL_PRIME_FD_TO_HANDLE, &arg)) {
		TBM_SPRD_LOG ("error bo:%p Cannot get gem handle from fd:%d (%s)\n",
			      bo, arg.fd, strerror(errno));
		return NULL;
	}
	gem = arg.handle;

	name = _get_name (bufmgr_sprd->fd, gem);

	ret = drmHashLookup (bufmgr_sprd->hashBos, name, (void **)&privGem);
	if (ret == 0) {
		if (gem == privGem->bo_priv->gem) {
			privGem->ref_count++;
			return privGem->bo_priv;
		}
	}

	unsigned int real_size = -1;
	struct drm_sprd_gem_info info = {0, };

	/* Determine size of bo.  The fd-to-handle ioctl really should
	 * return the size, but it doesn't.  If we have kernel 3.12 or
	 * later, we can lseek on the prime fd to get the size.  Older
	 * kernels will just fail, in which case we fall back to the
	 * provided (estimated or guess size). */
	real_size = lseek(key, 0, SEEK_END);

	info.handle = gem;
	if (drmCommandWriteRead(bufmgr_sprd->fd,
				DRM_SPRD_GEM_GET,
				&info,
				sizeof(struct drm_sprd_gem_info))) {
		TBM_SPRD_LOG ("error bo:%p Cannot get gem info from gem:%d, fd:%d (%s)\n",
			      bo, gem, key, strerror(errno));
		return 0;
	}

	if (real_size == -1)
		real_size = info.size;

	bo_sprd = calloc (1, sizeof(struct _tbm_bo_sprd));
	if (!bo_sprd) {
		TBM_SPRD_LOG ("error bo:%p fail to allocate the bo private\n", bo);
		return 0;
	}

	bo_sprd->fd = bufmgr_sprd->fd;
	bo_sprd->gem = gem;
	bo_sprd->size = real_size;
	bo_sprd->flags_sprd = info.flags;
	bo_sprd->flags_tbm = _get_tbm_flag_from_sprd (bo_sprd->flags_sprd);

	bo_sprd->name = name;
	if (!bo_sprd->name) {
		TBM_SPRD_LOG ("error bo:%p Cannot get name from gem:%d, fd:%d (%s)\n",
			      bo, gem, key, strerror(errno));
		free (bo_sprd);
		return 0;
	}

	if (!_tgl_init(bufmgr_sprd->tgl_fd, bo_sprd->name)) {
		TBM_SPRD_LOG ("error fail tgl init(%d)\n", bo_sprd->name);
		free (bo_sprd);
		return 0;
	}

	/* add bo to hash */
	privGem = NULL;

	privGem = calloc (1, sizeof(PrivGem));
	if (!privGem) {
		TBM_SPRD_LOG ("[libtbm-sprd:%d] "
			      "error %s:%d Fail to calloc privGem\n",
			      getpid(), __FUNCTION__, __LINE__);
		free (bo_sprd);
		return 0;
	}

	privGem->ref_count = 1;
	privGem->bo_priv = bo_sprd;
	if (drmHashInsert (bufmgr_sprd->hashBos, bo_sprd->name, (void *)privGem) < 0) {
		TBM_SPRD_LOG ("error bo:%p Cannot insert bo to Hash(%d) from gem:%d, fd:%d\n",
			      bo, bo_sprd->name, gem, key);
	}

	DBG (" [%s] bo:%p, gem:%d(%d), fd:%d, key_fd:%d, flags:%d(%d), size:%d\n",
	     target_name(),
	     bo,
	     bo_sprd->gem, bo_sprd->name,
	     bo_sprd->dmabuf,
	     key,
	     bo_sprd->flags_tbm, bo_sprd->flags_sprd,
	     bo_sprd->size);

	return (void *)bo_sprd;
}

static unsigned int
tbm_sprd_bo_export (tbm_bo bo)
{
	SPRD_RETURN_VAL_IF_FAIL (bo != NULL, 0);

	tbm_bo_sprd bo_sprd;

	bo_sprd = (tbm_bo_sprd)tbm_backend_get_bo_priv(bo);
	SPRD_RETURN_VAL_IF_FAIL (bo_sprd != NULL, 0);

	if (!bo_sprd->name) {
		bo_sprd->name = _get_name(bo_sprd->fd, bo_sprd->gem);
		if (!bo_sprd->name) {
			TBM_SPRD_LOG ("[libtbm-sprd:%d] "
				      "error %s:%d Cannot get name\n",
				      getpid(), __FUNCTION__, __LINE__);
			return 0;
		}
	}

	DBG ("[libtbm-sprd:%d] %s size:%d, gem:%d(%d), flags:%d(%d)\n", getpid(),
	     __FUNCTION__, bo_sprd->size,
	     bo_sprd->gem, bo_sprd->name,
	     bo_sprd->flags_tbm, bo_sprd->flags_sprd);

	return (unsigned int)bo_sprd->name;
}

tbm_fd
tbm_sprd_bo_export_fd (tbm_bo bo)
{
	SPRD_RETURN_VAL_IF_FAIL (bo != NULL, -1);

	tbm_bo_sprd bo_sprd;
	int ret;

	bo_sprd = (tbm_bo_sprd)tbm_backend_get_bo_priv(bo);
	SPRD_RETURN_VAL_IF_FAIL (bo_sprd != NULL, -1);

	struct drm_prime_handle arg = {0, };

	arg.handle = bo_sprd->gem;
	ret = drmIoctl (bo_sprd->fd, DRM_IOCTL_PRIME_HANDLE_TO_FD, &arg);
	if (ret) {
		TBM_SPRD_LOG ("error bo:%p Cannot dmabuf=%d (%s)\n",
			      bo, bo_sprd->gem, strerror(errno));
		return (tbm_fd) ret;
	}

	DBG (" [%s] bo:%p, gem:%d(%d), fd:%d, key_fd:%d, flags:%d(%d), size:%d\n",
	     target_name(),
	     bo,
	     bo_sprd->gem, bo_sprd->name,
	     bo_sprd->dmabuf,
	     arg.fd,
	     bo_sprd->flags_tbm, bo_sprd->flags_sprd,
	     bo_sprd->size);

	return (tbm_fd)arg.fd;
}


static tbm_bo_handle
tbm_sprd_bo_get_handle (tbm_bo bo, int device)
{
	SPRD_RETURN_VAL_IF_FAIL (bo != NULL, (tbm_bo_handle) NULL);

	tbm_bo_handle bo_handle;
	tbm_bo_sprd bo_sprd;

	bo_sprd = (tbm_bo_sprd)tbm_backend_get_bo_priv(bo);
	SPRD_RETURN_VAL_IF_FAIL (bo_sprd != NULL, (tbm_bo_handle) NULL);

	if (!bo_sprd->gem) {
		TBM_SPRD_LOG ("[libtbm-sprd:%d] "
			      "error %s:%d Cannot map gem=%d\n",
			      getpid(), __FUNCTION__, __LINE__, bo_sprd->gem);
		return (tbm_bo_handle) NULL;
	}

	DBG ("[libtbm-sprd:%d] %s gem:%d(%d), %s\n", getpid(),
	     __FUNCTION__, bo_sprd->gem, bo_sprd->name, STR_DEVICE[device]);

	/*Get mapped bo_handle*/
	bo_handle = _sprd_bo_handle (bo_sprd, device);
	if (bo_handle.ptr == NULL) {
		TBM_SPRD_LOG ("[libtbm-sprd:%d] "
			      "error %s:%d Cannot get handle: gem:%d, device:%d\n",
			      getpid(), __FUNCTION__, __LINE__, bo_sprd->gem, device);
		return (tbm_bo_handle) NULL;
	}

	return bo_handle;
}

static tbm_bo_handle
tbm_sprd_bo_map (tbm_bo bo, int device, int opt)
{
	SPRD_RETURN_VAL_IF_FAIL (bo != NULL, (tbm_bo_handle) NULL);

	tbm_bo_handle bo_handle;
	tbm_bo_sprd bo_sprd;

	bo_sprd = (tbm_bo_sprd)tbm_backend_get_bo_priv(bo);
	SPRD_RETURN_VAL_IF_FAIL (bo_sprd != NULL, (tbm_bo_handle) NULL);

	if (!bo_sprd->gem) {
		TBM_SPRD_LOG ("[libtbm-sprd:%d] "
			      "error %s:%d Cannot map gem=%d\n",
			      getpid(), __FUNCTION__, __LINE__, bo_sprd->gem);
		return (tbm_bo_handle) NULL;
	}

	DBG ("[libtbm-sprd:%d] %s gem:%d(%d), %s, %s\n", getpid(),
	     __FUNCTION__, bo_sprd->gem, bo_sprd->name, STR_DEVICE[device], STR_OPT[opt]);

	/*Get mapped bo_handle*/
	bo_handle = _sprd_bo_handle (bo_sprd, device);
	if (bo_handle.ptr == NULL) {
		TBM_SPRD_LOG ("[libtbm-sprd:%d] "
			      "error %s:%d Cannot get handle: gem:%d, device:%d, opt:%d\n",
			      getpid(), __FUNCTION__, __LINE__, bo_sprd->gem, device, opt);
		return (tbm_bo_handle) NULL;
	}

	if (bo_sprd->map_cnt == 0)
		_bo_set_cache_state (bo, device, opt);

	bo_sprd->map_cnt++;

	return bo_handle;
}

static int
tbm_sprd_bo_unmap (tbm_bo bo)
{
	SPRD_RETURN_VAL_IF_FAIL (bo != NULL, 0);

	tbm_bo_sprd bo_sprd;

	bo_sprd = (tbm_bo_sprd)tbm_backend_get_bo_priv(bo);
	SPRD_RETURN_VAL_IF_FAIL (bo_sprd != NULL, 0);

	if (!bo_sprd->gem)
		return 0;

	bo_sprd->map_cnt--;

	if (bo_sprd->map_cnt == 0)
		_bo_save_cache_state (bo);

	DBG ("[libtbm-sprd:%d] %s gem:%d(%d) \n", getpid(),
	     __FUNCTION__, bo_sprd->gem, bo_sprd->name);

	return 1;
}

static int
tbm_sprd_bo_lock(tbm_bo bo, int device, int opt)
{
	SPRD_RETURN_VAL_IF_FAIL (bo != NULL, 0);

#if USE_BACKEND_LOCK
	tbm_bufmgr_sprd bufmgr_sprd;
	tbm_bo_sprd bo_sprd;
	int ret = 0;

	bo_sprd = (tbm_bo_sprd)tbm_backend_get_bo_priv(bo);
	SPRD_RETURN_VAL_IF_FAIL (bo_sprd != NULL, 0);

	bufmgr_sprd = (tbm_bufmgr_sprd)tbm_backend_get_bufmgr_priv(bo);
	SPRD_RETURN_VAL_IF_FAIL (bufmgr_sprd != NULL, 0);

	if (bufmgr_sprd->use_dma_fence) {
		struct dma_buf_fence fence;

		memset(&fence, 0, sizeof(struct dma_buf_fence));

		/* Check if the given type is valid or not. */
		if (opt & TBM_OPTION_WRITE) {
			if (device == TBM_DEVICE_CPU)
				fence.type = DMA_BUF_ACCESS_WRITE;
			else if (device == TBM_DEVICE_3D)
				fence.type = DMA_BUF_ACCESS_WRITE | DMA_BUF_ACCESS_DMA;
			else {
				DBG ("[libtbm-sprd:%d] %s GET_FENCE is ignored(device type is not 3D/CPU),\n",
				     getpid(), __FUNCTION__);
				return 0;
			}
		} else if (opt & TBM_OPTION_READ) {
			if (device == TBM_DEVICE_CPU)
				fence.type = DMA_BUF_ACCESS_READ;
			else if (device == TBM_DEVICE_3D)
				fence.type = DMA_BUF_ACCESS_READ | DMA_BUF_ACCESS_DMA;
			else {
				DBG ("[libtbm-sprd:%d] %s GET_FENCE is ignored(device type is not 3D/CPU),\n",
				     getpid(), __FUNCTION__);
				return 0;
			}
		} else {
			TBM_SPRD_LOG ("[libtbm-sprd:%d] error %s:%d Invalid argument\n", getpid(),
				      __FUNCTION__, __LINE__);
			return 0;
		}

		/* Check if the tbm manager supports dma fence or not. */
		if (!bufmgr_sprd->use_dma_fence) {
			TBM_SPRD_LOG ("[libtbm-sprd:%d] "
				      "error %s:%d  Not support DMA FENCE(%s)\n",
				      getpid(), __FUNCTION__, __LINE__, strerror(errno) );
			return 0;

		}

		ret = ioctl(bo_sprd->dmabuf, DMABUF_IOCTL_GET_FENCE, &fence);
		if (ret < 0) {
			TBM_SPRD_LOG ("[libtbm-sprd:%d] "
				      "error %s:%d  Can not set GET FENCE(%s)\n",
				      getpid(), __FUNCTION__, __LINE__, strerror(errno) );
			return 0;
		}

		pthread_mutex_lock(&bo_sprd->mutex);
		int i;
		for (i = 0; i < DMA_FENCE_LIST_MAX; i++) {
			if (bo_sprd->dma_fence[i].ctx == 0) {
				bo_sprd->dma_fence[i].type = fence.type;
				bo_sprd->dma_fence[i].ctx = fence.ctx;
				break;
			}
		}
		if (i == DMA_FENCE_LIST_MAX) {
			//TODO: if dma_fence list is full, it needs realloc. I will fix this. by minseok3.kim
			TBM_SPRD_LOG ("[libtbm-sprd:%d] "
				      "error %s:%d  fence list is full\n",
				      getpid(), __FUNCTION__, __LINE__);
		}
		pthread_mutex_unlock(&bo_sprd->mutex);

		DBG ("[libtbm-sprd:%d] %s DMABUF_IOCTL_GET_FENCE! flink_id=%d dmabuf=%d\n",
		     getpid(),
		     __FUNCTION__, bo_sprd->name, bo_sprd->dmabuf);
	} else {
		ret = _tgl_lock(bufmgr_sprd->tgl_fd, bo_sprd->name);

		DBG ("[libtbm-sprd:%d] lock tgl flink_id:%d\n",
		     getpid(), __FUNCTION__, bo_sprd->name);

		return ret;
	}

#endif
	return 1;
}

static int
tbm_sprd_bo_unlock(tbm_bo bo)
{
	SPRD_RETURN_VAL_IF_FAIL (bo != NULL, 0);

#if USE_BACKEND_LOCK
	tbm_bufmgr_sprd bufmgr_sprd;
	tbm_bo_sprd bo_sprd;
	int ret = 0;

	bo_sprd = (tbm_bo_sprd)tbm_backend_get_bo_priv(bo);
	SPRD_RETURN_VAL_IF_FAIL (bo_sprd != NULL, 0);

	bufmgr_sprd = (tbm_bufmgr_sprd)tbm_backend_get_bufmgr_priv(bo);
	SPRD_RETURN_VAL_IF_FAIL (bufmgr_sprd != NULL, 0);

	if (bufmgr_sprd->use_dma_fence) {
		struct dma_buf_fence fence;

		if (!bo_sprd->dma_fence[0].ctx) {
			DBG ("[libtbm-sprd:%d] %s FENCE not support or ignored,\n", getpid(),
			     __FUNCTION__);
			return 0;
		}

		if (!bo_sprd->dma_fence[0].type) {
			DBG ("[libtbm-sprd:%d] %s device type is not 3D/CPU,\n", getpid(),
			     __FUNCTION__);
			return 0;
		}

		pthread_mutex_lock(&bo_sprd->mutex);
		fence.type = bo_sprd->dma_fence[0].type;
		fence.ctx = bo_sprd->dma_fence[0].ctx;
		int i;
		for (i = 1; i < DMA_FENCE_LIST_MAX; i++) {
			bo_sprd->dma_fence[i - 1].type = bo_sprd->dma_fence[i].type;
			bo_sprd->dma_fence[i - 1].ctx = bo_sprd->dma_fence[i].ctx;
		}
		bo_sprd->dma_fence[DMA_FENCE_LIST_MAX - 1].type = 0;
		bo_sprd->dma_fence[DMA_FENCE_LIST_MAX - 1].ctx = 0;
		pthread_mutex_unlock(&bo_sprd->mutex);

		ret = ioctl(bo_sprd->dmabuf, DMABUF_IOCTL_PUT_FENCE, &fence);
		if (ret < 0) {
			TBM_SPRD_LOG ("[libtbm-sprd:%d] "
				      "error %s:%d  Can not set PUT FENCE(%s)\n",
				      getpid(), __FUNCTION__, __LINE__, strerror(errno) );
			return 0;
		}

		DBG ("[libtbm-sprd:%d] %s DMABUF_IOCTL_PUT_FENCE! flink_id=%d dmabuf=%d\n",
		     getpid(),
		     __FUNCTION__, bo_sprd->name, bo_sprd->dmabuf);
	} else {
		ret = _tgl_unlock(bufmgr_sprd->tgl_fd, bo_sprd->name);

		DBG ("[libtbm-sprd:%d] unlock tgl flink_id:%d\n",
		     getpid(), __FUNCTION__, bo_sprd->name);

		return ret;
	}
#endif
	return 1;
}

static void
tbm_sprd_bufmgr_deinit (void *priv)
{
	SPRD_RETURN_IF_FAIL (priv != NULL);

	tbm_bufmgr_sprd bufmgr_sprd;

	bufmgr_sprd = (tbm_bufmgr_sprd)priv;

	if (bufmgr_sprd->hashBos) {
		unsigned long key;
		void *value;

		while (drmHashFirst(bufmgr_sprd->hashBos, &key, &value) > 0) {
			free (value);
			drmHashDelete (bufmgr_sprd->hashBos, key);
		}

		drmHashDestroy (bufmgr_sprd->hashBos);
		bufmgr_sprd->hashBos = NULL;
	}

	if (bufmgr_sprd->bind_display)
		tbm_drm_helper_wl_auth_server_deinit();

	if (bufmgr_sprd->device_name)
		free(bufmgr_sprd->device_name);

	close (bufmgr_sprd->tgl_fd);
	close (bufmgr_sprd->fd);

	free (bufmgr_sprd);
}

int
tbm_sprd_surface_supported_format(uint32_t **formats, uint32_t *num)
{
	uint32_t *color_formats = NULL;

	color_formats = (uint32_t *)calloc (1,
					    sizeof(uint32_t) * TBM_COLOR_FORMAT_COUNT);

	if ( color_formats == NULL ) {
		return 0;
	}
	memcpy( color_formats, tbm_sprd_color_format_list ,
		sizeof(uint32_t)*TBM_COLOR_FORMAT_COUNT );


	*formats = color_formats;
	*num = TBM_COLOR_FORMAT_COUNT;


	return 1;
}


/**
 * @brief get the plane data of the surface.
 * @param[in] surface : the surface
 * @param[in] width : the width of the surface
 * @param[in] height : the height of the surface
 * @param[in] format : the format of the surface
 * @param[in] plane_idx : the format of the surface
 * @param[out] size : the size of the plane
 * @param[out] offset : the offset of the plane
 * @param[out] pitch : the pitch of the plane
 * @param[out] padding : the padding of the plane
 * @return 1 if this function succeeds, otherwise 0.
 */
int
tbm_sprd_surface_get_plane_data(tbm_surface_h surface, int width, int height,
				tbm_format format, int plane_idx, uint32_t *size, uint32_t *offset,
				uint32_t *pitch, int *bo_idx)
{
	int ret = 1;
	int bpp;
	int _offset = 0;
	int _pitch = 0;
	int _size = 0;
	int _bo_idx = 0;

	switch (format) {
		/* 16 bpp RGB */
	case TBM_FORMAT_XRGB4444:
	case TBM_FORMAT_XBGR4444:
	case TBM_FORMAT_RGBX4444:
	case TBM_FORMAT_BGRX4444:
	case TBM_FORMAT_ARGB4444:
	case TBM_FORMAT_ABGR4444:
	case TBM_FORMAT_RGBA4444:
	case TBM_FORMAT_BGRA4444:
	case TBM_FORMAT_XRGB1555:
	case TBM_FORMAT_XBGR1555:
	case TBM_FORMAT_RGBX5551:
	case TBM_FORMAT_BGRX5551:
	case TBM_FORMAT_ARGB1555:
	case TBM_FORMAT_ABGR1555:
	case TBM_FORMAT_RGBA5551:
	case TBM_FORMAT_BGRA5551:
	case TBM_FORMAT_RGB565:
		bpp = 16;
		_offset = 0;
		_pitch = SIZE_ALIGN((width * bpp) >> 3, TBM_SURFACE_ALIGNMENT_PITCH_RGB);
		_size = SIZE_ALIGN(_pitch * height, TBM_SURFACE_ALIGNMENT_PLANE);
		_bo_idx = 0;
		break;
		/* 24 bpp RGB */
	case TBM_FORMAT_RGB888:
	case TBM_FORMAT_BGR888:
		bpp = 24;
		_offset = 0;
		_pitch = SIZE_ALIGN((width * bpp) >> 3, TBM_SURFACE_ALIGNMENT_PITCH_RGB);
		_size = SIZE_ALIGN(_pitch * height, TBM_SURFACE_ALIGNMENT_PLANE);
		_bo_idx = 0;
		break;
		/* 32 bpp RGB */
	case TBM_FORMAT_XRGB8888:
	case TBM_FORMAT_XBGR8888:
	case TBM_FORMAT_RGBX8888:
	case TBM_FORMAT_BGRX8888:
	case TBM_FORMAT_ARGB8888:
	case TBM_FORMAT_ABGR8888:
	case TBM_FORMAT_RGBA8888:
	case TBM_FORMAT_BGRA8888:
		bpp = 32;
		_offset = 0;
		_pitch = SIZE_ALIGN((width * bpp) >> 3, TBM_SURFACE_ALIGNMENT_PITCH_RGB);
		_size = SIZE_ALIGN(_pitch * height, TBM_SURFACE_ALIGNMENT_PLANE);
		_bo_idx = 0;
		break;

		/* packed YCbCr */
	case TBM_FORMAT_YUYV:
	case TBM_FORMAT_YVYU:
	case TBM_FORMAT_UYVY:
	case TBM_FORMAT_VYUY:
	case TBM_FORMAT_AYUV:
		bpp = 32;
		_offset = 0;
		_pitch = SIZE_ALIGN((width * bpp) >> 3, TBM_SURFACE_ALIGNMENT_PITCH_YUV);
		_size = SIZE_ALIGN(_pitch * height, TBM_SURFACE_ALIGNMENT_PLANE);
		_bo_idx = 0;
		break;

		/*
		* 2 plane YCbCr
		* index 0 = Y plane, [7:0] Y
		* index 1 = Cr:Cb plane, [15:0] Cr:Cb little endian
		* or
		* index 1 = Cb:Cr plane, [15:0] Cb:Cr little endian
		*/
	case TBM_FORMAT_NV12:
	case TBM_FORMAT_NV21:
		bpp = 12;
		if (plane_idx == 0) {
			_offset = 0;
			_pitch = SIZE_ALIGN( width , TBM_SURFACE_ALIGNMENT_PITCH_YUV);
			_size = SIZE_ALIGN(_pitch * height, TBM_SURFACE_ALIGNMENT_PLANE);
			_bo_idx = 0;
		} else if ( plane_idx == 1 ) {
			_offset = width * height;
			_pitch = SIZE_ALIGN( width , TBM_SURFACE_ALIGNMENT_PITCH_YUV / 2);
			_size = SIZE_ALIGN(_pitch * (height / 2), TBM_SURFACE_ALIGNMENT_PLANE);
			_bo_idx = 0;
		}
		break;

	case TBM_FORMAT_NV16:
	case TBM_FORMAT_NV61:
		bpp = 16;
		//if(plane_idx == 0)
		{
			_offset = 0;
			_pitch = SIZE_ALIGN(width, TBM_SURFACE_ALIGNMENT_PITCH_YUV);
			_size = SIZE_ALIGN(_pitch * height, TBM_SURFACE_ALIGNMENT_PLANE);
			_bo_idx = 0;
			if (plane_idx == 0)
				break;
		}
		//else if( plane_idx ==1 )
		{
			_offset += _size;
			_pitch = SIZE_ALIGN(width, TBM_SURFACE_ALIGNMENT_PITCH_YUV / 2);
			_size = SIZE_ALIGN(_pitch * height, TBM_SURFACE_ALIGNMENT_PLANE);
			_bo_idx = 0;
		}
		break;

		/*
		* 3 plane YCbCr
		* index 0: Y plane, [7:0] Y
		* index 1: Cb plane, [7:0] Cb
		* index 2: Cr plane, [7:0] Cr
		* or
		* index 1: Cr plane, [7:0] Cr
		* index 2: Cb plane, [7:0] Cb
		*/
		/*
		NATIVE_BUFFER_FORMAT_YV12
		NATIVE_BUFFER_FORMAT_I420
		*/
	case TBM_FORMAT_YUV410:
	case TBM_FORMAT_YVU410:
		bpp = 9;
		break;
	case TBM_FORMAT_YUV411:
	case TBM_FORMAT_YVU411:
	case TBM_FORMAT_YUV420:
	case TBM_FORMAT_YVU420:
		bpp = 12;
		//if(plane_idx == 0)
		{
			_offset = 0;
			_pitch = SIZE_ALIGN(width, TBM_SURFACE_ALIGNMENT_PITCH_YUV);
			_size = SIZE_ALIGN(_pitch * height, TBM_SURFACE_ALIGNMENT_PLANE);
			_bo_idx = 0;
			if (plane_idx == 0)
				break;
		}
		//else if( plane_idx == 1 )
		{
			_offset += _size;
			_pitch = SIZE_ALIGN(width / 2, TBM_SURFACE_ALIGNMENT_PITCH_YUV / 2);
			_size = SIZE_ALIGN(_pitch * (height / 2), TBM_SURFACE_ALIGNMENT_PLANE);
			_bo_idx = 0;
			if (plane_idx == 1)
				break;
		}
		//else if (plane_idx == 2 )
		{
			_offset += _size;
			_pitch = SIZE_ALIGN(width / 2, TBM_SURFACE_ALIGNMENT_PITCH_YUV / 2);
			_size = SIZE_ALIGN(_pitch * (height / 2), TBM_SURFACE_ALIGNMENT_PLANE);
			_bo_idx = 0;
		}
		break;
	case TBM_FORMAT_YUV422:
	case TBM_FORMAT_YVU422:
		bpp = 16;
		//if(plane_idx == 0)
		{
			_offset = 0;
			_pitch = SIZE_ALIGN(width, TBM_SURFACE_ALIGNMENT_PITCH_YUV);
			_size = SIZE_ALIGN(_pitch * height, TBM_SURFACE_ALIGNMENT_PLANE);
			_bo_idx = 0;
			if (plane_idx == 0)
				break;
		}
		//else if( plane_idx == 1 )
		{
			_offset += _size;
			_pitch = SIZE_ALIGN(width / 2, TBM_SURFACE_ALIGNMENT_PITCH_YUV / 2);
			_size = SIZE_ALIGN(_pitch * (height), TBM_SURFACE_ALIGNMENT_PLANE);
			_bo_idx = 0;
			if (plane_idx == 1)
				break;
		}
		//else if (plane_idx == 2 )
		{
			_offset += _size;
			_pitch = SIZE_ALIGN(width / 2, TBM_SURFACE_ALIGNMENT_PITCH_YUV / 2);
			_size = SIZE_ALIGN(_pitch * (height), TBM_SURFACE_ALIGNMENT_PLANE);
			_bo_idx = 0;
		}
		break;
	case TBM_FORMAT_YUV444:
	case TBM_FORMAT_YVU444:
		bpp = 24;
		//if(plane_idx == 0)
		{
			_offset = 0;
			_pitch = SIZE_ALIGN(width, TBM_SURFACE_ALIGNMENT_PITCH_YUV);
			_size = SIZE_ALIGN(_pitch * height, TBM_SURFACE_ALIGNMENT_PLANE);
			_bo_idx = 0;
			if (plane_idx == 0)
				break;
		}
		//else if( plane_idx == 1 )
		{
			_offset += _size;
			_pitch = SIZE_ALIGN(width, TBM_SURFACE_ALIGNMENT_PITCH_YUV);
			_size = SIZE_ALIGN(_pitch * height, TBM_SURFACE_ALIGNMENT_PLANE);
			_bo_idx = 0;
			if (plane_idx == 1)
				break;
		}
		//else if (plane_idx == 2 )
		{
			_offset += _size;
			_pitch = SIZE_ALIGN(width, TBM_SURFACE_ALIGNMENT_PITCH_YUV);
			_size = SIZE_ALIGN(_pitch * height, TBM_SURFACE_ALIGNMENT_PLANE);
			_bo_idx = 0;
		}
		break;
	default:
		bpp = 0;
		break;
	}

	*size = _size;
	*offset = _offset;
	*pitch = _pitch;
	*bo_idx = _bo_idx;

	return ret;
}

int
tbm_sprd_bo_get_flags (tbm_bo bo)
{
	SPRD_RETURN_VAL_IF_FAIL (bo != NULL, 0);

	tbm_bo_sprd bo_sprd;

	bo_sprd = (tbm_bo_sprd)tbm_backend_get_bo_priv(bo);
	SPRD_RETURN_VAL_IF_FAIL (bo_sprd != NULL, 0);

	return bo_sprd->flags_tbm;
}

int
tbm_sprd_bufmgr_bind_native_display (tbm_bufmgr bufmgr, void *NativeDisplay)
{
	tbm_bufmgr_sprd bufmgr_sprd;

	bufmgr_sprd = tbm_backend_get_priv_from_bufmgr(bufmgr);
	SPRD_RETURN_VAL_IF_FAIL(bufmgr_sprd != NULL, 0);

	if (!tbm_drm_helper_wl_auth_server_init(NativeDisplay, bufmgr_sprd->fd,
					   bufmgr_sprd->device_name, 0)) {
		TBM_SPRD_LOG("[libtbm-sprd:%d] error:Fail to tbm_drm_helper_wl_server_init\n");
		return 0;
	}

	bufmgr_sprd->bind_display = NativeDisplay;

	return 1;
}

MODULEINITPPROTO (init_tbm_bufmgr_priv);

static TBMModuleVersionInfo SprdVersRec = {
	"sprd",
	"Samsung",
	TBM_ABI_VERSION,
};

TBMModuleData tbmModuleData = { &SprdVersRec, init_tbm_bufmgr_priv};

int
init_tbm_bufmgr_priv (tbm_bufmgr bufmgr, int fd)
{
	tbm_bufmgr_sprd bufmgr_sprd;
	tbm_bufmgr_backend bufmgr_backend;

	if (!bufmgr)
		return 0;

	bufmgr_sprd = calloc (1, sizeof(struct _tbm_bufmgr_sprd));
	if (!bufmgr_sprd) {
		TBM_SPRD_LOG ("[libtbm-sprd:%d] error: Fail to alloc bufmgr_sprd!\n", getpid());
		return 0;
	}

	if (tbm_backend_is_display_server()) {
		int master_fd = -1;

		bufmgr_sprd->fd = -1;
		master_fd = tbm_drm_helper_get_master_fd();
		if (master_fd < 0) {
			bufmgr_sprd->fd = _tbm_sprd_open_drm();
			tbm_drm_helper_set_master_fd(bufmgr_sprd->fd);
		} else {
			bufmgr_sprd->fd = dup(master_fd);
		}

		if (bufmgr_sprd->fd < 0) {
			TBM_SPRD_LOG ("[libtbm-sprd:%d] error: Fail to create drm!\n", getpid());
			free (bufmgr_sprd);
			return 0;
		}

		bufmgr_sprd->device_name = drmGetDeviceNameFromFd(bufmgr_sprd->fd);

		if (!bufmgr_sprd->device_name)
		{
			TBM_SPRD_LOG ("[libtbm-sprd:%d] error: Fail to get device name!\n", getpid());
			free (bufmgr_sprd);
			return 0;
		}

	} else {
		if (!tbm_drm_helper_get_auth_info(&(bufmgr_sprd->fd), &(bufmgr_sprd->device_name), NULL)) {
			TBM_SPRD_LOG ("[libtbm-sprd:%d] error: Fail to get auth drm info!\n", getpid());
			free (bufmgr_sprd);
			return 0;
		}
	}

	/* open tgl fd for saving cache flush data */
	bufmgr_sprd->tgl_fd = open(tgl_devfile, O_RDWR);

	if (bufmgr_sprd->tgl_fd < 0) {
		bufmgr_sprd->tgl_fd = open(tgl_devfile1, O_RDWR);
		if (bufmgr_sprd->tgl_fd < 0) {
			TBM_SPRD_LOG("[libtbm:%d] "
				     "error: Fail to open global_lock:%s\n",
				     getpid(), tgl_devfile);

			close(bufmgr_sprd->fd);
			free (bufmgr_sprd);
			return 0;
		}
	}

	if (!_tgl_init(bufmgr_sprd->tgl_fd, GLOBAL_KEY)) {
		TBM_SPRD_LOG("[libtbm:%d] "
			     "error: Fail to initialize the tgl\n",
			     getpid());

		close(bufmgr_sprd->fd);
		close(bufmgr_sprd->tgl_fd);

		free (bufmgr_sprd);
		return 0;
	}

	//Create Hash Table
	bufmgr_sprd->hashBos = drmHashCreate ();

	//Check if the tbm manager supports dma fence or not.
	int fp = open("/sys/module/dmabuf_sync/parameters/enabled", O_RDONLY);
	int length;
	char buf[1];
	if (fp != -1) {
		length = read(fp, buf, 1);

		if (length == 1 && buf[0] == '1')
			bufmgr_sprd->use_dma_fence = 1;

		close(fp);
	}

	bufmgr_backend = tbm_backend_alloc();
	if (!bufmgr_backend) {
		TBM_SPRD_LOG ("[libtbm-sprd:%d] error: Fail to create drm!\n", getpid());

		close(bufmgr_sprd->fd);
		close(bufmgr_sprd->tgl_fd);

		free (bufmgr_sprd);
		return 0;
	}

	bufmgr_backend->priv = (void *)bufmgr_sprd;
	bufmgr_backend->bufmgr_deinit = tbm_sprd_bufmgr_deinit;
	bufmgr_backend->bo_size = tbm_sprd_bo_size;
	bufmgr_backend->bo_alloc = tbm_sprd_bo_alloc;
	bufmgr_backend->bo_free = tbm_sprd_bo_free;
	bufmgr_backend->bo_import = tbm_sprd_bo_import;
	bufmgr_backend->bo_import_fd = tbm_sprd_bo_import_fd;
	bufmgr_backend->bo_export = tbm_sprd_bo_export;
	bufmgr_backend->bo_export_fd = tbm_sprd_bo_export_fd;
	bufmgr_backend->bo_get_handle = tbm_sprd_bo_get_handle;
	bufmgr_backend->bo_map = tbm_sprd_bo_map;
	bufmgr_backend->bo_unmap = tbm_sprd_bo_unmap;
	bufmgr_backend->surface_get_plane_data = tbm_sprd_surface_get_plane_data;
	bufmgr_backend->surface_supported_format = tbm_sprd_surface_supported_format;
	bufmgr_backend->bo_get_flags = tbm_sprd_bo_get_flags;
	bufmgr_backend->bo_lock = NULL;
	bufmgr_backend->bo_lock2 = tbm_sprd_bo_lock;
	bufmgr_backend->bo_unlock = tbm_sprd_bo_unlock;
	bufmgr_backend->bufmgr_bind_native_display = tbm_sprd_bufmgr_bind_native_display;

	bufmgr_backend->flags |= TBM_USE_2_0_BACKEND;

	if (!tbm_backend_init (bufmgr, bufmgr_backend)) {
		TBM_SPRD_LOG ("[libtbm-sprd:%d] error: Fail to init backend!\n", getpid());
		tbm_backend_free (bufmgr_backend);

		close(bufmgr_sprd->tgl_fd);
		close(bufmgr_sprd->fd);

		free (bufmgr_sprd);
		return 0;
	}

#ifdef DEBUG
	{
		char *env;
		env = getenv ("TBM_SPRD_DEBUG");
		if (env) {
			bDebug = atoi (env);
			TBM_SPRD_LOG ("TBM_SPRD_DEBUG=%s\n", env);
		} else {
			bDebug = 0;
		}
	}
#endif

	DBG ("[libtbm-sprd:%d] %s DMABUF FENCE is %s\n", getpid(),
	     __FUNCTION__, bufmgr_sprd->use_dma_fence ? "supported!" : "NOT supported!");

	DBG ("[libtbm-sprd:%d] %s fd:%d\n", getpid(),
	     __FUNCTION__, bufmgr_sprd->fd);

	return 1;
}


