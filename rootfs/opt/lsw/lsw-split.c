// lsw-split.so: LD_PRELOAD shim for Qualcomm SDE "plane layout" split.
// SDE rejects any plane whose destination crosses the layout split (hdisplay/2).
// The compositor sees only one primary plane (LSW_PLANE_L). When it is placed across
// the split, the shim adds a hidden companion plane (LSW_PLANE_R) that shows the right
// part of the same framebuffer, and trims the left plane to the left part.
// Env: LSW_PLANE_L (default 127), LSW_PLANE_R (131), LSW_SPLIT (1600), LSW_DEBUG=1.
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <pthread.h>
#include <time.h>
#include <poll.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

/* libdrm's private request layout (stable since 2015, xf86drmMode.c) */
typedef struct { uint32_t object_id, property_id; uint64_t value; uint32_t cursor; } item_t;
struct _drmModeAtomicReq { uint32_t cursor; uint32_t size_items; item_t *items; };

enum { P_FB, P_CRTC, P_SX, P_SY, P_SW, P_SH, P_CX, P_CY, P_CW, P_CH, P_N };
static const char *pname[P_N] = { "FB_ID", "CRTC_ID", "SRC_X", "SRC_Y", "SRC_W", "SRC_H",
                                  "CRTC_X", "CRTC_Y", "CRTC_W", "CRTC_H" };
static uint32_t planeL = 127, planeR = 131, split = 1600;
static int dbg, inited;
static uint32_t idL[P_N], idR[P_N];
/* strip planes: the screen (width 2*split) is cut into ns equal vertical strips, one hw
 * plane each (left to right). planeL stays the only plane the compositor sees ("virtual"). */
#define MAXS 8
static int ns; static uint32_t strip[MAXS], idS[MAXS][P_N]; static int virt_in_strips;
static uint32_t fenceL, fenceS[MAXS];  /* IN_FENCE_FD prop ids */
static uint32_t name_id(int fd, uint32_t plane, const char *name) {
	uint32_t id = 0; drmModeObjectPropertiesPtr p = drmModeObjectGetProperties(fd, plane, DRM_MODE_OBJECT_PLANE);
	for (uint32_t i = 0; p && i < p->count_props; i++) { drmModePropertyPtr r = drmModeGetProperty(fd, p->props[i]); if (r && !strcmp(r->name, name)) id = r->prop_id; drmModeFreeProperty(r); }
	drmModeFreeObjectProperties(p); return id;
}
/* fill out[i][P_N] for each strip from the virtual plane state st */
static void strip_state(const uint64_t *st, uint64_t out[MAXS][P_N]) {
	int64_t W = 2 * (int64_t)split, cx = (int32_t)st[P_CX], cw = st[P_CW];
	int on = st[P_FB] && st[P_CRTC] && cw > 0;
	for (int i = 0; i < ns; i++) {
		memset(out[i], 0, sizeof out[i]);
		int64_t lo = W * i / ns, hi = W * (i + 1) / ns;
		int64_t a = cx > lo ? cx : lo, b = cx + cw < hi ? cx + cw : hi;
		if (!on || a >= b) continue;
		uint64_t sa = ((uint64_t)st[P_SW] * (uint64_t)(a - cx) / (uint64_t)cw) & ~0xffffULL;
		uint64_t sb = (b == cx + cw) ? st[P_SW] : (((uint64_t)st[P_SW] * (uint64_t)(b - cx) / (uint64_t)cw) & ~0xffffULL);
		uint64_t v[P_N] = { st[P_FB], st[P_CRTC], st[P_SX] + sa, st[P_SY], sb - sa, st[P_SH],
		                    (uint64_t)a, st[P_CY], (uint64_t)(b - a), st[P_CH] };
		memcpy(out[i], v, sizeof v);
	}
}
static uint64_t stL[P_N];          /* last committed state of the left plane */
static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;

static drmModePlaneResPtr (*real_getres)(int);
static int (*real_commit)(int, drmModeAtomicReqPtr, uint32_t, void *);
static drmModeAtomicReqPtr (*real_dup)(drmModeAtomicReqPtr);
static int (*real_add)(drmModeAtomicReqPtr, uint32_t, uint32_t, uint64_t);

#define LOG(...) do { if (dbg) { fprintf(stderr, "lsw-split: " __VA_ARGS__); fputc('\n', stderr); } } while (0)

static void load_ids(int fd, uint32_t plane, uint32_t *ids) {
	drmModeObjectPropertiesPtr p = drmModeObjectGetProperties(fd, plane, DRM_MODE_OBJECT_PLANE);
	for (uint32_t i = 0; p && i < p->count_props; i++) {
		drmModePropertyPtr r = drmModeGetProperty(fd, p->props[i]);
		for (int k = 0; r && k < P_N; k++) if (!strcmp(r->name, pname[k])) ids[k] = r->prop_id;
		drmModeFreeProperty(r);
	}
	drmModeFreeObjectProperties(p);
}

