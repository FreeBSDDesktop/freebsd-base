/*
 * CDDL HEADER START
 *
 * The contents of this file are subject to the terms of the
 * Common Development and Distribution License (the "License").
 * You may not use this file except in compliance with the License.
 *
 * You can obtain a copy of the license at usr/src/OPENSOLARIS.LICENSE
 * or http://www.opensolaris.org/os/licensing.
 * See the License for the specific language governing permissions
 * and limitations under the License.
 *
 * When distributing Covered Code, include this CDDL HEADER in each
 * file and include the License file at usr/src/OPENSOLARIS.LICENSE.
 * If applicable, add the following below this CDDL HEADER, with the
 * fields enclosed by brackets "[]" replaced with your own identifying
 * information: Portions Copyright [yyyy] [name of copyright owner]
 *
 * CDDL HEADER END
 */
/*
 * Copyright (c) 2006 Pawel Jakub Dawidek <pjd@FreeBSD.org>
 * All rights reserved.
 *
 * Portions Copyright (c) 2012 Martin Matuska <mm@FreeBSD.org>
 */

#include <sys/zfs_context.h>
#include <sys/param.h>
#include <sys/kernel.h>
#include <sys/bio.h>
#include <sys/disk.h>
#include <sys/spa.h>
#include <sys/spa_impl.h>
#include <sys/vdev_impl.h>
#include <sys/fs/zfs.h>
#include <sys/zio.h>
#include <geom/geom.h>
#include <geom/geom_int.h>

/*
 * Virtual device vector for GEOM.
 */

struct g_class zfs_vdev_class = {
	.name = "ZFS::VDEV",
	.version = G_VERSION,
};

DECLARE_GEOM_CLASS(zfs_vdev_class, zfs_vdev);

SYSCTL_DECL(_vfs_zfs_vdev);
/* Don't send BIO_FLUSH. */
static int vdev_geom_bio_flush_disable = 0;
TUNABLE_INT("vfs.zfs.vdev.bio_flush_disable", &vdev_geom_bio_flush_disable);
SYSCTL_INT(_vfs_zfs_vdev, OID_AUTO, bio_flush_disable, CTLFLAG_RW,
    &vdev_geom_bio_flush_disable, 0, "Disable BIO_FLUSH");
/* Don't send BIO_DELETE. */
static int vdev_geom_bio_delete_disable = 0;
TUNABLE_INT("vfs.zfs.vdev.bio_delete_disable", &vdev_geom_bio_delete_disable);
SYSCTL_INT(_vfs_zfs_vdev, OID_AUTO, bio_delete_disable, CTLFLAG_RW,
    &vdev_geom_bio_delete_disable, 0, "Disable BIO_DELETE");

static void
vdev_geom_orphan(struct g_consumer *cp)
{
	vdev_t *vd;

	g_topology_assert();

	vd = cp->private;
	if (vd == NULL) {
		/* Vdev close in progress.  Ignore the event. */
		return;
	}

	/*
	 * Orphan callbacks occur from the GEOM event thread.
	 * Concurrent with this call, new I/O requests may be
	 * working their way through GEOM about to find out
	 * (only once executed by the g_down thread) that we've
	 * been orphaned from our disk provider.  These I/Os
	 * must be retired before we can detach our consumer.
	 * This is most easily achieved by acquiring the
	 * SPA ZIO configuration lock as a writer, but doing
	 * so with the GEOM topology lock held would cause
	 * a lock order reversal.  Instead, rely on the SPA's
	 * async removal support to invoke a close on this
	 * vdev once it is safe to do so.
	 */
	vd->vdev_remove_wanted = B_TRUE;
	spa_async_request(vd->vdev_spa, SPA_ASYNC_REMOVE);
}

