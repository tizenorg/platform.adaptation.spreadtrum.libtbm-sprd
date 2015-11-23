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

#define DEBUG

//#define USE_CONTIG_ONLY
#define USE_DMAIMPORT

#define TBM_COLOR_FORMAT_COUNT 8



#ifdef DEBUG
#define LOG_TAG	"TBM_BACKEND"
#include <dlog.h>
static int bDebug=0;

char* target_name()
{
    FILE *f;
    char *slash;
    static int 	initialized = 0;
    static char app_name[128];

    if ( initialized )
        return app_name;

    /* get the application name */
    f = fopen("/proc/self/cmdline", "r");

    if ( !f )
    {
        return 0;
    }

    memset(app_name, 0x00, sizeof(app_name));

    if ( fgets(app_name, 100, f) == NULL )
    {
        fclose(f);
        return 0;
    }

    fclose(f);

    if ( (slash=strrchr(app_name, '/')) != NULL )
    {
        memmove(app_name, slash+1, strlen(slash));
    }

    initialized = 1;

    return app_name;
}
#define TBM_SPRD_LOG(fmt, args...)		LOGE("\033[31m"  "[%s]" fmt "\033[0m", target_name(), ##args)
#define DBG(fmt, args...)		if(bDebug&01) LOGE("[%s]" fmt, target_name(), ##args)
#else
#define TBM_SPRD_LOG(...)
#define DBG(...)
#endif

#define SIZE_ALIGN( value, base ) (((value) + ((base) - 1)) & ~((base) - 1))

#define TBM_SURFACE_ALIGNMENT_PLANE (64)
#define TBM_SURFACE_ALIGNMENT_PITCH_RGB (64)
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
	unsigned long	size;
	unsigned int	fence_supported;
	unsigned int	padding;
};

#define DMA_BUF_ACCESS_READ		0x1
#define DMA_BUF_ACCESS_WRITE		0x2
#define DMA_BUF_ACCESS_DMA		0x4
#define DMA_BUF_ACCESS_MAX		0x8

#define DMA_FENCE_LIST_MAX 		5

struct dma_buf_fence {
	unsigned long		ctx;
	unsigned int		type;
};

#define DMABUF_IOCTL_BASE	'F'
#define DMABUF_IOWR(nr, type)	_IOWR(DMABUF_IOCTL_BASE, nr, type)

#define DMABUF_IOCTL_GET_INFO	DMABUF_IOWR(0x00, struct dma_buf_info)
#define DMABUF_IOCTL_GET_FENCE	DMABUF_IOWR(0x01, struct dma_buf_fence)
#define DMABUF_IOCTL_PUT_FENCE	DMABUF_IOWR(0x02, struct dma_buf_fence)

typedef struct _tbm_bufmgr_sprd *tbm_bufmgr_sprd;
typedef struct _tbm_bo_sprd *tbm_bo_sprd;

typedef struct _sprd_private
{
    int ref_count;
} PrivGem;

/* tbm buffor object for sprd */
struct _tbm_bo_sprd
{
    int fd;

    unsigned int name;    /* FLINK ID */

    unsigned int gem;     /* GEM Handle */

    unsigned int dmabuf;  /* fd for dmabuf */

    void *pBase;          /* virtual address */

    unsigned int size;

    unsigned int flags_sprd;
    unsigned int flags_tbm;

    PrivGem* private;

    pthread_mutex_t mutex;
    struct dma_buf_fence dma_fence[DMA_FENCE_LIST_MAX];
    int device;
    int opt;
};

/* tbm bufmgr private for sprd */
struct _tbm_bufmgr_sprd
{
    int fd;
    void* hashBos;

    int use_dma_fence;
};

char *STR_DEVICE[]=
{
    "DEF",
    "CPU",
    "2D",
    "3D",
    "MM"
};