static void init(int fd) {
	if (inited) return;
	inited = 1;
	if (getenv("LSW_PLANE_L")) planeL = atoi(getenv("LSW_PLANE_L"));
	if (getenv("LSW_PLANE_R")) planeR = atoi(getenv("LSW_PLANE_R"));
	if (getenv("LSW_SPLIT")) split = atoi(getenv("LSW_SPLIT"));
	dbg = getenv("LSW_DEBUG") != NULL;
	load_ids(fd, planeL, idL);
	load_ids(fd, planeR, idR);
	{ const char *e = getenv("LSW_STRIPS"); char buf[128];
	  snprintf(buf, sizeof buf, "%s", e ? e : "155,151,147,143");
	  for (char *t = strtok(buf, ","); t && ns < MAXS; t = strtok(NULL, ",")) strip[ns++] = atoi(t);
	  fenceL = name_id(fd, planeL, "IN_FENCE_FD");
	  for (int i = 0; i < ns; i++) { load_ids(fd, strip[i], idS[i]); fenceS[i] = name_id(fd, strip[i], "IN_FENCE_FD"); if (strip[i] == planeL) virt_in_strips = 1; }
	  LOG("fence prop ids: virtual %u strip0 %u", fenceL, ns ? fenceS[0] : 0);
	  if (getenv("LSW_DEBUG")) { fprintf(stderr, "lsw-split: %d strips:", ns); for (int i = 0; i < ns; i++) fprintf(stderr, " %u", strip[i]); fprintf(stderr, " (virtual %u)\n", planeL); } }
	LOG("planeL=%u planeR=%u split=%u", planeL, planeR, split);
}

static void syms(void) {
	if (real_commit) return;
	real_getres = dlsym(RTLD_NEXT, "drmModeGetPlaneResources");
	real_commit = dlsym(RTLD_NEXT, "drmModeAtomicCommit");
	real_dup = dlsym(RTLD_NEXT, "drmModeAtomicDuplicate");
	real_add = dlsym(RTLD_NEXT, "drmModeAtomicAddProperty");
}

/* LSW_DP=1: also show the USB-C/HDMI output (DP-1) to the compositor. It needs none of the panel's
 * strips: one plane, normal commits Ids can be overridden for other builds. */
static int dp_on(void) { static int v = -1; if (v < 0) v = getenv("LSW_DP") != NULL; return v; }
static uint32_t env_u32(const char *n, uint32_t d) { const char *e = getenv(n); return e ? (uint32_t)atoi(e) : d; }
static uint32_t dp_crtc(void)  { return env_u32("LSW_DP_CRTC", 265); }
static uint32_t dp_conn(void)  { return env_u32("LSW_DP_CONN", 84); }
static uint32_t dp_plane(void) { return env_u32("LSW_DP_PLANE", 92); }

/* Only expose the left primary plane, so the compositor never uses overlay/cursor
 * planes that could cross the split, and never touches the companion plane. */
drmModePlaneResPtr drmModeGetPlaneResources(int fd) {
	syms();
	drmModePlaneResPtr r = real_getres(fd);
	if (!r) return r;
	int found = 0;
	for (uint32_t i = 0; i < r->count_planes; i++) if (r->planes[i] == planeL) found = 1;
	if (found) {
		int dp = 0;
		for (uint32_t i = 0; dp_on() && i < r->count_planes; i++) if (r->planes[i] == dp_plane()) dp = 1;
		r->planes[0] = planeL; r->count_planes = 1;
		if (dp) { r->planes[1] = dp_plane(); r->count_planes = 2; }
	}
	return r;
}

/* Every plane reports every CRTC as possible; tie each exposed plane to its own output so the
 * compositor cannot put the panel's virtual plane on DP-1 or the other way round. */
drmModePlanePtr drmModeGetPlane(int fd, uint32_t id) {
	static drmModePlanePtr (*real)(int, uint32_t);
	if (!real) real = dlsym(RTLD_NEXT, "drmModeGetPlane");
	drmModePlanePtr p = real(fd, id);
	if (p && dp_on()) {
		if (id == planeL) p->possible_crtcs = 1;
		else if (id == dp_plane()) p->possible_crtcs = 2;
	}
	return p;
}

/* Only expose the panel: CRTC LSW_CRTC (must be index 0, so encoder/plane
 * possible_crtcs bitmasks stay valid) and connector LSW_CONN. Virtual writeback
 * connectors report "connected" and make wlroots try (and fail) modesets on them. */
static drmModeResPtr (*real_getres2)(int);
drmModeResPtr drmModeGetResources(int fd) {
	if (!real_getres2) real_getres2 = dlsym(RTLD_NEXT, "drmModeGetResources");
	drmModeResPtr r = real_getres2(fd);
	if (!r) return r;
	uint32_t crtc = getenv("LSW_CRTC") ? atoi(getenv("LSW_CRTC")) : 200;
	uint32_t conn = getenv("LSW_CONN") ? atoi(getenv("LSW_CONN")) : 67;
	int dp_c = dp_on() && r->count_crtcs > 1 && r->crtcs[1] == dp_crtc(), dp_k = 0;
	for (int i = 0; dp_c && i < r->count_connectors; i++) if (r->connectors[i] == dp_conn()) dp_k = 1;
	if (r->count_crtcs > 0 && r->crtcs[0] == crtc) r->count_crtcs = dp_k ? 2 : 1;
	for (int i = 0; i < r->count_connectors; i++)
		if (r->connectors[i] == conn) {
			r->connectors[0] = conn; r->count_connectors = 1;
			if (dp_k) { r->connectors[1] = dp_conn(); r->count_connectors = 2; }
			break;
		}
	return r;
}


