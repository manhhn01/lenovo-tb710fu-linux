// drmoff: leave the display in a clean "off" state before Android's composer takes it back.
// Takes DRM master, disables every plane on every CRTC, detaches every connector and
// turns every CRTC off in one atomic commit. Prints what it did. Safe to run when nothing
// is active. Exit 0 = commit ok (or nothing to do), 1 = failure.
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

static uint32_t prop(int fd, uint32_t obj, uint32_t type, const char *name) {
	drmModeObjectProperties *p = drmModeObjectGetProperties(fd, obj, type);
	uint32_t id = 0;
	for (uint32_t i = 0; p && i < p->count_props; i++) {
		drmModePropertyRes *r = drmModeGetProperty(fd, p->props[i]);
		if (r && !strcmp(r->name, name)) id = r->prop_id;
		drmModeFreeProperty(r);
	}
	drmModeFreeObjectProperties(p);
	return id;
}

int main(void) {
	int fd = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
	if (fd < 0) { perror("open card0"); return 1; }
	if (drmSetMaster(fd)) perror("drmSetMaster (continuing)");
	drmSetClientCap(fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1);
	if (drmSetClientCap(fd, DRM_CLIENT_CAP_ATOMIC, 1)) { perror("atomic cap"); return 1; }
	drmModeRes *res = drmModeGetResources(fd);
	drmModePlaneRes *pr = drmModeGetPlaneResources(fd);
	if (!res || !pr) { fprintf(stderr, "no resources\n"); return 1; }
	/* Only clean up a state Linux set (our strip planes). Turning off the state Android's composer
	 * left behind hangs the SoC on this SDE driver; the composer takes that back by itself. */
	static const uint32_t ours[] = { 92, 127, 131, 143, 147, 151, 155 };   // 92 = USB-C output (LSW_DP)
	for (uint32_t i = 0; i < pr->count_planes; i++) {
		drmModePlane *p = drmModeGetPlane(fd, pr->planes[i]);
		if (p && (p->crtc_id || p->fb_id)) {
			int mine = 0;
			for (unsigned k = 0; k < sizeof ours / sizeof *ours; k++) if (p->plane_id == ours[k]) mine = 1;
			if (!mine) { printf("plane %u holds Android's state: leaving the display alone\n", p->plane_id); return 0; }
		}
		drmModeFreePlane(p);
	}
	drmModeAtomicReq *q = drmModeAtomicAlloc();
	int n = 0;
	for (uint32_t i = 0; i < pr->count_planes; i++) {
		drmModePlane *p = drmModeGetPlane(fd, pr->planes[i]);
		if (p && (p->crtc_id || p->fb_id)) {
			drmModeAtomicAddProperty(q, p->plane_id, prop(fd, p->plane_id, DRM_MODE_OBJECT_PLANE, "FB_ID"), 0);
			drmModeAtomicAddProperty(q, p->plane_id, prop(fd, p->plane_id, DRM_MODE_OBJECT_PLANE, "CRTC_ID"), 0);
			printf("plane %u off\n", p->plane_id); n++;
		}
		drmModeFreePlane(p);
	}
	for (int i = 0; i < res->count_connectors; i++) {
		drmModeConnector *c = drmModeGetConnector(fd, res->connectors[i]);
		if (c && c->encoder_id) {
			drmModeAtomicAddProperty(q, c->connector_id, prop(fd, c->connector_id, DRM_MODE_OBJECT_CONNECTOR, "CRTC_ID"), 0);
			printf("connector %u detached\n", c->connector_id); n++;
		}
		drmModeFreeConnector(c);
	}
	for (int i = 0; i < res->count_crtcs; i++) {
		drmModeCrtc *c = drmModeGetCrtc(fd, res->crtcs[i]);
		if (c && c->mode_valid) {
			drmModeAtomicAddProperty(q, c->crtc_id, prop(fd, c->crtc_id, DRM_MODE_OBJECT_CRTC, "ACTIVE"), 0);
			drmModeAtomicAddProperty(q, c->crtc_id, prop(fd, c->crtc_id, DRM_MODE_OBJECT_CRTC, "MODE_ID"), 0);
			printf("crtc %u off\n", c->crtc_id); n++;
		}
		drmModeFreeCrtc(c);
	}
	if (!n) { printf("display already off\n"); return 0; }
	int r = drmModeAtomicCommit(fd, q, DRM_MODE_ATOMIC_ALLOW_MODESET, NULL);
	printf("atomic off commit: %d (%s)\n", r, r ? strerror(-r) : "ok");
	return r ? 1 : 0;
}