static void
vdev_geom_attrchanged(struct g_consumer *cp, const char *attr)
{
	vdev_t *vd;
	spa_t *spa;
	char *physpath;
	int error, physpath_len;

	g_topology_assert();

	if (strcmp(attr, "GEOM::physpath") != 0)
		return;

	if (g_access(cp, 1, 0, 0) != 0)
		return;

	/*
	 * Record/Update physical path information for this device.
	 */
	vd = cp->private;
	spa = vd->vdev_spa;
	physpath_len = MAXPATHLEN;
	physpath = g_malloc(physpath_len, M_WAITOK|M_ZERO);
	error = g_io_getattr("GEOM::physpath", cp, &physpath_len, physpath);
	g_access(cp, -1, 0, 0);
	if (error == 0) {
		char *old_physpath;

		old_physpath = vd->vdev_physpath;
		vd->vdev_physpath = spa_strdup(physpath);
		spa_async_request(spa, SPA_ASYNC_CONFIG_UPDATE);

		if (old_physpath != NULL) {
			int held_lock;

			held_lock = spa_config_held(spa, SCL_STATE, RW_WRITER);
			if (held_lock == 0) {
				g_topology_unlock();
				spa_config_enter(spa, SCL_STATE, FTAG,
				    RW_WRITER);
			}

			spa_strfree(old_physpath);

			if (held_lock == 0) {
				spa_config_exit(spa, SCL_STATE, FTAG);
				g_topology_lock();
			}
		}
	}
	g_free(physpath);
}

static struct g_consumer *
vdev_geom_attach(struct g_provider *pp, vdev_t *vd)
{
	struct g_geom *gp;
	struct g_consumer *cp;

	g_topology_assert();

	ZFS_LOG(1, "Attaching to %s.", pp->name);
	/* Do we have geom already? No? Create one. */
	LIST_FOREACH(gp, &zfs_vdev_class.geom, geom) {
		if (gp->flags & G_GEOM_WITHER)
			continue;
		if (strcmp(gp->name, "zfs::vdev") != 0)
			continue;
		break;
	}
	if (gp == NULL) {
		gp = g_new_geomf(&zfs_vdev_class, "zfs::vdev");
		gp->orphan = vdev_geom_orphan;
		gp->attrchanged = vdev_geom_attrchanged;
		cp = g_new_consumer(gp);
		if (g_attach(cp, pp) != 0) {
			g_wither_geom(gp, ENXIO);
			return (NULL);
		}
		if (g_access(cp, 1, 0, 1) != 0) {
			g_wither_geom(gp, ENXIO);
			return (NULL);
		}
		ZFS_LOG(1, "Created geom and consumer for %s.", pp->name);
	} else {
		/* Check if we are already connected to this provider. */
		LIST_FOREACH(cp, &gp->consumer, consumer) {
			if (cp->provider == pp) {
				ZFS_LOG(1, "Found consumer for %s.", pp->name);
				break;
			}
		}
		if (cp == NULL) {
			cp = g_new_consumer(gp);
			if (g_attach(cp, pp) != 0) {
				g_destroy_consumer(cp);
				return (NULL);
			}
			if (g_access(cp, 1, 0, 1) != 0) {
				g_detach(cp);
				g_destroy_consumer(cp);
				return (NULL);
			}
			ZFS_LOG(1, "Created consumer for %s.", pp->name);
		} else {
			if (g_access(cp, 1, 0, 1) != 0)
				return (NULL);
			ZFS_LOG(1, "Used existing consumer for %s.", pp->name);
		}
	}

	cp->private = vd;

	/* Fetch initial physical path information for this device. */
	vdev_geom_attrchanged(cp, "GEOM::physpath");
	
	return (cp);
}

static void
vdev_geom_detach(void *arg)
{
	struct g_geom *gp;
	struct g_consumer *cp;
	vdev_t *vd;

	g_topology_assert();
	cp = arg;
	gp = cp->geom;

	ZFS_LOG(1, "Closing access to %s.", cp->provider->name);
	vd = cp->private;
	if (vd != NULL) {
		vd->vdev_tsd = NULL;
		cp->private = NULL;
	}
	g_access(cp, -1, 0, -1);
	/* Destroy consumer on last close. */
	if (cp->acr == 0 && cp->ace == 0) {
		ZFS_LOG(1, "Destroyed consumer to %s.", cp->provider->name);
		if (cp->acw > 0)
			g_access(cp, 0, -cp->acw, 0);
		g_detach(cp);
		g_destroy_consumer(cp);
	}
	/* Destroy geom if there are no consumers left. */
	if (LIST_EMPTY(&gp->consumer)) {
		ZFS_LOG(1, "Destroyed geom %s.", gp->name);
		g_wither_geom(gp, ENXIO);
	}
}