/* ---- debug: dump the framebuffer being scanned out (LSW_DUMP=<frame number>) ---- */
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <linux/dma-buf.h>
#include <unistd.h>
static void dump_fb(int fd, uint32_t fb_id) {
	drmModeFB2Ptr fb = drmModeGetFB2(fd, fb_id);
	if (!fb) { fprintf(stderr, "lsw-dump: GetFB2 failed\n"); return; }
	int dfd = -1;
	if (drmPrimeHandleToFD(fd, fb->handles[0], DRM_CLOEXEC | DRM_RDWR, &dfd) || dfd < 0) {
		fprintf(stderr, "lsw-dump: prime export failed\n"); drmModeFreeFB2(fb); return; }
	size_t sz = (size_t)fb->pitches[0] * fb->height;
	uint8_t *m = mmap(NULL, sz, PROT_READ, MAP_SHARED, dfd, 0);
	if (m == MAP_FAILED) { fprintf(stderr, "lsw-dump: mmap failed\n"); close(dfd); drmModeFreeFB2(fb); return; }
	struct dma_buf_sync sy = { DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ };
	ioctl(dfd, DMA_BUF_IOCTL_SYNC, &sy);
	FILE *f = fopen("/tmp/lsw-fb.ppm", "wb");
	int W = fb->width / 4, H = fb->height / 4;
	if (f) fprintf(f, "P6\n%d %d\n255\n", W, H);
	unsigned long long sum = 0;
	for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
		uint32_t px = *(uint32_t *)(m + (size_t)(y * 4) * fb->pitches[0] + (size_t)(x * 4) * 4);
		uint8_t rgb[3] = { (px >> 16) & 255, (px >> 8) & 255, px & 255 };
		sum += px;
		if (f) fwrite(rgb, 1, 3, f);
	}
	if (f) fclose(f);
	sy.flags = DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ;
	ioctl(dfd, DMA_BUF_IOCTL_SYNC, &sy);
	fprintf(stderr, "lsw-dump: fb %u %ux%u pitch %u mod 0x%llx sum %llu -> /tmp/lsw-fb.ppm\n", fb_id, fb->width, fb->height,
	        fb->pitches[0], (unsigned long long)fb->modifier, sum);
	munmap(m, sz); close(dfd); drmModeFreeFB2(fb);
}

/* ---- mode control: log the mode a compositor asks for; LSW_FORCE_PREFERRED=1 swaps it for the panel's
 * preferred mode (3200x2000@120, what Android uses). Hypothesis: other modes leave the DSC panel in a bad
 * state that survives until reboot (rainbow noise). ---- */
static uint32_t mode_prop_id(int fd, uint32_t crtc) {
	static uint32_t id; if (id) return id;
	drmModeObjectPropertiesPtr p = drmModeObjectGetProperties(fd, crtc, DRM_MODE_OBJECT_CRTC);
	for (uint32_t i = 0; p && i < p->count_props; i++) {
		drmModePropertyPtr r = drmModeGetProperty(fd, p->props[i]);
		if (r && !strcmp(r->name, "MODE_ID")) id = r->prop_id;
		drmModeFreeProperty(r);
	}
	drmModeFreeObjectProperties(p);
	return id;
}
static uint64_t fix_mode(int fd, uint64_t blob_id) {
	if (!blob_id) return blob_id;
	drmModePropertyBlobPtr b = drmModeGetPropertyBlob(fd, (uint32_t)blob_id);
	if (!b) return blob_id;
	drmModeModeInfo m; memcpy(&m, b->data, sizeof m < b->length ? sizeof m : b->length);
	drmModeFreePropertyBlob(b);
	static uint64_t last; if (blob_id != last) { fprintf(stderr, "lsw-mode: compositor asks %s (%u Hz)\n", m.name, m.vrefresh); last = blob_id; }
	if (!getenv("LSW_FORCE_PREFERRED")) return blob_id;
	static uint32_t pref_blob;
	if (!pref_blob) {
		uint32_t conn = getenv("LSW_CONN") ? atoi(getenv("LSW_CONN")) : 67;
		drmModeConnectorPtr c = drmModeGetConnector(fd, conn);
		for (int i = 0; c && i < c->count_modes; i++)
			if (c->modes[i].type & DRM_MODE_TYPE_PREFERRED) {
				drmModeCreatePropertyBlob(fd, &c->modes[i], sizeof c->modes[i], &pref_blob);
				fprintf(stderr, "lsw-mode: forcing preferred %s (%u Hz) blob %u\n", c->modes[i].name, c->modes[i].vrefresh, pref_blob);
			}
		drmModeFreeConnector(c);
	}
	if (pref_blob && m.vrefresh != 120) return pref_blob;
	return blob_id;
}

