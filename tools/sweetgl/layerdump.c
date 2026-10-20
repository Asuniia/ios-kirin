/*
 * Research aid, loaded into SpringBoard (sweetenv SpringBoard:...): every
 * 30 s from 60 s on, the layers of all windows that carry filters, a
 * compositing filter or a material/backdrop class, with their view, frame,
 * colour, opacity and visibility. To see what CA is asked to draw for the
 * CC toggle circles and App Library plates that its GL renderer drops.
 */
#include <CoreFoundation/CoreFoundation.h>
#include <dispatch/dispatch.h>
#include <objc/message.h>
#include <objc/runtime.h>
#include <stdio.h>
#include <string.h>

#include "sweet_drm.h"

typedef struct { double x, y, w, h; } rect_t;

static SEL S(const char *n) { return sel_registerName(n); }
static id call(id o, const char *s) { return o ? ((id (*)(id, SEL))objc_msgSend)(o, S(s)) : nil; }
static unsigned long count(id a) { return a ? ((unsigned long (*)(id, SEL))objc_msgSend)(a, S("count")) : 0; }
static id at(id a, unsigned long i) { return ((id (*)(id, SEL, unsigned long))objc_msgSend)(a, S("objectAtIndex:"), i); }

static const char *
desc(id o, char *buf, size_t len)
{
	buf[0] = 0;
	if (o == nil) {
		return "-";
	}
	CFStringRef d = CFCopyDescription((CFTypeRef)o);
	if (d != NULL) {
		CFStringGetCString(d, buf, (CFIndex)len, kCFStringEncodingUTF8);
		CFRelease(d);
	}
	for (char *c = buf; *c; c++) {
		if (*c == '\n') {
			*c = ' ';
		}
	}
	return buf;
}

static int lines;

static void
walk(id layer, int depth)
{
	if (layer == nil || lines > 600) {
		return;
	}
	id cf = call(layer, "compositingFilter"), filters = call(layer, "filters");
	const char *cls = object_getClassName(layer);
	id delegate = call(layer, "delegate");
	const char *view = delegate ? object_getClassName(delegate) : "-";
	bool interesting = cf != nil || count(filters) > 0 || strstr(cls, "Backdrop") || strstr(cls, "Material") ||
	    strstr(view, "Material") || strstr(view, "RoundButton") || strstr(view, "Pod") || strstr(view, "Visual");
	if (interesting) {
		rect_t b = ((rect_t (*)(id, SEL))objc_msgSend)(layer, S("bounds"));
		rect_t r = ((rect_t (*)(id, SEL, rect_t, id))objc_msgSend)(layer, S("convertRect:toLayer:"), b, nil);
		float op = ((float (*)(id, SEL))objc_msgSend)(layer, S("opacity"));
		BOOL hid = ((BOOL (*)(id, SEL))objc_msgSend)(layer, S("isHidden"));
		double rad = ((double (*)(id, SEL))objc_msgSend)(layer, S("cornerRadius"));
		const void *bg = ((const void *(*)(id, SEL))objc_msgSend)(layer, S("backgroundColor"));
		char b1[200], b2[300], b3[160];
		sweet_drm_log("sweetgl: L%d %s (%s) %.0f,%.0f %.0fx%.0f op %.2f hid %d rad %.1f mask %d contents %d bg %.150s cf %.150s",
		    depth, cls, view, r.x, r.y, r.w, r.h, op, hid, rad, call(layer, "mask") != nil,
		    call(layer, "contents") != nil, desc((id)bg, b3, sizeof(b3)), desc(cf, b1, sizeof(b1)));
		for (unsigned long i = 0; i < count(filters) && i < 4; i++) {
			sweet_drm_log("sweetgl:   filter %lu: %.300s", i, desc(at(filters, i), b2, sizeof(b2)));
		}
		lines++;
	}
	id subs = call(layer, "sublayers");
	for (unsigned long i = 0; i < count(subs); i++) {
		walk(at(subs, i), depth + 1);
	}
}

static void
dump(void *ctx)
{
	static int round;
	Class w = objc_getClass("UIWindow");
	id wins = w ? ((id (*)(id, SEL, BOOL, BOOL))objc_msgSend)((id)w, S("allWindowsIncludingInternalWindows:onlyVisibleWindows:"),
	    YES, YES) : nil;
	lines = 0;
	sweet_drm_log("sweetgl: layer dump %d: %lu windows", round, count(wins));
	for (unsigned long i = 0; i < count(wins); i++) {
		id win = at(wins, i);
		sweet_drm_log("sweetgl: window %s", object_getClassName(win));
		walk(call(win, "layer"), 0);
	}
	if (++round < 8) {
		dispatch_after_f(dispatch_time(DISPATCH_TIME_NOW, 30 * NSEC_PER_SEC), dispatch_get_main_queue(), NULL, dump);
	}
	(void)ctx;
}

void
sweetgl_layerdump_start(void)
{
	dispatch_after_f(dispatch_time(DISPATCH_TIME_NOW, 60 * NSEC_PER_SEC), dispatch_get_main_queue(), NULL, dump);
}