static void
nvlist_get_guids(nvlist_t *list, uint64_t *pguid, uint64_t *vguid)
{
	nvpair_t *elem = NULL;

	*vguid = 0;
	*pguid = 0;
	while ((elem = nvlist_next_nvpair(list, elem)) != NULL) {
		if (nvpair_type(elem) != DATA_TYPE_UINT64)
			continue;

		if (strcmp(nvpair_name(elem), ZPOOL_CONFIG_POOL_GUID) == 0) {
			VERIFY(nvpair_value_uint64(elem, pguid) == 0);
		} else if (strcmp(nvpair_name(elem), ZPOOL_CONFIG_GUID) == 0) {
			VERIFY(nvpair_value_uint64(elem, vguid) == 0);
		}

		if (*pguid != 0 && *vguid != 0)
			break;
	}
}

static int
vdev_geom_io(struct g_consumer *cp, int cmd, void *data, off_t offset, off_t size)
{
	struct bio *bp;
	u_char *p;
	off_t off, maxio;
	int error;

	ASSERT((offset % cp->provider->sectorsize) == 0);
	ASSERT((size % cp->provider->sectorsize) == 0);

	bp = g_alloc_bio();
	off = offset;
	offset += size;
	p = data;
	maxio = MAXPHYS - (MAXPHYS % cp->provider->sectorsize);
	error = 0;

	for (; off < offset; off += maxio, p += maxio, size -= maxio) {
		bzero(bp, sizeof(*bp));
		bp->bio_cmd = cmd;
		bp->bio_done = NULL;
		bp->bio_offset = off;
		bp->bio_length = MIN(size, maxio);
		bp->bio_data = p;
		g_io_request(bp, cp);
		error = biowait(bp, "vdev_geom_io");
		if (error != 0)
			break;
	}

	g_destroy_bio(bp);
	return (error);
}

static void
vdev_geom_read_guids(struct g_consumer *cp, uint64_t *pguid, uint64_t *vguid)
{
	struct g_provider *pp;
	vdev_label_t *label;
	char *p, *buf;
	size_t buflen;
	uint64_t psize;
	off_t offset, size;
	int error, l, len;

	g_topology_assert_not();

	*pguid = 0;
	*vguid = 0;
	pp = cp->provider;
	ZFS_LOG(1, "Reading guids from %s...", pp->name);

	psize = pp->mediasize;
	psize = P2ALIGN(psize, (uint64_t)sizeof(vdev_label_t));

	size = sizeof(*label) + pp->sectorsize -
	    ((sizeof(*label) - 1) % pp->sectorsize) - 1;

	label = kmem_alloc(size, KM_SLEEP);
	buflen = sizeof(label->vl_vdev_phys.vp_nvlist);

	for (l = 0; l < VDEV_LABELS; l++) {
		nvlist_t *config = NULL;

		offset = vdev_label_offset(psize, l, 0);
		if ((offset % pp->sectorsize) != 0)
			continue;

		if (vdev_geom_io(cp, BIO_READ, label, offset, size) != 0)
			continue;
		buf = label->vl_vdev_phys.vp_nvlist;

		if (nvlist_unpack(buf, buflen, &config, 0) != 0)
			continue;

		nvlist_get_guids(config, pguid, vguid);
		nvlist_free(config);
		if (*pguid != 0 && *vguid != 0)
			break;
	}

	kmem_free(label, size);
	if (*pguid != 0 && *vguid != 0)
		ZFS_LOG(1, "guids for %s are %ju:%ju", pp->name,
		    (uintmax_t)*pguid, (uintmax_t)*vguid);
}

static void
vdev_geom_taste_orphan(struct g_consumer *cp)
{

	KASSERT(1 == 0, ("%s called while tasting %s.", __func__,
	    cp->provider->name));
}