int drmModeAtomicCommit(int fd, drmModeAtomicReqPtr req, uint32_t flags, void *user) {
	syms();
	pthread_mutex_lock(&mu);
	init(fd);
	{ /* LSW_PROPS: log every property of the first non-test commits */
		static int logged;
		if (getenv("LSW_PROPS") && !(flags & DRM_MODE_ATOMIC_TEST_ONLY) && logged < 3) {
			logged++;
			fprintf(stderr, "lsw-props: commit %d flags 0x%x items %u\n", logged, flags, req->cursor);
			for (uint32_t i = 0; i < req->cursor; i++) {
				drmModePropertyPtr pr = drmModeGetProperty(fd, req->items[i].property_id);
				fprintf(stderr, "lsw-props:   obj %u %s = %llu\n", req->items[i].object_id, pr ? pr->name : "?",
				        (unsigned long long)req->items[i].value);
				drmModeFreeProperty(pr);
			}
		}
	}
	{ uint32_t crtc = getenv("LSW_CRTC") ? atoi(getenv("LSW_CRTC")) : 200, mid = mode_prop_id(fd, crtc);
	  for (uint32_t i = 0; i < req->cursor; i++)
		if (req->items[i].object_id == crtc && req->items[i].property_id == mid) req->items[i].value = fix_mode(fd, req->items[i].value); }
	uint64_t st[P_N];
	memcpy(st, stL, sizeof st);
	int touched = 0;
	for (uint32_t i = 0; i < req->cursor; i++) {
		item_t *it = &req->items[i];
		if (it->object_id != planeL) continue;
		for (int k = 0; k < P_N; k++) if (it->property_id == idL[k]) { st[k] = it->value; touched = 1; }
	}
	drmModeAtomicReqPtr q = req;
	if (touched) {
		q = real_dup(req);
		uint64_t out[MAXS][P_N];
		strip_state(st, out);
		int64_t fence = -1;
		for (uint32_t i = 0; i < req->cursor; i++)
			if (req->items[i].object_id == planeL && fenceL && req->items[i].property_id == fenceL) fence = (int64_t)req->items[i].value;
		if (fence >= 0 && !getenv("LSW_FENCE_STRIPS")) { struct pollfd pf = { (int)fence, POLLIN, 0 }; poll(&pf, 1, 200); }
		if (fenceL && fence >= 0) real_add(q, planeL, fenceL, (uint64_t)-1);
		if (!virt_in_strips) { real_add(q, planeL, idL[P_FB], 0); real_add(q, planeL, idL[P_CRTC], 0); }
		for (int i = 0; i < ns; i++) {
			for (int k = 0; k < P_N; k++) real_add(q, strip[i], idS[i][k], out[i][k]);
			if (fence >= 0 && out[i][P_FB] && fenceS[i] && getenv("LSW_FENCE_STRIPS")) real_add(q, strip[i], fenceS[i], (uint64_t)fence);
		}
		LOG("strips fb=%llu dst %lld+%llu", (unsigned long long)st[P_FB], (long long)(int32_t)st[P_CX], (unsigned long long)st[P_CW]);
	}
	int ret = real_commit(fd, q, flags, user);
	if (q != req) drmModeAtomicFree(q);
	if (ret == 0 && !(flags & DRM_MODE_ATOMIC_TEST_ONLY)) {
		memcpy(stL, st, sizeof st);
		static long sec0; static int n;
		struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
		if (!sec0) sec0 = ts.tv_sec;
		if (ts.tv_sec != sec0) {
			if (getenv("LSW_STATS")) { fprintf(stderr, "lsw-fps %ld %d\n", (long)ts.tv_sec, n); fflush(stderr); }
			sec0 = ts.tv_sec; n = 0;
		}
		n++;
		static int total; total++;
		if (getenv("LSW_DUMP") && total == atoi(getenv("LSW_DUMP")) && st[P_FB]) dump_fb(fd, (uint32_t)st[P_FB]);
	}
	if (ret) LOG("commit flags=0x%x -> %d", flags, ret);
	pthread_mutex_unlock(&mu);
	return ret;
}

/* ---- GBM: scanout buffers must be LINEAR ----
 * SDE only advertises DRM_FORMAT_MOD_LINEAR. aquamarine 0.15 drops LINEAR when the
 * renderer (Zink) doesn't list it as renderable and falls back to a modifier-less
 * allocation, so Turnip picks a tiled layout and the panel shows garbage. */