char *STR_OPT[]=
{
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
                                                                    TBM_FORMAT_YVU420 };


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

    if (ftbm & TBM_BO_SCANOUT)
    {
      flags = SPRD_BO_CONTIG;
    }
    else
    {
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
    if (drmIoctl (fd, DRM_IOCTL_GEM_FLINK, &arg))
    {
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

    switch(device)
    {
    case TBM_DEVICE_DEFAULT:
    case TBM_DEVICE_2D:
        bo_handle.u32 = (uint32_t)bo_sprd->gem;
        break;
    case TBM_DEVICE_CPU:
        if (!bo_sprd->pBase)
        {
            struct drm_sprd_gem_mmap arg = {0,};

            arg.handle = bo_sprd->gem;
            arg.size = bo_sprd->size;
            if (drmCommandWriteRead (bo_sprd->fd, DRM_SPRD_GEM_MMAP, &arg, sizeof(arg)))
            {
                TBM_SPRD_LOG ("[libtbm-sprd:%d] "
                         "error %s:%d Cannot usrptr gem=%d\n",
                         getpid(), __FUNCTION__, __LINE__, bo_sprd->gem);
                return (tbm_bo_handle) NULL;
            }
            bo_sprd->pBase = (void*)((uint32_t)arg.mapped);
        }

        bo_handle.ptr = (void *)bo_sprd->pBase;
        break;
    case TBM_DEVICE_3D:
#ifdef USE_DMAIMPORT
   if (!bo_sprd->dmabuf)
        {
            struct drm_prime_handle arg = {0, };
            arg.handle = bo_sprd->gem;
            if (drmIoctl (bo_sprd->fd, DRM_IOCTL_PRIME_HANDLE_TO_FD, &arg))
            {
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
         __FUNCTION_);_

#else
        if (!bo_sprd->dmabuf)
        {
            struct drm_prime_handle arg = {0, };

            arg.handle = bo_sprd->gem;
            if (drmIoctl (bo_sprd->fd, DRM_IOCTL_PRIME_HANDLE_TO_FD, &arg))
            {
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

#ifdef USE_CACHE
static int
_sprd_cache_flush (int fd, tbm_bo_sprd bo_sprd, int flags)
{
    struct drm_sprd_gem_cache_op cache_op = {0, };
    int ret;

    /* if bo_sprd is null, do cache_flush_all */
    if(bo_sprd)
    {
        cache_op.flags = 0;
        cache_op.usr_addr = (uint64_t)((uint32_t)bo_sprd->pBase);
        cache_op.size = bo_sprd->size;
    }
    else
    {
        flags = TBM_CACHE_FLUSH_ALL;
        cache_op.flags = 0;
        cache_op.usr_addr = 0;
        cache_op.size = 0;
    }

    if (flags & TBM_CACHE_INV)
    {
        if(flags & TBM_CACHE_ALL)
            cache_op.flags |= SPRD_DRM_CACHE_INV_ALL;
        else
            cache_op.flags |= SPRD_DRM_CACHE_INV_RANGE;
    }

    if (flags & TBM_CACHE_CLN)
    {
        if(flags & TBM_CACHE_ALL)
            cache_op.flags |= SPRD_DRM_CACHE_CLN_ALL;
        else
            cache_op.flags |= SPRD_DRM_CACHE_CLN_RANGE;
    }

    if(flags & TBM_CACHE_ALL)
        cache_op.flags |= SPRD_DRM_ALL_CACHES_CORES;

    ret = drmCommandWriteRead (fd, DRM_SPRD_GEM_CACHE_OP, &cache_op, sizeof(cache_op));
    if (ret)
    {
        TBM_SPRD_LOG ("[libtbm-sprd:%d] "
                 "error %s:%d fail to flush the cache.\n",
                 getpid(), __FUNCTION__, __LINE__);
        return 0;
    }

    return 1;
}
#endif

static int
tbm_sprd_bo_size (tbm_bo bo)
{
    SPRD_RETURN_VAL_IF_FAIL (bo!=NULL, 0);

    tbm_bo_sprd bo_sprd;

    bo_sprd = (tbm_bo_sprd)tbm_backend_get_bo_priv(bo);

    return bo_sprd->size;
}

static void *
tbm_sprd_bo_alloc (tbm_bo bo, int size, int flags)
{
    SPRD_RETURN_VAL_IF_FAIL (bo!=NULL, 0);

    tbm_bo_sprd bo_sprd;
    tbm_bufmgr_sprd bufmgr_sprd;
    unsigned int sprd_flags;

    bufmgr_sprd = (tbm_bufmgr_sprd)tbm_backend_get_bufmgr_priv(bo);
    SPRD_RETURN_VAL_IF_FAIL (bufmgr_sprd!=NULL, 0);

    bo_sprd = calloc (1, sizeof(struct _tbm_bo_sprd));
    if (!bo_sprd)
    {
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
    if((flags & TBM_BO_SCANOUT) &&
        size <= 4*1024)
    {
        sprd_flags |= SPRD_BO_NONCONTIG;
    }
#endif // USE_CONTIG_ONLY
    struct drm_sprd_gem_create arg = {0, };
    arg.size = size;
    arg.flags = sprd_flags;
    if (drmCommandWriteRead(bufmgr_sprd->fd, DRM_SPRD_GEM_CREATE, &arg, sizeof(arg)))
    {
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

    pthread_mutex_init(&bo_sprd->mutex, NULL);

    if (bufmgr_sprd->use_dma_fence
        && !bo_sprd->dmabuf)
    {
        struct drm_prime_handle arg = {0, };

        arg.handle = bo_sprd->gem;
        if (drmIoctl (bo_sprd->fd, DRM_IOCTL_PRIME_HANDLE_TO_FD, &arg))
        {
            TBM_SPRD_LOG ("[libtbm-sprd:%d] "
                    "error %s:%d Cannot dmabuf=%d\n",
                    getpid(), __FUNCTION__, __LINE__, bo_sprd->gem);
            free (bo_sprd);
            return 0;
        }
        bo_sprd->dmabuf = arg.fd;
    }

    /* add bo to hash */
    PrivGem* privGem = calloc (1, sizeof(PrivGem));
    privGem->ref_count = 1;
    if (drmHashInsert(bufmgr_sprd->hashBos, bo_sprd->name, (void *)privGem) < 0)
    {
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
    SPRD_RETURN_IF_FAIL (bufmgr_sprd!=NULL);

    bo_sprd = (tbm_bo_sprd)tbm_backend_get_bo_priv(bo);
    SPRD_RETURN_IF_FAIL (bo_sprd!=NULL);

    DBG ("[libtbm-sprd:%d] %s size:%d, gem:%d(%d)\n",
         getpid(), __FUNCTION__, bo_sprd->size, bo_sprd->gem, bo_sprd->name);

    if (bo_sprd->pBase)
    {
        if (munmap(bo_sprd->pBase, bo_sprd->size) == -1)
        {
            TBM_SPRD_LOG ("[libtbm-sprd:%d] "
                     "error %s:%d\n",
                     getpid(), __FUNCTION__, __LINE__);
        }
    }

    /* close dmabuf */
    if (bo_sprd->dmabuf)
    {
        close (bo_sprd->dmabuf);
        bo_sprd->dmabuf = 0;
    }

    /* delete bo from hash */
    PrivGem *privGem = NULL;
    int ret;

    ret = drmHashLookup (bufmgr_sprd->hashBos, bo_sprd->name, (void**)&privGem);
    if (ret == 0)
    {
        privGem->ref_count--;
        if (privGem->ref_count == 0)
        {
            drmHashDelete (bufmgr_sprd->hashBos, bo_sprd->name);
            free (privGem);
            privGem = NULL;
        }
    }
    else
    {
        TBM_SPRD_LOG ("[libtbm-sprd:%d] "
                 "warning %s:%d Cannot find bo to Hash(%d), ret=%d\n",
                 getpid(), __FUNCTION__, __LINE__, bo_sprd->name, ret);
    }

    /* Free gem handle */
    struct drm_gem_close arg = {0, };
    memset (&arg, 0, sizeof(arg));
    arg.handle = bo_sprd->gem;
    if (drmIoctl (bo_sprd->fd, DRM_IOCTL_GEM_CLOSE, &arg))
    {
        TBM_SPRD_LOG ("[libtbm-sprd:%d] "
                 "error %s:%d\n",
                 getpid(), __FUNCTION__, __LINE__);
    }

    free (bo_sprd);
}


static void *
tbm_sprd_bo_import (tbm_bo bo, unsigned int key)
{
    SPRD_RETURN_VAL_IF_FAIL (bo!=NULL, 0);

    tbm_bufmgr_sprd bufmgr_sprd;
    tbm_bo_sprd bo_sprd;

    bufmgr_sprd = (tbm_bufmgr_sprd)tbm_backend_get_bufmgr_priv(bo);
    SPRD_RETURN_VAL_IF_FAIL (bufmgr_sprd!=NULL, 0);

    struct drm_gem_open arg = {0, };
    struct drm_sprd_gem_info info = {0, };

    arg.name = key;
    if (drmIoctl(bufmgr_sprd->fd, DRM_IOCTL_GEM_OPEN, &arg))
    {
        TBM_SPRD_LOG ("[libtbm-sprd:%d] "
                "error %s:%d Cannot open gem name=%d\n",
                getpid(), __FUNCTION__, __LINE__, key);
        return 0;
    }

    info.handle = arg.handle;
    if (drmCommandWriteRead(bufmgr_sprd->fd,
                           DRM_SPRD_GEM_GET,
                           &info,
                           sizeof(struct drm_sprd_gem_info)))
    {
        TBM_SPRD_LOG ("[libtbm-sprd:%d] "
                "error %s:%d Cannot get gem info=%d\n",
                getpid(), __FUNCTION__, __LINE__, key);
        return 0;
    }

    bo_sprd = calloc (1, sizeof(struct _tbm_bo_sprd));
    if (!bo_sprd)
    {
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


    if (!bo_sprd->dmabuf)
    {
        struct drm_prime_handle arg = {0, };

        arg.handle = bo_sprd->gem;
        if (drmIoctl (bo_sprd->fd, DRM_IOCTL_PRIME_HANDLE_TO_FD, &arg))
        {
            TBM_SPRD_LOG ("[libtbm-sprd:%d] "
                    "error %s:%d Cannot dmabuf=%d\n",
                    getpid(), __FUNCTION__, __LINE__, bo_sprd->gem);
            free (bo_sprd);
            return 0;
        }
        bo_sprd->dmabuf = arg.fd;
    }

    /* add bo to hash */
    PrivGem *privGem = NULL;
    int ret;

    ret = drmHashLookup (bufmgr_sprd->hashBos, bo_sprd->name, (void**)&privGem);
    if (ret == 0)
    {
        privGem->ref_count++;
    }
    else if (ret == 1)
    {
        privGem = calloc (1, sizeof(PrivGem));
        privGem->ref_count = 1;
        if (drmHashInsert (bufmgr_sprd->hashBos, bo_sprd->name, (void *)privGem) < 0)
        {
            TBM_SPRD_LOG ("[libtbm-sprd:%d] "
                    "error %s:%d Cannot insert bo to Hash(%d)\n",
                    getpid(), __FUNCTION__, __LINE__, bo_sprd->name);
        }
    }
    else
    {
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

static unsigned int
tbm_sprd_bo_export (tbm_bo bo)
{
    SPRD_RETURN_VAL_IF_FAIL (bo!=NULL, 0);

    tbm_bo_sprd bo_sprd;

    bo_sprd = (tbm_bo_sprd)tbm_backend_get_bo_priv(bo);
    SPRD_RETURN_VAL_IF_FAIL (bo_sprd!=NULL, 0);

    if (!bo_sprd->name)
    {
        bo_sprd->name = _get_name(bo_sprd->fd, bo_sprd->gem);
        if (!bo_sprd->name)
        {
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

static tbm_bo_handle
tbm_sprd_bo_get_handle (tbm_bo bo, int device)
{
    SPRD_RETURN_VAL_IF_FAIL (bo!=NULL, (tbm_bo_handle) NULL);

    tbm_bo_handle bo_handle;
    tbm_bo_sprd bo_sprd;

    bo_sprd = (tbm_bo_sprd)tbm_backend_get_bo_priv(bo);
    SPRD_RETURN_VAL_IF_FAIL (bo_sprd!=NULL, (tbm_bo_handle) NULL);

    if (!bo_sprd->gem)
    {
        TBM_SPRD_LOG ("[libtbm-sprd:%d] "
                "error %s:%d Cannot map gem=%d\n",
                getpid(), __FUNCTION__, __LINE__, bo_sprd->gem);
        return (tbm_bo_handle) NULL;
    }

    DBG ("[libtbm-sprd:%d] %s gem:%d(%d), %s\n", getpid(),
         __FUNCTION__, bo_sprd->gem, bo_sprd->name, STR_DEVICE[device]);

    /*Get mapped bo_handle*/
    bo_handle = _sprd_bo_handle (bo_sprd, device);
    if (bo_handle.ptr == NULL)
    {
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
    SPRD_RETURN_VAL_IF_FAIL (bo!=NULL, (tbm_bo_handle) NULL);

    tbm_bo_handle bo_handle;
    tbm_bo_sprd bo_sprd;

    bo_sprd = (tbm_bo_sprd)tbm_backend_get_bo_priv(bo);
    SPRD_RETURN_VAL_IF_FAIL (bo_sprd!=NULL, (tbm_bo_handle) NULL);

    if (!bo_sprd->gem)
    {
        TBM_SPRD_LOG ("[libtbm-sprd:%d] "
                "error %s:%d Cannot map gem=%d\n",
                getpid(), __FUNCTION__, __LINE__, bo_sprd->gem);
        return (tbm_bo_handle) NULL;
    }

    DBG ("[libtbm-sprd:%d] %s gem:%d(%d), %s, %s\n", getpid(),
         __FUNCTION__, bo_sprd->gem, bo_sprd->name, STR_DEVICE[device], STR_OPT[opt]);

    /*Get mapped bo_handle*/
    bo_handle = _sprd_bo_handle (bo_sprd, device);
    if (bo_handle.ptr == NULL)
    {
        TBM_SPRD_LOG ("[libtbm-sprd:%d] "
                "error %s:%d Cannot get handle: gem:%d, device:%d, opt:%d\n",
                getpid(), __FUNCTION__, __LINE__, bo_sprd->gem, device, opt);
        return (tbm_bo_handle) NULL;
    }

    return bo_handle;
}

static int
tbm_sprd_bo_unmap (tbm_bo bo)
{
    SPRD_RETURN_VAL_IF_FAIL (bo!=NULL, 0);

    tbm_bo_sprd bo_sprd;

    bo_sprd = (tbm_bo_sprd)tbm_backend_get_bo_priv(bo);
    SPRD_RETURN_VAL_IF_FAIL (bo_sprd!=NULL, 0);

    if (!bo_sprd->gem)
        return 0;

    DBG ("[libtbm-sprd:%d] %s gem:%d(%d) \n", getpid(),
         __FUNCTION__, bo_sprd->gem, bo_sprd->name);

    return 1;
}

static int
tbm_sprd_bo_cache_flush (tbm_bo bo, int flags)
{
    tbm_bufmgr_sprd bufmgr_sprd = (tbm_bufmgr_sprd)tbm_backend_get_bufmgr_priv(bo);
    SPRD_RETURN_VAL_IF_FAIL (bufmgr_sprd!=NULL, 0);

    /* cache flush is managed by kernel side when using dma-fence. */
    if (bufmgr_sprd->use_dma_fence)
       return 1;

    SPRD_RETURN_VAL_IF_FAIL (bo!=NULL, 0);

    tbm_bo_sprd bo_sprd;

    bo_sprd = (tbm_bo_sprd)tbm_backend_get_bo_priv(bo);
    SPRD_RETURN_VAL_IF_FAIL (bo_sprd!=NULL, 0);

#ifdef USE_CACHE
    if (!_sprd_cache_flush(bo_sprd->fd, bo_sprd, flags))
        return 0;
#endif

    return 1;
}

static int
tbm_sprd_bo_get_global_key (tbm_bo bo)
{
    SPRD_RETURN_VAL_IF_FAIL (bo!=NULL, 0);

    tbm_bo_sprd bo_sprd;

    bo_sprd = (tbm_bo_sprd)tbm_backend_get_bo_priv(bo);
    SPRD_RETURN_VAL_IF_FAIL (bo_sprd!=NULL, 0);

    if (!bo_sprd->name)
    {
        if (!bo_sprd->gem)
            return 0;

        bo_sprd->name = _get_name(bo_sprd->fd, bo_sprd->gem);
    }

    return bo_sprd->name;
}

static int
tbm_sprd_bo_lock(tbm_bo bo, int device, int opt)
{
    SPRD_RETURN_VAL_IF_FAIL (bo!=NULL, 0);

#if USE_BACKEND_LOCK
    tbm_bufmgr_sprd bufmgr_sprd;
    tbm_bo_sprd bo_sprd;
    struct dma_buf_fence fence;
    int ret=0;

    bo_sprd = (tbm_bo_sprd)tbm_backend_get_bo_priv(bo);
    SPRD_RETURN_VAL_IF_FAIL (bo_sprd!=NULL, 0);

    bufmgr_sprd = (tbm_bufmgr_sprd)tbm_backend_get_bufmgr_priv(bo);
    SPRD_RETURN_VAL_IF_FAIL (bufmgr_sprd!=NULL, 0);

    memset(&fence, 0, sizeof(struct dma_buf_fence));

    /* Check if the given type is valid or not. */
    if (opt & TBM_OPTION_WRITE)
    {
        if (device == TBM_DEVICE_CPU)
            fence.type = DMA_BUF_ACCESS_WRITE;
        else if (device == TBM_DEVICE_3D)
            fence.type = DMA_BUF_ACCESS_WRITE | DMA_BUF_ACCESS_DMA;
        else
        {
            DBG ("[libtbm-sprd:%d] %s GET_FENCE is ignored(device type is not 3D/CPU),\n", getpid(), __FUNCTION__);
            return 0;
        }
    }
    else if (opt & TBM_OPTION_READ)
    {
        if (device == TBM_DEVICE_CPU)
            fence.type = DMA_BUF_ACCESS_READ;
        else if (device == TBM_DEVICE_3D)
            fence.type = DMA_BUF_ACCESS_READ | DMA_BUF_ACCESS_DMA;
        else
        {
            DBG ("[libtbm-sprd:%d] %s GET_FENCE is ignored(device type is not 3D/CPU),\n", getpid(), __FUNCTION__);
            return 0;
        }
    }
    else
    {
        TBM_SPRD_LOG ("[libtbm-sprd:%d] error %s:%d Invalid argument\n", getpid(), __FUNCTION__, __LINE__);
        return 0;
    }

    /* Check if the tbm manager supports dma fence or not. */
    if (!bufmgr_sprd->use_dma_fence)
    {
        TBM_SPRD_LOG ("[libtbm-sprd:%d] "
                "error %s:%d  Not support DMA FENCE(%s)\n",
                getpid(), __FUNCTION__, __LINE__, strerror(errno) );
        return 0;

    }

    ret = ioctl(bo_sprd->dmabuf, DMABUF_IOCTL_GET_FENCE, &fence);
    if (ret < 0)
    {
        TBM_SPRD_LOG ("[libtbm-sprd:%d] "
                "error %s:%d  Can not set GET FENCE(%s)\n",
                getpid(), __FUNCTION__, __LINE__, strerror(errno) );
        return 0;
    }

    pthread_mutex_lock(&bo_sprd->mutex);
    int i;
    for (i = 0; i < DMA_FENCE_LIST_MAX; i++)
    {
        if (bo_sprd->dma_fence[i].ctx == 0)
        {
            bo_sprd->dma_fence[i].type = fence.type;
            bo_sprd->dma_fence[i].ctx = fence.ctx;
            break;
        }
    }
    if (i == DMA_FENCE_LIST_MAX)
    {
        //TODO: if dma_fence list is full, it needs realloc. I will fix this. by minseok3.kim
        TBM_SPRD_LOG ("[libtbm-sprd:%d] "
                "error %s:%d  fence list is full\n",
                getpid(), __FUNCTION__, __LINE__);
    }
    pthread_mutex_unlock(&bo_sprd->mutex);

    DBG ("[libtbm-sprd:%d] %s DMABUF_IOCTL_GET_FENCE! flink_id=%d dmabuf=%d\n", getpid(),
            __FUNCTION__, bo_sprd->name, bo_sprd->dmabuf);

#endif
    return 1;
}

static int
tbm_sprd_bo_unlock(tbm_bo bo)
{
    SPRD_RETURN_VAL_IF_FAIL (bo!=NULL, 0);

#if USE_BACKEND_LOCK
    tbm_bo_sprd bo_sprd;
    struct dma_buf_fence fence;
    int ret=0;

    bo_sprd = (tbm_bo_sprd)tbm_backend_get_bo_priv(bo);
    SPRD_RETURN_VAL_IF_FAIL (bo_sprd!=NULL, 0);

    if (!bo_sprd->dma_fence[0].ctx)
    {
        DBG ("[libtbm-sprd:%d] %s FENCE not support or ignored,\n", getpid(), __FUNCTION__);
        return 0;
    }

    if (!bo_sprd->dma_fence[0].type)
    {
        DBG ("[libtbm-sprd:%d] %s device type is not 3D/CPU,\n", getpid(), __FUNCTION__);
        return 0;
    }

    pthread_mutex_lock(&bo_sprd->mutex);
    fence.type = bo_sprd->dma_fence[0].type;
    fence.ctx = bo_sprd->dma_fence[0].ctx;
    int i;
    for (i = 1; i < DMA_FENCE_LIST_MAX; i++)
    {
        bo_sprd->dma_fence[i-1].type = bo_sprd->dma_fence[i].type;
        bo_sprd->dma_fence[i-1].ctx = bo_sprd->dma_fence[i].ctx;
    }
    bo_sprd->dma_fence[DMA_FENCE_LIST_MAX-1].type = 0;
    bo_sprd->dma_fence[DMA_FENCE_LIST_MAX-1].ctx = 0;
    pthread_mutex_unlock(&bo_sprd->mutex);

    ret = ioctl(bo_sprd->dmabuf, DMABUF_IOCTL_PUT_FENCE, &fence);
    if (ret < 0)
    {
        TBM_SPRD_LOG ("[libtbm-sprd:%d] "
                "error %s:%d  Can not set PUT FENCE(%s)\n",
                getpid(), __FUNCTION__, __LINE__, strerror(errno) );
        return 0;
    }

    DBG ("[libtbm-sprd:%d] %s DMABUF_IOCTL_PUT_FENCE! flink_id=%d dmabuf=%d\n", getpid(),
            __FUNCTION__, bo_sprd->name, bo_sprd->dmabuf);

#endif
    return 1;
}

static void
tbm_sprd_bufmgr_deinit (void *priv)
{
    SPRD_RETURN_IF_FAIL (priv!=NULL);

    tbm_bufmgr_sprd bufmgr_sprd;

    bufmgr_sprd = (tbm_bufmgr_sprd)priv;

    if (bufmgr_sprd->hashBos)
    {
        unsigned long key;
        void *value;

        while (drmHashFirst(bufmgr_sprd->hashBos, &key, &value) > 0)
        {
            free (value);
            drmHashDelete (bufmgr_sprd->hashBos, key);
        }

        drmHashDestroy (bufmgr_sprd->hashBos);
        bufmgr_sprd->hashBos = NULL;
    }

    free (bufmgr_sprd);
}

int
tbm_sprd_surface_supported_format(uint32_t **formats, uint32_t *num)
{
    uint32_t* color_formats=NULL;

    color_formats = (uint32_t*)calloc (1,sizeof(uint32_t)*TBM_COLOR_FORMAT_COUNT);

    if( color_formats == NULL )
    {
        return 0;
    }
    memcpy( color_formats, tbm_sprd_color_format_list , sizeof(uint32_t)*TBM_COLOR_FORMAT_COUNT );


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
tbm_sprd_surface_get_plane_data(tbm_surface_h surface, int width, int height, tbm_format format, int plane_idx, uint32_t *size, uint32_t *offset, uint32_t *pitch, int *bo_idx)
{
    int ret = 1;
    int bpp;
    int _offset =0;
    int _pitch =0;
    int _size =0;
    int _bo_idx = 0;

    switch(format)
    {
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
            _pitch = SIZE_ALIGN((width*bpp)>>3,TBM_SURFACE_ALIGNMENT_PITCH_RGB);
            _size = SIZE_ALIGN(_pitch*height,TBM_SURFACE_ALIGNMENT_PLANE);
            _bo_idx = 0;
            break;
        /* 24 bpp RGB */
        case TBM_FORMAT_RGB888:
        case TBM_FORMAT_BGR888:
            bpp = 24;
            _offset = 0;
            _pitch = SIZE_ALIGN((width*bpp)>>3,TBM_SURFACE_ALIGNMENT_PITCH_RGB);
            _size = SIZE_ALIGN(_pitch*height,TBM_SURFACE_ALIGNMENT_PLANE);
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
            _pitch = SIZE_ALIGN((width*bpp)>>3,TBM_SURFACE_ALIGNMENT_PITCH_RGB);
            _size = SIZE_ALIGN(_pitch*height,TBM_SURFACE_ALIGNMENT_PLANE);
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
            _pitch = SIZE_ALIGN((width*bpp)>>3,TBM_SURFACE_ALIGNMENT_PITCH_YUV);
            _size = SIZE_ALIGN(_pitch*height,TBM_SURFACE_ALIGNMENT_PLANE);
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
            if(plane_idx == 0)
            {
                _offset = 0;
                _pitch = SIZE_ALIGN( width ,TBM_SURFACE_ALIGNMENT_PITCH_YUV);
                _size = SIZE_ALIGN(_pitch*height,TBM_SURFACE_ALIGNMENT_PLANE);
                _bo_idx = 0;
            }
            else if( plane_idx ==1 )
            {
                _offset = width*height;
                _pitch = SIZE_ALIGN( width ,TBM_SURFACE_ALIGNMENT_PITCH_YUV/2);
                _size = SIZE_ALIGN(_pitch*(height/2),TBM_SURFACE_ALIGNMENT_PLANE);
                _bo_idx = 0;
            }
            break;

        case TBM_FORMAT_NV16:
        case TBM_FORMAT_NV61:
			bpp = 16;
            //if(plane_idx == 0)
            {
                _offset = 0;
		_pitch = SIZE_ALIGN(width,TBM_SURFACE_ALIGNMENT_PITCH_YUV);
                _size = SIZE_ALIGN(_pitch*height,TBM_SURFACE_ALIGNMENT_PLANE);
                _bo_idx = 0;
		if(plane_idx == 0)
			break;
            }
            //else if( plane_idx ==1 )
            {
                _offset += _size;
		_pitch = SIZE_ALIGN(width,TBM_SURFACE_ALIGNMENT_PITCH_YUV/2);
                _size = SIZE_ALIGN(_pitch*height,TBM_SURFACE_ALIGNMENT_PLANE);
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
                _pitch = SIZE_ALIGN(width,TBM_SURFACE_ALIGNMENT_PITCH_YUV);
                _size = SIZE_ALIGN(_pitch*height,TBM_SURFACE_ALIGNMENT_PLANE);
                _bo_idx = 0;
                if(plane_idx == 0)
                    break;
            }
            //else if( plane_idx == 1 )
            {
                _offset += _size;
                _pitch = SIZE_ALIGN(width/2,TBM_SURFACE_ALIGNMENT_PITCH_YUV/2);
                _size = SIZE_ALIGN(_pitch*(height/2),TBM_SURFACE_ALIGNMENT_PLANE);
                _bo_idx = 0;
                if(plane_idx == 1)
                    break;
            }
            //else if (plane_idx == 2 )
            {
                _offset += _size;
                _pitch = SIZE_ALIGN(width/2,TBM_SURFACE_ALIGNMENT_PITCH_YUV/2);
                _size = SIZE_ALIGN(_pitch*(height/2),TBM_SURFACE_ALIGNMENT_PLANE);
                _bo_idx = 0;
            }
            break;
        case TBM_FORMAT_YUV422:
        case TBM_FORMAT_YVU422:
            bpp = 16;
            //if(plane_idx == 0)
            {
                _offset = 0;
                _pitch = SIZE_ALIGN(width,TBM_SURFACE_ALIGNMENT_PITCH_YUV);
                _size = SIZE_ALIGN(_pitch*height,TBM_SURFACE_ALIGNMENT_PLANE);
                _bo_idx = 0;
                if(plane_idx == 0)
                    break;
            }
            //else if( plane_idx == 1 )
            {
                _offset += _size;
                _pitch = SIZE_ALIGN(width/2,TBM_SURFACE_ALIGNMENT_PITCH_YUV/2);
                _size = SIZE_ALIGN(_pitch*(height),TBM_SURFACE_ALIGNMENT_PLANE);
                _bo_idx = 0;
                if(plane_idx == 1)
                    break;
            }
            //else if (plane_idx == 2 )
            {
                _offset += _size;
                _pitch = SIZE_ALIGN(width/2,TBM_SURFACE_ALIGNMENT_PITCH_YUV/2);
                _size = SIZE_ALIGN(_pitch*(height),TBM_SURFACE_ALIGNMENT_PLANE);
                _bo_idx = 0;
            }
            break;
        case TBM_FORMAT_YUV444:
        case TBM_FORMAT_YVU444:
            bpp = 24;
            //if(plane_idx == 0)
            {
                _offset = 0;
                _pitch = SIZE_ALIGN(width,TBM_SURFACE_ALIGNMENT_PITCH_YUV);
                _size = SIZE_ALIGN(_pitch*height,TBM_SURFACE_ALIGNMENT_PLANE);
                _bo_idx = 0;
                if(plane_idx == 0)
                    break;
            }
            //else if( plane_idx == 1 )
            {
                _offset += _size;
                _pitch = SIZE_ALIGN(width,TBM_SURFACE_ALIGNMENT_PITCH_YUV);
                _size = SIZE_ALIGN(_pitch*height,TBM_SURFACE_ALIGNMENT_PLANE);
                _bo_idx = 0;
                if(plane_idx == 1)
                    break;
            }
            //else if (plane_idx == 2 )
            {
                _offset += _size;
                _pitch = SIZE_ALIGN(width,TBM_SURFACE_ALIGNMENT_PITCH_YUV);
               _size = SIZE_ALIGN(_pitch*height,TBM_SURFACE_ALIGNMENT_PLANE);
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
/**
* @brief get the size of the surface with a format.
* @param[in] surface : the surface
* @param[in] width : the width of the surface
* @param[in] height : the height of the surface
* @param[in] format : the format of the surface
* @return size of the surface if this function succeeds, otherwise 0.
*/

int
tbm_sprd_surface_get_size(tbm_surface_h surface, int width, int height, tbm_format format)
{
    int ret = 0;
    int bpp = 0;
    int _pitch =0;
    int _size =0;
    int align =TBM_SURFACE_ALIGNMENT_PLANE;


    switch(format)
    {
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
            _pitch = SIZE_ALIGN((width*bpp)>>3,TBM_SURFACE_ALIGNMENT_PITCH_RGB);
            _size = SIZE_ALIGN(_pitch*height,TBM_SURFACE_ALIGNMENT_PLANE);
            break;
        /* 24 bpp RGB */
        case TBM_FORMAT_RGB888:
        case TBM_FORMAT_BGR888:
            bpp = 24;
            _pitch = SIZE_ALIGN((width*bpp)>>3,TBM_SURFACE_ALIGNMENT_PITCH_RGB);
            _size = SIZE_ALIGN(_pitch*height,TBM_SURFACE_ALIGNMENT_PLANE);
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
            _pitch = SIZE_ALIGN((width*bpp)>>3,TBM_SURFACE_ALIGNMENT_PITCH_RGB);
            _size = SIZE_ALIGN(_pitch*height,TBM_SURFACE_ALIGNMENT_PLANE);
            break;
        /* packed YCbCr */
        case TBM_FORMAT_YUYV:
        case TBM_FORMAT_YVYU:
        case TBM_FORMAT_UYVY:
        case TBM_FORMAT_VYUY:
        case TBM_FORMAT_AYUV:
            bpp = 32;
            _pitch = SIZE_ALIGN((width*bpp)>>3,TBM_SURFACE_ALIGNMENT_PITCH_YUV);
            _size = SIZE_ALIGN(_pitch*height,TBM_SURFACE_ALIGNMENT_PLANE);
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
            //plane_idx == 0
            {
                _pitch = SIZE_ALIGN( width ,TBM_SURFACE_ALIGNMENT_PITCH_YUV);
                _size = SIZE_ALIGN(_pitch*height,TBM_SURFACE_ALIGNMENT_PLANE);
            }
            //plane_idx ==1
            {
                _pitch = SIZE_ALIGN( width ,TBM_SURFACE_ALIGNMENT_PITCH_YUV/2);
                _size += SIZE_ALIGN(_pitch*(height/2),TBM_SURFACE_ALIGNMENT_PLANE);
            }
            break;
        case TBM_FORMAT_NV16:
        case TBM_FORMAT_NV61:
            bpp = 16;
            //plane_idx == 0
            {
                _pitch = SIZE_ALIGN(width,TBM_SURFACE_ALIGNMENT_PITCH_YUV);
                _size = SIZE_ALIGN(_pitch*height,TBM_SURFACE_ALIGNMENT_PLANE);
            }
            //plane_idx ==1
            {
                _pitch = SIZE_ALIGN(width,TBM_SURFACE_ALIGNMENT_PITCH_YUV/2);
                _size += SIZE_ALIGN(_pitch*height,TBM_SURFACE_ALIGNMENT_PLANE);
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
        case TBM_FORMAT_YUV410:
        case TBM_FORMAT_YVU410:
            bpp = 9;
	    align = TBM_SURFACE_ALIGNMENT_PITCH_YUV;
            break;
        case TBM_FORMAT_YUV411:
        case TBM_FORMAT_YVU411:
        case TBM_FORMAT_YUV420:
        case TBM_FORMAT_YVU420:
            bpp = 12;
	    //plane_idx == 0
            {
		_pitch = SIZE_ALIGN(width,TBM_SURFACE_ALIGNMENT_PITCH_YUV);
                _size = SIZE_ALIGN(_pitch*height,TBM_SURFACE_ALIGNMENT_PLANE);
            }
            //plane_idx == 1
            {
		_pitch = SIZE_ALIGN(width/2,TBM_SURFACE_ALIGNMENT_PITCH_YUV/2);
		_size += SIZE_ALIGN(_pitch*(height/2),TBM_SURFACE_ALIGNMENT_PLANE);
            }
            //plane_idx == 2
            {
		_pitch = SIZE_ALIGN(width/2,TBM_SURFACE_ALIGNMENT_PITCH_YUV/2);
                _size += SIZE_ALIGN(_pitch*(height/2),TBM_SURFACE_ALIGNMENT_PLANE);
            }

            break;
        case TBM_FORMAT_YUV422:
        case TBM_FORMAT_YVU422:
            bpp = 16;
	    //plane_idx == 0
            {
		_pitch = SIZE_ALIGN(width,TBM_SURFACE_ALIGNMENT_PITCH_YUV);
                _size = SIZE_ALIGN(_pitch*height,TBM_SURFACE_ALIGNMENT_PLANE);
            }
            //plane_idx == 1
            {
		_pitch = SIZE_ALIGN(width/2,TBM_SURFACE_ALIGNMENT_PITCH_YUV/2);
		_size += SIZE_ALIGN(_pitch*height,TBM_SURFACE_ALIGNMENT_PLANE);
            }
            //plane_idx == 2
            {
		_pitch = SIZE_ALIGN(width/2,TBM_SURFACE_ALIGNMENT_PITCH_YUV/2);
                _size += SIZE_ALIGN(_pitch*height,TBM_SURFACE_ALIGNMENT_PLANE);
            }
            break;
        case TBM_FORMAT_YUV444:
        case TBM_FORMAT_YVU444:
            bpp = 24;
	    align = TBM_SURFACE_ALIGNMENT_PITCH_YUV;
            break;

        default:
            bpp = 0;
            break;
    }

	if(_size > 0)
		ret = _size;
	else
	    ret =  SIZE_ALIGN( (width * height * bpp) >> 3, align);

    return ret;

}

int
tbm_sprd_surface_get_num_bos(tbm_format format)
{
	int num = 0;

    switch(format)
    {
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
        /* 24 bpp RGB */
        case TBM_FORMAT_RGB888:
        case TBM_FORMAT_BGR888:
        /* 32 bpp RGB */
        case TBM_FORMAT_XRGB8888:
        case TBM_FORMAT_XBGR8888:
        case TBM_FORMAT_RGBX8888:
        case TBM_FORMAT_BGRX8888:
        case TBM_FORMAT_ARGB8888:
        case TBM_FORMAT_ABGR8888:
        case TBM_FORMAT_RGBA8888:
        case TBM_FORMAT_BGRA8888:
        /* packed YCbCr */
        case TBM_FORMAT_YUYV:
        case TBM_FORMAT_YVYU:
        case TBM_FORMAT_UYVY:
        case TBM_FORMAT_VYUY:
        case TBM_FORMAT_AYUV:
        /*
        * 2 plane YCbCr
        * index 0 = Y plane, [7:0] Y
        * index 1 = Cr:Cb plane, [15:0] Cr:Cb little endian
        * or
        * index 1 = Cb:Cr plane, [15:0] Cb:Cr little endian
        */
        case TBM_FORMAT_NV12:
        case TBM_FORMAT_NV21:
        case TBM_FORMAT_NV16:
        case TBM_FORMAT_NV61:
        /*
        * 3 plane YCbCr
        * index 0: Y plane, [7:0] Y
        * index 1: Cb plane, [7:0] Cb
        * index 2: Cr plane, [7:0] Cr
        * or
        * index 1: Cr plane, [7:0] Cr
        * index 2: Cb plane, [7:0] Cb
        */
        case TBM_FORMAT_YUV410:
        case TBM_FORMAT_YVU410:
        case TBM_FORMAT_YUV411:
        case TBM_FORMAT_YVU411:
        case TBM_FORMAT_YUV420:
        case TBM_FORMAT_YVU420:
        case TBM_FORMAT_YUV422:
        case TBM_FORMAT_YVU422:
        case TBM_FORMAT_YUV444:
        case TBM_FORMAT_YVU444:
            num = 1;
            break;

        default:
            num = 0;
            break;
    }

    return num;
}

MODULEINITPPROTO (init_tbm_bufmgr_priv);

static TBMModuleVersionInfo SprdVersRec =
{
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
    if (!bufmgr_sprd)
    {
        TBM_SPRD_LOG ("[libtbm-sprd:%d] error: Fail to alloc bufmgr_sprd!\n", getpid());
        return 0;
    }

    bufmgr_sprd->fd = fd;
    if (bufmgr_sprd->fd < 0)
    {
        TBM_SPRD_LOG ("[libtbm-sprd:%d] error: Fail to create drm!\n", getpid());
        free (bufmgr_sprd);
        return 0;
    }

    //Create Hash Table
    bufmgr_sprd->hashBos = drmHashCreate ();

    //Check if the tbm manager supports dma fence or not.
    int fp = open("/sys/module/dmabuf_sync/parameters/enabled", O_RDONLY);
    int length;
    char buf[1];
    if (fp != -1)
    {
        length = read(fp, buf, 1);

        if (length == 1 && buf[0] == '1')
            bufmgr_sprd->use_dma_fence = 1;

        close(fp);
    }

    bufmgr_backend = tbm_backend_alloc();
    if (!bufmgr_backend)
    {
        TBM_SPRD_LOG ("[libtbm-sprd:%d] error: Fail to create drm!\n", getpid());
        free (bufmgr_sprd);
        return 0;
    }

    bufmgr_backend->priv = (void *)bufmgr_sprd;
    bufmgr_backend->bufmgr_deinit = tbm_sprd_bufmgr_deinit,
    bufmgr_backend->bo_size = tbm_sprd_bo_size,
    bufmgr_backend->bo_alloc = tbm_sprd_bo_alloc,
    bufmgr_backend->bo_free = tbm_sprd_bo_free,
    bufmgr_backend->bo_import = tbm_sprd_bo_import,
    bufmgr_backend->bo_export = tbm_sprd_bo_export,
    bufmgr_backend->bo_get_handle = tbm_sprd_bo_get_handle,
    bufmgr_backend->bo_map = tbm_sprd_bo_map,
    bufmgr_backend->bo_unmap = tbm_sprd_bo_unmap,
    bufmgr_backend->bo_cache_flush = tbm_sprd_bo_cache_flush,
    bufmgr_backend->bo_get_global_key = tbm_sprd_bo_get_global_key;
    bufmgr_backend->surface_get_plane_data = tbm_sprd_surface_get_plane_data;
    bufmgr_backend->surface_get_size = tbm_sprd_surface_get_size;
    bufmgr_backend->surface_supported_format = tbm_sprd_surface_supported_format;
    bufmgr_backend->surface_get_num_bos = tbm_sprd_surface_get_num_bos;

    if (bufmgr_sprd->use_dma_fence)
    {
        bufmgr_backend->flags = (TBM_LOCK_CTRL_BACKEND | TBM_CACHE_CTRL_BACKEND);
        bufmgr_backend->bo_lock = NULL;
        bufmgr_backend->bo_lock2 = tbm_sprd_bo_lock;
        bufmgr_backend->bo_unlock = tbm_sprd_bo_unlock;
    }
    else
    {
        bufmgr_backend->flags = 0;
        bufmgr_backend->bo_lock = NULL;
        bufmgr_backend->bo_unlock = NULL;
    }

    if (!tbm_backend_init (bufmgr, bufmgr_backend))
    {
        TBM_SPRD_LOG ("[libtbm-sprd:%d] error: Fail to init backend!\n", getpid());
        tbm_backend_free (bufmgr_backend);
        free (bufmgr_sprd);
        return 0;
    }

#ifdef DEBUG
    {
        char* env;
        env = getenv ("TBM_SPRD_DEBUG");
        if (env)
        {
            bDebug = atoi (env);
            TBM_SPRD_LOG ("TBM_SPRD_DEBUG=%s\n", env);
        }
        else
        {
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