static struct g_consumer *
vdev_geom_attach_by_guids(vdev_t *vd)
{
	struct g_class *mp;
	struct g_geom *gp, *zgp;
	struct g_provider *pp;
	struct g_consumer *cp, *zcp;
	uint64_t pguid;
	uint64_t vguid;

	g_topology_assert();

	zgp = g_new_geomf(&zfs_vdev_class, "zfs::vdev::taste");
	/* This orphan function should be never called. */
	zgp->orphan = vdev_geom_taste_orphan;
	zcp = g_new_consumer(zgp);

	cp = NULL;
	LIST_FOREACH(mp, &g_classes, class) {
		if (mp == &zfs_vdev_class)
			continue;
		LIST_FOREACH(gp, &mp->geom, geom) {
			if (gp->flags & G_GEOM_WITHER)
				continue;
			LIST_FOREACH(pp, &gp->provider, provider) {
				if (pp->flags & G_PF_WITHER)
					continue;
				g_attach(zcp, pp);
				if (g_access(zcp, 1, 0, 0) != 0) {
					g_detach(zcp);
					continue;
				}
				g_topology_unlock();
				vdev_geom_read_guids(zcp, &pguid, &vguid);
				g_topology_lock();
				g_access(zcp, -1, 0, 0);
				g_detach(zcp);
				if (pguid != spa_guid(vd->vdev_spa) ||
				    vguid != vd->vdev_guid)
					continue;
				cp = vdev_geom_attach(pp, vd);
				if (cp == NULL) {
					printf("ZFS WARNING: Unable to attach to %s.\n",
					    pp->name);
					continue;
				}
				break;
			}
			if (cp != NULL)
				break;
		}
		if (cp != NULL)
			break;
	}
end:
	g_destroy_consumer(zcp);
	g_destroy_geom(zgp);
	return (cp);
}

static struct g_consumer *
vdev_geom_open_by_guids(vdev_t *vd)
{
	struct g_consumer *cp;
	char *buf;
	size_t len;

	g_topology_assert();

	ZFS_LOG(1, "Searching by guids [%ju:%ju].",
	    (uintmax_t)spa_guid(vd->vdev_spa), (uintmax_t)vd->vdev_guid);
	cp = vdev_geom_attach_by_guids(vd);
	if (cp != NULL) {
		len = strlen(cp->provider->name) + strlen("/dev/") + 1;
		buf = kmem_alloc(len, KM_SLEEP);

		snprintf(buf, len, "/dev/%s", cp->provider->name);
		spa_strfree(vd->vdev_path);
		vd->vdev_path = buf;

		ZFS_LOG(1, "Attach by guids [%ju:%ju] succeeded, provider %s.",
		    (uintmax_t)spa_guid(vd->vdev_spa),
		    (uintmax_t)vd->vdev_guid, vd->vdev_path);
	} else {
		ZFS_LOG(1, "Search by guids [%ju:%ju] failed.",
		    (uintmax_t)spa_guid(vd->vdev_spa),
		    (uintmax_t)vd->vdev_guid);
	}

	return (cp);
}

static struct g_consumer *
vdev_geom_open_by_path(vdev_t *vd, int check_guid)
{
	struct g_provider *pp;
	struct g_consumer *cp;
	uint64_t pguid;
	uint64_t vguid;

	g_topology_assert();

	cp = NULL;
	pp = g_provider_by_name(vd->vdev_path + sizeof("/dev/") - 1);
	if (pp != NULL) {
		ZFS_LOG(1, "Found provider by name %s.", vd->vdev_path);
		cp = vdev_geom_attach(pp, vd);
		if (cp != NULL && check_guid && ISP2(pp->sectorsize) &&
		    pp->sectorsize <= VDEV_PAD_SIZE) {
			g_topology_unlock();
			vdev_geom_read_guids(cp, &pguid, &vguid);
			g_topology_lock();
			if (pguid != spa_guid(vd->vdev_spa) ||
			    vguid != vd->vdev_guid) {
				vdev_geom_detach(cp);
				cp = NULL;
				ZFS_LOG(1, "guid mismatch for provider %s: "
				    "%ju:%ju != %ju:%ju.", vd->vdev_path,
				    (uintmax_t)spa_guid(vd->vdev_spa),
				    (uintmax_t)vd->vdev_guid,
				    (uintmax_t)pguid, (uintmax_t)vguid);
			} else {
				ZFS_LOG(1, "guids match for provider %s.",
				    vd->vdev_path);
			}
		}
	}

	return (cp);
}