#include <gbm.h>
static int scanout_count;
struct gbm_bo *gbm_bo_create(struct gbm_device *d, uint32_t w, uint32_t h, uint32_t fmt, uint32_t flags) {
	static struct gbm_bo *(*real)(struct gbm_device *, uint32_t, uint32_t, uint32_t, uint32_t);
	if (!real) real = dlsym(RTLD_NEXT, "gbm_bo_create");
	if (getenv("LSW_NO_GBM_HOOKS")) return real(d, w, h, fmt, flags);
	if (flags & GBM_BO_USE_SCANOUT) {
		if (getenv("LSW_DEBUG") && scanout_count++ < 8) fprintf(stderr, "lsw-split: gbm_bo_create %ux%u scanout (implicit layout)\n", w, h);
	}
	return real(d, w, h, fmt, flags);
}
struct gbm_bo *gbm_bo_create_with_modifiers2(struct gbm_device *d, uint32_t w, uint32_t h, uint32_t fmt,
                                             const uint64_t *mods, const unsigned int count, uint32_t flags) {
	static struct gbm_bo *(*real)(struct gbm_device *, uint32_t, uint32_t, uint32_t, const uint64_t *, const unsigned int, uint32_t);
	if (!real) real = dlsym(RTLD_NEXT, "gbm_bo_create_with_modifiers2");
	if (getenv("LSW_NO_GBM_HOOKS")) return real(d, w, h, fmt, mods, count, flags);
	if (flags & GBM_BO_USE_SCANOUT) {
		static const uint64_t lin = 0; /* DRM_FORMAT_MOD_LINEAR */
		if (getenv("LSW_DEBUG") && scanout_count++ < 8) {
			fprintf(stderr, "lsw-split: gbm_bo_create_with_modifiers2 %ux%u fmt %.4s scanout mods:", w, h, (char *)&fmt);
			for (unsigned i = 0; i < count; i++) fprintf(stderr, " 0x%llx", (unsigned long long)mods[i]);
			fprintf(stderr, " -> %s\n", getenv("LSW_GBM_MODE") && *getenv("LSW_GBM_MODE") == 'B' ? "explicit LINEAR" : "implicit gbm_bo_create");
		}
		if (getenv("LSW_KEEP_MODS")) return real(d, w, h, fmt, mods, count, flags);
		const char *mode = getenv("LSW_GBM_MODE");
		if (!(mode && *mode == 'B')) return gbm_bo_create(d, w, h, fmt, flags | GBM_BO_USE_RENDERING); /* implicit, like wlroots */
		return real(d, w, h, fmt, &lin, 1, flags);
	}
	return real(d, w, h, fmt, mods, count, flags);
}

/* log every framebuffer handed to the display (format, pitch, modifier) */
int drmModeAddFB2WithModifiers(int fd, uint32_t w, uint32_t h, uint32_t fmt, const uint32_t bo[4], const uint32_t pitches[4],
                               const uint32_t offsets[4], const uint64_t modifier[4], uint32_t *id, uint32_t flags) {
	static int (*real)(int, uint32_t, uint32_t, uint32_t, const uint32_t[4], const uint32_t[4], const uint32_t[4], const uint64_t[4], uint32_t *, uint32_t);
	static int n;
	if (!real) real = dlsym(RTLD_NEXT, "drmModeAddFB2WithModifiers");
	/* SDE misreads LINEAR when DRM_MODE_FB_MODIFIERS is set (rainbow tiles on the panel).
	 * Drop the flag for LINEAR buffers, which is what wlroots does with WLR_DRM_NO_MODIFIERS. */
	const char *mode = getenv("LSW_FB_MODE");
	if (!getenv("LSW_NO_GBM_HOOKS") && (flags & DRM_MODE_FB_MODIFIERS) && modifier && modifier[0] == 0 && !(mode && *mode == 'B')) {
		flags &= ~DRM_MODE_FB_MODIFIERS;
		modifier = NULL;
	}
	int r = real(fd, w, h, fmt, bo, pitches, offsets, modifier, id, flags);
	if (getenv("LSW_DEBUG") && n++ < 12)
		fprintf(stderr, "lsw-split: addfb2 %ux%u fmt %.4s pitch %u off %u mod 0x%llx flags 0x%x -> fb %u r=%d\n", w, h, (char *)&fmt, pitches[0], offsets[0],
		        modifier ? (unsigned long long)modifier[0] : 0ULL, flags, id ? *id : 0, r);
	return r;
}
int drmModeAddFB2(int fd, uint32_t w, uint32_t h, uint32_t fmt, const uint32_t bo[4], const uint32_t pitches[4],
                  const uint32_t offsets[4], uint32_t *id, uint32_t flags) {
	static int (*real)(int, uint32_t, uint32_t, uint32_t, const uint32_t[4], const uint32_t[4], const uint32_t[4], uint32_t *, uint32_t);
	static int n;
	if (!real) real = dlsym(RTLD_NEXT, "drmModeAddFB2");
	int r = real(fd, w, h, fmt, bo, pitches, offsets, id, flags);
	if (getenv("LSW_DEBUG") && n++ < 12)
		fprintf(stderr, "lsw-split: addfb2(nomod) %ux%u fmt %.4s pitch %u -> fb %u r=%d\n", w, h, (char *)&fmt, pitches[0], id ? *id : 0, r);
	return r;
}

/* ---- raw DRM_IOCTL_MODE_ATOMIC path (KWin builds its own request and calls drmIoctl) ---- */
#include <drm.h>
#include <drm_mode.h>
#include <poll.h>

/* LSW_CLOG=<file>: append every non-test atomic request (after rewriting) with property names, O_SYNC */
static void clog(int fd, const char *tag, uint32_t flags, uint32_t no, const uint32_t *objs, const uint32_t *cnt,
                 const uint32_t *props, const uint64_t *vals, int ret) {
	static int lf = -2, n;
	if (lf == -2) { const char *f = getenv("LSW_CLOG"); lf = f ? open(f, O_WRONLY | O_CREAT | O_APPEND | O_SYNC, 0644) : -1; }
	static int seen; seen++;
	if (getenv("LSW_CLOG_FROM") && seen < atoi(getenv("LSW_CLOG_FROM"))) return;
	if (lf < 0 || n >= 400) return;
	if ((flags & DRM_MODE_ATOMIC_TEST_ONLY) && n > 40) return;
	n++;
	char b[16384]; int l = 0; struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
	l += snprintf(b + l, sizeof b - l, "== %s #%d t=%ld.%03ld flags=0x%x ret=%d\n", tag, n, (long)ts.tv_sec, ts.tv_nsec / 1000000, flags, ret);
	uint32_t t = 0;
	for (uint32_t o = 0; o < no && l < (int)sizeof b - 200; o++) {
		l += snprintf(b + l, sizeof b - l, " obj %u:", objs[o]);
		for (uint32_t k = 0; k < cnt[o] && l < (int)sizeof b - 100; k++) {
			drmModePropertyPtr pr = drmModeGetProperty(fd, props[t + k]);
			l += snprintf(b + l, sizeof b - l, " %s=%lld", pr ? pr->name : "?", (long long)vals[t + k]);
			drmModeFreeProperty(pr);
		}
		l += snprintf(b + l, sizeof b - l, "\n");
		t += cnt[o];
	}
	if (write(lf, b, l) < 0) {}
}