static int
vdev_geom_open(vdev_t *vd, uint64_t *psize, uint64_t *max_psize,
    uint64_t *ashift)
{
	struct g_provider *pp;
	struct g_consumer *cp;
	size_t bufsize;
	int error;

	/*
	 * We must have a pathname, and it must be absolute.
	 */
	if (vd->vdev_path == NULL || vd->vdev_path[0] != '/') {
		vd->vdev_stat.vs_aux = VDEV_AUX_BAD_LABEL;
		return (EINVAL);
	}

	vd->vdev_tsd = NULL;

	DROP_GIANT();
	g_topology_lock();
	error = 0;

	/*
	 * Try using the recorded path for this device, but only
	 * accept it if its label data contains the expected GUIDs.
	 */
	cp = vdev_geom_open_by_path(vd, 1);
	if (cp == NULL) {
		/*
		 * The device at vd->vdev_path doesn't have the
		 * expected GUIDs. The disks might have merely
		 * moved around so try all other GEOM providers
		 * to find one with the right GUIDs.
		 */
		cp = vdev_geom_open_by_guids(vd);
	}

	if (cp == NULL &&
	    ((vd->vdev_prevstate == VDEV_STATE_UNKNOWN &&
	      vd->vdev_spa->spa_load_state == SPA_LOAD_NONE) ||
	     vd->vdev_spa->spa_splitting_newspa == B_TRUE)) {
		/*
		 * We are dealing with a vdev that hasn't been previosly
		 * opened (since boot), and we are not loading an
		 * existing pool configuration (e.g. this operations is
		 * an add of a vdev to new or * existing pool) or we are
		 * in the process of splitting a pool.  Find the GEOM
		 * provider by its name, ignoring GUID mismatches.
		 *
		 * XXPOLICY: It would be safer to only allow a device
		 *           that is unlabeled or labeled but missing
		 *           GUID information to be opened in this fashion.
		 */
		cp = vdev_geom_open_by_path(vd, 0);
	}

	if (cp == NULL) {
		ZFS_LOG(1, "Provider %s not found.", vd->vdev_path);
		error = ENOENT;
	} else if (cp->provider->sectorsize > VDEV_PAD_SIZE ||
	    !ISP2(cp->provider->sectorsize)) {
		ZFS_LOG(1, "Provider %s has unsupported sectorsize.",
		    vd->vdev_path);
		vdev_geom_detach(cp);
		error = EINVAL;
		cp = NULL;
	} else if (cp->acw == 0 && (spa_mode(vd->vdev_spa) & FWRITE) != 0) {
		int i;

		for (i = 0; i < 5; i++) {
			error = g_access(cp, 0, 1, 0);
			if (error == 0)
				break;
			g_topology_unlock();
			tsleep(vd, 0, "vdev", hz / 2);
			g_topology_lock();
		}
		if (error != 0) {
			printf("ZFS WARNING: Unable to open %s for writing (error=%d).\n",
			    vd->vdev_path, error);
			vdev_geom_detach(cp);
			cp = NULL;
		}
	}

	g_topology_unlock();
	PICKUP_GIANT();
	if (cp == NULL) {
		vd->vdev_stat.vs_aux = VDEV_AUX_OPEN_FAILED;
		return (error);
	}
	pp = cp->provider;
	vd->vdev_tsd = cp;

	/*
	 * Determine the actual size of the device.
	 */
	*max_psize = *psize = pp->mediasize;

	/*
	 * Determine the device's minimum transfer size.
	 */
	*ashift = highbit(MAX(pp->sectorsize, SPA_MINBLOCKSIZE)) - 1;

	/*
	 * Clear the nowritecache settings, so that on a vdev_reopen()
	 * we will try again.
	 */
	vd->vdev_nowritecache = B_FALSE;

	return (0);
}

static void
vdev_geom_close(vdev_t *vd)
{
	struct g_consumer *cp;

	cp = vd->vdev_tsd;
	if (cp == NULL)
		return;
	g_topology_lock();
	vdev_geom_detach(cp);
	g_topology_unlock();
}