static uint32_t obj_prop(int fd, uint32_t obj, uint32_t type, const char *name) {
	uint32_t id = 0; drmModeObjectPropertiesPtr p = drmModeObjectGetProperties(fd, obj, type);
	for (uint32_t i = 0; p && i < p->count_props; i++) { drmModePropertyPtr r = drmModeGetProperty(fd, p->props[i]); if (r && !strcmp(r->name, name)) id = r->prop_id; drmModeFreeProperty(r); }
	drmModeFreeObjectProperties(p); return id;
}
static uint64_t obj_val(int fd, uint32_t obj, uint32_t type, uint32_t prop) {
	uint64_t v = 0; drmModeObjectPropertiesPtr p = drmModeObjectGetProperties(fd, obj, type);
	for (uint32_t i = 0; p && i < p->count_props; i++) if (p->props[i] == prop) v = p->prop_values[i];
	drmModeFreeObjectProperties(p); return v;
}
int drmIoctl(int fd, unsigned long request, void *arg) {
	static int (*real)(int, unsigned long, void *);
	if (!real) real = dlsym(RTLD_NEXT, "drmIoctl");
	if (request != DRM_IOCTL_MODE_ATOMIC || !arg) return real(fd, request, arg);
	struct drm_mode_atomic *a = arg;
	uint32_t *objs = (uint32_t *)(uintptr_t)a->objs_ptr, *cnt = (uint32_t *)(uintptr_t)a->count_props_ptr;
	uint32_t *props = (uint32_t *)(uintptr_t)a->props_ptr;
	uint64_t *vals = (uint64_t *)(uintptr_t)a->prop_values_ptr;
	pthread_mutex_lock(&mu);
	init(fd);
	{ uint32_t crtc = getenv("LSW_CRTC") ? atoi(getenv("LSW_CRTC")) : 200, mid = mode_prop_id(fd, crtc), t = 0;
	  for (uint32_t o = 0; o < a->count_objs; o++) {
		for (uint32_t k = 0; k < cnt[o]; k++) if (objs[o] == crtc && props[t + k] == mid) vals[t + k] = fix_mode(fd, vals[t + k]);
		t += cnt[o]; } }
	uint64_t st[P_N]; memcpy(st, stL, sizeof st);
	int touched = 0, has_r = 0, panel = !dp_on(); uint32_t total = 0;
	for (uint32_t o = 0; o < a->count_objs; o++) {
		for (int i = 0; i < ns; i++) if (objs[o] == strip[i] && strip[i] != planeL) has_r = 1;
		if (objs[o] == planeL || objs[o] == env_u32("LSW_CRTC", 200) || objs[o] == env_u32("LSW_CONN", 67)) panel = 1;
		for (uint32_t k = 0; k < cnt[o]; k++) {
			if (objs[o] == planeL)
				for (int j = 0; j < P_N; j++) if (props[total + k] == idL[j]) { st[j] = vals[total + k]; touched = 1; }
		}
		total += cnt[o];
	}
	{ /* once strips are live, every real commit must go the TWRP way, even ones that don't touch the plane */
	  /* commits for DP-1 alone are not the panel's business and go through unchanged */
	  if (panel && !touched && !has_r && stL[P_FB] && !(a->flags & DRM_MODE_ATOMIC_TEST_ONLY) && !getenv("LSW_PLAIN_FLIPS")) touched = 1; }
	if (!touched || has_r) {  /* nothing for us, or already split by the libdrm-level hook */
		pthread_mutex_unlock(&mu);
		int r0 = real(fd, request, arg);
		clog(fd, "pass", a->flags, a->count_objs, objs, cnt, props, vals, r0);
		return r0;
	}
	/* copy of the request without the virtual plane; strip planes appended with only the
	 * properties that changed since the last real commit (like a plain page flip) */
	static uint64_t lastS[MAXS][P_N]; static int haveS;
	uint32_t n = a->count_objs + ns + 2, tp = total + ns * P_N + 3;
	uint32_t *o2 = calloc(n, 4), *c2 = calloc(n, 4), *p2 = calloc(tp, 4);
	uint64_t *v2 = calloc(tp, 8);
	uint64_t out[MAXS][P_N];
	strip_state(st, out);
	uint32_t no = 0, np_ = 0;
	{ uint32_t t = 0;  /* the virtual plane's in-fence is dropped with it: wait for it here */
	  for (uint32_t o = 0; o < a->count_objs; o++) {
		for (uint32_t k = 0; k < cnt[o]; k++) if (objs[o] == planeL && fenceL && props[t + k] == fenceL && (int64_t)vals[t + k] >= 0) {
			struct pollfd pf = { (int)vals[t + k], POLLIN, 0 }; poll(&pf, 1, 200); }
		t += cnt[o]; } }
	/* TWRP style (default): every real commit carries the whole state with ALLOW_MODESET and is
	 * blocking. With 4 strip planes, plain page flips hang the SoC on this SDE driver. */
	int twrp = !getenv("LSW_PLAIN_FLIPS");
	int full = twrp || !haveS || (a->flags & DRM_MODE_ATOMIC_ALLOW_MODESET);
	static uint32_t pMode, pActive, pConnCrtc; static uint64_t curMode;
	uint32_t crtcId = getenv("LSW_CRTC") ? atoi(getenv("LSW_CRTC")) : 200, connId = getenv("LSW_CONN") ? atoi(getenv("LSW_CONN")) : 67;
	if (!pMode) { pMode = obj_prop(fd, crtcId, DRM_MODE_OBJECT_CRTC, "MODE_ID"); pActive = obj_prop(fd, crtcId, DRM_MODE_OBJECT_CRTC, "ACTIVE");
	              pConnCrtc = obj_prop(fd, connId, DRM_MODE_OBJECT_CONNECTOR, "CRTC_ID"); }
	int deact = 0;
	{ uint32_t t = 0;
	  for (uint32_t o = 0; o < a->count_objs; o++) {
		for (uint32_t k = 0; k < cnt[o]; k++) {
			if (objs[o] == crtcId && props[t + k] == pMode && vals[t + k]) curMode = vals[t + k];
			if (objs[o] == crtcId && props[t + k] == pActive && !vals[t + k]) deact = 1;
		}
		t += cnt[o]; } }
	/* Display off (DPMS): ACTIVE=0 needs every strip off too. Later commits that say nothing about
	 * ACTIVE must not turn the panel back on, so remember it until the compositor sets ACTIVE=1. */
	static int panel_off;
	{ uint32_t t = 0; int on = 0;
	  for (uint32_t o = 0; o < a->count_objs; o++) {
		for (uint32_t k = 0; k < cnt[o]; k++) if (objs[o] == crtcId && props[t + k] == pActive && vals[t + k]) on = 1;
		t += cnt[o]; }
	  if (on) deact = 0;
	  else if (panel_off) deact = 1; }
	if (deact) memset(out, 0, sizeof out);
	if (!curMode) curMode = obj_val(fd, crtcId, DRM_MODE_OBJECT_CRTC, pMode);
	{ uint32_t t = 0;
	  for (uint32_t o = 0; o < a->count_objs; o++) {
		if (objs[o] == planeL && !virt_in_strips) { t += cnt[o]; continue; }
		o2[no] = objs[o]; c2[no] = cnt[o];
		memcpy(p2 + np_, props + t, cnt[o] * 4); memcpy(v2 + np_, vals + t, cnt[o] * 8);
		np_ += cnt[o]; t += cnt[o]; no++;
	  } }
	for (int i = 0; i < ns; i++) {
		uint32_t c = 0;
		for (int j = 0; j < P_N; j++) if (full || out[i][j] != lastS[i][j]) { p2[np_ + c] = idS[i][j]; v2[np_ + c] = out[i][j]; c++; }
		if (!c) continue;
		o2[no] = strip[i]; c2[no] = c; no++; np_ += c;
	}
	uint32_t newflags = a->flags;
	if (twrp && !deact && curMode && st[P_FB]) {
		o2[no] = crtcId; c2[no] = 2; p2[np_] = pMode; v2[np_++] = curMode; p2[np_] = pActive; v2[np_++] = 1; no++;
		o2[no] = connId; c2[no] = 1; p2[np_] = pConnCrtc; v2[np_++] = crtcId; no++;
		newflags = (a->flags & ~DRM_MODE_ATOMIC_NONBLOCK) | DRM_MODE_ATOMIC_ALLOW_MODESET;
	}
	n = no;
	struct drm_mode_atomic a2 = *a;
	a2.count_objs = n; a2.objs_ptr = (uintptr_t)o2; a2.count_props_ptr = (uintptr_t)c2; a2.flags = newflags;
	a2.props_ptr = (uintptr_t)p2; a2.prop_values_ptr = (uintptr_t)v2;
	clog(fd, "pre", newflags, no, o2, c2, p2, v2, 0);
	int ret = real(fd, request, &a2);
	clog(fd, "done", a->flags, 0, o2, c2, p2, v2, ret);
	if (!(a->flags & DRM_MODE_ATOMIC_TEST_ONLY)) {
		if (deact != panel_off) {              /* survives a hang: O_SYNC in the rootfs */
			int lf = open("/var/lib/lsw/dpms.log", O_WRONLY | O_CREAT | O_APPEND | O_SYNC, 0644);
			char b[80]; int l = snprintf(b, sizeof b, "panel %s flags=0x%x ret=%d\n", deact ? "off" : "on", newflags, ret);
			if (lf >= 0) { if (write(lf, b, l) < 0) {} close(lf); }
		}
		if (ret == 0) panel_off = deact;
	}
	if (ret) LOG("ioctl commit flags=0x%x -> %d", a->flags, ret);
	if (ret == 0 && !(a->flags & DRM_MODE_ATOMIC_TEST_ONLY)) {
		memcpy(stL, st, sizeof st); memcpy(lastS, out, sizeof out); haveS = 1;
		static long t0, sec0; static int nf, dumped; struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
		if (!t0) t0 = sec0 = ts.tv_sec;
		nf++;
		if (ts.tv_sec != sec0) { if (getenv("LSW_STATS")) { fprintf(stderr, "lsw-fps %ld %d\n", (long)(ts.tv_sec - t0), nf); fflush(stderr); } sec0 = ts.tv_sec; nf = 0; }
		if (getenv("LSW_DUMP_SEC") && !dumped && ts.tv_sec - t0 >= atoi(getenv("LSW_DUMP_SEC")) && st[P_FB]) { dumped = 1; dump_fb(fd, (uint32_t)st[P_FB]); }
		static long lastdump; /* LSW_DUMP_EVERY: keep /tmp/lsw-fb.ppm = latest frame, at most once per second */
		if (getenv("LSW_DUMP_EVERY") && st[P_FB] && ts.tv_sec != lastdump) { lastdump = ts.tv_sec; dump_fb(fd, (uint32_t)st[P_FB]); }
	}
	free(o2); free(c2); free(p2); free(v2);
	pthread_mutex_unlock(&mu);
	return ret;
}

/* LSW_KWIN_NO_SYSBUS=1: inside kwin_wayland only, point the system bus at nothing so KWin does not
 * pick logind (it has no session for us there and would then open no input devices) and falls back
 * to its no-op session. The rest of Plasma keeps the real system bus. */
__attribute__((constructor)) static void lsw_kwin_bus(void) {
	if (!getenv("LSW_KWIN_NO_SYSBUS")) return;
	char c[32] = { 0 }; FILE *f = fopen("/proc/self/comm", "r");
	if (!f) return;
	if (fgets(c, sizeof c, f)) c[strcspn(c, "\n")] = 0;
	fclose(f);
	if (!strcmp(c, "kwin_wayland")) setenv("DBUS_SYSTEM_BUS_ADDRESS", "unix:path=/run/lsw-no-system-bus", 1);
}

/* LSW_GPU_KWIN=1: only KWin renders on the GPU. In kwin_wayland_wrapper (which execs kwin_wayland)
 * swap the session's software-rendering variables for the Zink/Turnip ones, so KWin and what it
 * spawns get the custom Mesa while every other client keeps the system Mesa (llvmpipe). */
__attribute__((constructor)) static void lsw_gpu_kwin(void) {
	if (!getenv("LSW_GPU_KWIN")) return;
	char c[32] = { 0 }; FILE *f = fopen("/proc/self/comm", "r");
	if (!f) return;
	if (fgets(c, sizeof c, f)) c[strcspn(c, "\n")] = 0;
	fclose(f);
	if (strcmp(c, "kwin_wayland_wr")) return;
	const char *Z = "/opt/mesa-zink/lib/aarch64-linux-gnu";
	char b[256];
	setenv("LD_LIBRARY_PATH", Z, 1);
	snprintf(b, sizeof b, "%s/dri", Z); setenv("LIBGL_DRIVERS_PATH", b, 1);
	snprintf(b, sizeof b, "%s/gbm", Z); setenv("GBM_BACKENDS_PATH", b, 1);
	setenv("VK_ICD_FILENAMES", "/opt/mesa-zink/share/vulkan/icd.d/freedreno_icd.aarch64.json", 1);
	setenv("MESA_LOADER_DRIVER_OVERRIDE", "zink", 1);
	setenv("TU_DEBUG", "noconform", 1);
	unsetenv("LIBGL_ALWAYS_SOFTWARE"); unsetenv("GALLIUM_DRIVER"); unsetenv("KWIN_COMPOSE");
}