static void
vdev_geom_io_intr(struct bio *bp)
{
	vdev_t *vd;
	zio_t *zio;

	zio = bp->bio_caller1;
	vd = zio->io_vd;
	zio->io_error = bp->bio_error;
	if (zio->io_error == 0 && bp->bio_resid != 0)
		zio->io_error = EIO;
	if (bp->bio_cmd == BIO_FLUSH && bp->bio_error == ENOTSUP) {
		/*
		 * If we get ENOTSUP, we know that no future
		 * attempts will ever succeed.  In this case we
		 * set a persistent bit so that we don't bother
		 * with the ioctl in the future.
		 */
		vd->vdev_nowritecache = B_TRUE;
	}
	if (bp->bio_cmd == BIO_DELETE && bp->bio_error == ENOTSUP) {
		/*
		 * If we get ENOTSUP, we know that no future
		 * attempts will ever succeed.  In this case we
		 * set a persistent bit so that we don't bother
		 * with the ioctl in the future.
		 */
		vd->vdev_notrim = B_TRUE;
	}
	if (zio->io_error == EIO && !vd->vdev_remove_wanted) {
		/*
		 * If provider's error is set we assume it is being
		 * removed.
		 */
		if (bp->bio_to->error != 0) {
			/*
			 * We post the resource as soon as possible, instead of
			 * when the async removal actually happens, because the
			 * DE is using this information to discard previous I/O
			 * errors.
			 */
			/* XXX: zfs_post_remove() can sleep. */
			zfs_post_remove(zio->io_spa, vd);
			vd->vdev_remove_wanted = B_TRUE;
			spa_async_request(zio->io_spa, SPA_ASYNC_REMOVE);
		} else if (!vd->vdev_delayed_close) {
			vd->vdev_delayed_close = B_TRUE;
		}
	}
	g_destroy_bio(bp);
	zio_interrupt(zio);
}

static int
vdev_geom_io_start(zio_t *zio)
{
	vdev_t *vd;
	struct g_consumer *cp;
	struct bio *bp;
	int error;

	vd = zio->io_vd;

	if (zio->io_type == ZIO_TYPE_IOCTL) {
		/* XXPOLICY */
		if (!vdev_readable(vd)) {
			zio->io_error = ENXIO;
			return (ZIO_PIPELINE_CONTINUE);
		}

		switch (zio->io_cmd) {
		case DKIOCFLUSHWRITECACHE:
			if (zfs_nocacheflush || vdev_geom_bio_flush_disable)
				break;
			if (vd->vdev_nowritecache) {
				zio->io_error = ENOTSUP;
				break;
			}
			goto sendreq;
		case DKIOCTRIM:
			if (vdev_geom_bio_delete_disable)
				break;
			if (vd->vdev_notrim) {
				zio->io_error = ENOTSUP;
				break;
			}
			goto sendreq;
		default:
			zio->io_error = ENOTSUP;
		}

		return (ZIO_PIPELINE_CONTINUE);
	}
sendreq:
	cp = vd->vdev_tsd;
	if (cp == NULL) {
		zio->io_error = ENXIO;
		return (ZIO_PIPELINE_CONTINUE);
	}
	bp = g_alloc_bio();
	bp->bio_caller1 = zio;
	switch (zio->io_type) {
	case ZIO_TYPE_READ:
	case ZIO_TYPE_WRITE:
		bp->bio_cmd = zio->io_type == ZIO_TYPE_READ ? BIO_READ : BIO_WRITE;
		bp->bio_data = zio->io_data;
		bp->bio_offset = zio->io_offset;
		bp->bio_length = zio->io_size;
		break;
	case ZIO_TYPE_IOCTL:
		switch (zio->io_cmd) {
		case DKIOCFLUSHWRITECACHE:
			bp->bio_cmd = BIO_FLUSH;
			bp->bio_flags |= BIO_ORDERED;
			bp->bio_data = NULL;
			bp->bio_offset = cp->provider->mediasize;
			bp->bio_length = 0;
			break;
		case DKIOCTRIM:
			bp->bio_cmd = BIO_DELETE;
			bp->bio_data = NULL;
			bp->bio_offset = zio->io_offset;
			bp->bio_length = zio->io_size;
			break;
		}
		break;
	}
	bp->bio_done = vdev_geom_io_intr;

	g_io_request(bp, cp);

	return (ZIO_PIPELINE_STOP);
}

static void
vdev_geom_io_done(zio_t *zio)
{
}

static void
vdev_geom_hold(vdev_t *vd)
{
}

static void
vdev_geom_rele(vdev_t *vd)
{
}

vdev_ops_t vdev_geom_ops = {
	vdev_geom_open,
	vdev_geom_close,
	vdev_default_asize,
	vdev_geom_io_start,
	vdev_geom_io_done,
	NULL,
	vdev_geom_hold,
	vdev_geom_rele,
	VDEV_TYPE_DISK,		/* name of this vdev type */
	B_TRUE			/* leaf vdev */
};
