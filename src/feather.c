/*
 * FeatherPDF - a minimal-footprint PDF reader for Windows.
 *
 * Plain Win32 + MuPDF. Single file, no frameworks, no runtime dependencies.
 * Memory is kept low by:
 *   - rendering only the pages (or parts of pages) that are on screen,
 *   - a small, size-bounded bitmap cache (24-bit, ~3 screens worth),
 *   - a capped MuPDF resource store,
 *   - no back-buffer, no GPU/DirectX, no scripting engine.
 */
#define UNICODE
#define _UNICODE
#define WIN32_LEAN_AND_MEAN
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <math.h>

#include <mupdf/fitz.h>
#include "resource.h"

#define APP_NAME      L"FeatherPDF"
#define APP_VERSION   L"1.0.0"
#define MAIN_CLASS    L"FeatherPDFMain"
#define CANVAS_CLASS  L"FeatherPDFCanvas"

#define STORE_LIMIT   (32 << 20)   /* MuPDF resource cache cap (fonts/images) */
#define MAX_RECENT    10
#define MAX_HITS      1024
#define MAX_HISTORY   64
#define MAX_TILES     16

enum { ZM_CUSTOM, ZM_FIT_WIDTH, ZM_FIT_PAGE };

enum {
	IDM_OPEN = 1001, IDM_CLOSE, IDM_PRINT, IDM_RELOAD, IDM_PROPERTIES, IDM_EXIT,
	IDM_COPY, IDM_SELECTALL, IDM_FIND, IDM_FINDNEXT, IDM_FINDPREV,
	IDM_ZOOMIN, IDM_ZOOMOUT, IDM_FITPAGE, IDM_FITWIDTH, IDM_ACTUAL,
	IDM_ROTL, IDM_ROTR, IDM_OUTLINE, IDM_FULLSCREEN, IDM_DARK, IDM_HAND,
	IDM_NEXTPAGE, IDM_PREVPAGE, IDM_FIRSTPAGE, IDM_LASTPAGE, IDM_GOTO,
	IDM_BACK, IDM_FORWARD, IDM_SHORTCUTS, IDM_ABOUT,
	IDM_RECENT0 = 1100, /* .. IDM_RECENT0 + MAX_RECENT - 1 */
	IDM_RECENT_CLEAR = 1150,
	IDC_TREE = 2001, IDC_CANVAS, IDC_STATUS
};

/* ------------------------------------------------------------------ */
/* State                                                               */
/* ------------------------------------------------------------------ */

static HINSTANCE g_inst;
static HWND g_main, g_canvas, g_tree, g_status, g_find_dlg;
static HMENU g_menu, g_file_menu, g_recent_menu, g_view_menu;
static HACCEL g_accel;
static HFONT g_ui_font;

static fz_context *g_ctx;
static fz_document *g_doc;
static fz_outline *g_outline;
static wchar_t g_path[32768];
static int g_npages;
static fz_rect *g_bounds;     /* page bounds in points (unrotated) */
static int *g_page_y, *g_page_w, *g_page_h;   /* layout in device pixels */
static int g_doc_w, g_doc_h;
static int g_scroll_x, g_scroll_y;
static float g_zoom = 1.0f;
static int g_zoom_mode = ZM_FIT_WIDTH;
static int g_rotate;
static int g_dpi = 96;
static BOOL g_dark, g_sidebar = TRUE, g_fullscreen, g_hand;
static int g_sidebar_w = 230;   /* in 96-dpi units */
static WINDOWPLACEMENT g_saved_wp = { sizeof(WINDOWPLACEMENT) };
static LONG_PTR g_saved_style;

/* bitmap cache */
typedef struct {
	int page, rot, dark;
	float scale;
	RECT r;                 /* area of page (page pixel coords) in this tile */
	int stride;
	unsigned char *samples; /* 24-bit BGR, top-down, DWORD aligned rows */
	unsigned int used;
} Tile;
static Tile g_tiles[MAX_TILES];
static unsigned int g_tick;

/* text selection */
static int g_sel_page = -1;
static fz_point g_sel_a, g_sel_b;
static BOOL g_selecting, g_mouse_down, g_panning;
static POINT g_down_pt, g_pan_origin;
static int g_pan_sx, g_pan_sy;
static int g_stext_page = -1;
static fz_stext_page *g_stext;

/* links for the page under the mouse */
static int g_links_page = -1;
static fz_page *g_links_pg;
static fz_link *g_links;

/* search */
static char g_needle[1024];
static int g_hit_page = -1, g_hit_cur, g_nhits, g_nquads;
static fz_quad g_hit_quads[MAX_HITS];
static int g_hit_marks[MAX_HITS];
static int g_hit_start[MAX_HITS + 1];

/* navigation history */
typedef struct { int page; float y; } HistEntry;
static HistEntry g_hist[MAX_HISTORY];
static int g_hist_len, g_hist_pos;

/* settings */
static wchar_t g_ini[32768];
typedef struct { wchar_t *path; int page; } Recent;
static Recent g_recent[MAX_RECENT];
static int g_nrecent;

/* splitter drag */
static BOOL g_split_drag;

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

static int S(int v) { return MulDiv(v, g_dpi, 96); }
static float scale(void) { return g_zoom * g_dpi / 72.0f; }

static char *utf8_from_w(const wchar_t *w)
{
	int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
	char *s = malloc(n > 0 ? n : 1);
	if (!s) return NULL;
	if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w, -1, s, n, NULL, NULL); else s[0] = 0;
	return s;
}

static wchar_t *w_from_utf8(const char *s)
{
	int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
	wchar_t *w = malloc(sizeof(wchar_t) * (n > 0 ? n : 1));
	if (!w) return NULL;
	if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n); else w[0] = 0;
	return w;
}

static void error_box(const wchar_t *fmt, const char *detail)
{
	wchar_t buf[2048];
	wchar_t *wd = w_from_utf8(detail ? detail : "");
	_snwprintf(buf, 2047, fmt, wd ? wd : L"");
	buf[2047] = 0;
	free(wd);
	MessageBoxW(g_main, buf, APP_NAME, MB_ICONERROR | MB_OK);
}

static const wchar_t *base_name(const wchar_t *p)
{
	const wchar_t *b = p;
	for (; *p; p++) if (*p == L'\\' || *p == L'/') b = p + 1;
	return b;
}

static void client_size(int *w, int *h)
{
	RECT rc = { 0, 0, 0, 0 };
	if (g_canvas) GetClientRect(g_canvas, &rc);
	*w = rc.right; *h = rc.bottom;
}

/* ------------------------------------------------------------------ */
/* Geometry                                                            */
/* ------------------------------------------------------------------ */

static float page_w_pt(int i)
{
	fz_rect b = g_bounds[i];
	return (g_rotate % 180) ? b.y1 - b.y0 : b.x1 - b.x0;
}
static float page_h_pt(int i)
{
	fz_rect b = g_bounds[i];
	return (g_rotate % 180) ? b.x1 - b.x0 : b.y1 - b.y0;
}

/* Matrix mapping page points -> page pixels (origin at top-left of page). */
static fz_matrix page_ctm_s(int i, float s, int rot)
{
	fz_matrix m = fz_pre_rotate(fz_scale(s, s), (float)rot);
	fz_rect b = fz_transform_rect(g_bounds[i], m);
	return fz_concat(m, fz_translate(-b.x0, -b.y0));
}
static fz_matrix page_ctm(int i) { return page_ctm_s(i, scale(), g_rotate); }

static int page_x(int i)
{
	int vw, vh;
	client_size(&vw, &vh);
	int total = g_doc_w > vw ? g_doc_w : vw;
	return (total - g_page_w[i]) / 2;
}

/* first page whose bottom edge is below doc-y */
static int page_at_y(int y)
{
	int lo = 0, hi = g_npages - 1;
	if (g_npages <= 0) return -1;
	while (lo < hi) {
		int mid = (lo + hi) / 2;
		if (g_page_y[mid] + g_page_h[mid] < y) lo = mid + 1; else hi = mid;
	}
	return lo;
}

static int current_page(void)
{
	int vw, vh;
	if (!g_doc) return 0;
	client_size(&vw, &vh);
	return page_at_y(g_scroll_y + vh / 3);
}

/* client point -> page index + point in page space; returns -1 if none */
static int client_to_page(int cx, int cy, fz_point *pt, BOOL strict)
{
	if (!g_doc) return -1;
	int dy = cy + g_scroll_y, dx = cx + g_scroll_x;
	int i = page_at_y(dy);
	if (i < 0) return -1;
	int px = page_x(i), py = g_page_y[i];
	if (strict && (dx < px || dx >= px + g_page_w[i] || dy < py || dy >= py + g_page_h[i]))
		return -1;
	fz_matrix inv = fz_invert_matrix(page_ctm(i));
	*pt = fz_transform_point(fz_make_point((float)(dx - px), (float)(dy - py)), inv);
	return i;
}

static RECT page_rect_to_client(int i, fz_rect r)
{
	fz_rect t = fz_transform_rect(r, page_ctm(i));
	int ox = page_x(i) - g_scroll_x, oy = g_page_y[i] - g_scroll_y;
	RECT rc = { ox + (int)floorf(t.x0), oy + (int)floorf(t.y0), ox + (int)ceilf(t.x1), oy + (int)ceilf(t.y1) };
	return rc;
}

/* ------------------------------------------------------------------ */
/* Cache                                                               */
/* ------------------------------------------------------------------ */

static void tile_free(Tile *t)
{
	free(t->samples);
	memset(t, 0, sizeof *t);
	t->page = -1;
}

static void cache_clear(void)
{
	for (int i = 0; i < MAX_TILES; i++) tile_free(&g_tiles[i]);
}

static size_t cache_budget(void)
{
	int vw, vh;
	client_size(&vw, &vh);
	size_t b = (size_t)vw * vh * 3 * 4;   /* about four screens */
	if (b < (8u << 20)) b = 8u << 20;
	return b;
}

static void cache_trim(unsigned int keep_from_tick)
{
	size_t budget = cache_budget();
	for (;;) {
		size_t total = 0;
		int victim = -1;
		for (int i = 0; i < MAX_TILES; i++) {
			Tile *t = &g_tiles[i];
			if (!t->samples) continue;
			total += (size_t)t->stride * (t->r.bottom - t->r.top);
			if (t->used < keep_from_tick && (victim < 0 || t->used < g_tiles[victim].used))
				victim = i;
		}
		if (total <= budget || victim < 0) break;
		tile_free(&g_tiles[victim]);
	}
}

static Tile *get_tile(int pg, RECT need, int vw, int vh)
{
	float s = scale();
	Tile *t;
	for (int i = 0; i < MAX_TILES; i++) {
		t = &g_tiles[i];
		if (t->samples && t->page == pg && t->rot == g_rotate && t->dark == g_dark && t->scale == s &&
			t->r.left <= need.left && t->r.top <= need.top && t->r.right >= need.right && t->r.bottom >= need.bottom) {
			t->used = ++g_tick;
			return t;
		}
	}

	/* What to render: the whole page when small, otherwise a generous window around the need. */
	RECT r;
	int pw = g_page_w[pg], ph = g_page_h[pg];
	if ((long long)pw * ph <= (long long)vw * vh * 3) {
		SetRect(&r, 0, 0, pw, ph);
	} else {
		r = need;
		InflateRect(&r, vw / 2, vh / 2);
		if (r.left < 0) r.left = 0;
		if (r.top < 0) r.top = 0;
		if (r.right > pw) r.right = pw;
		if (r.bottom > ph) r.bottom = ph;
	}
	int w = r.right - r.left, h = r.bottom - r.top;
	if (w <= 0 || h <= 0) return NULL;

	/* pick a slot: empty or least recently used */
	int slot = 0;
	for (int i = 0; i < MAX_TILES; i++) {
		if (!g_tiles[i].samples) { slot = i; break; }
		if (g_tiles[i].used < g_tiles[slot].used) slot = i;
	}
	t = &g_tiles[slot];
	tile_free(t);

	int stride = (w * 3 + 3) & ~3;
	unsigned char *samples = malloc((size_t)stride * h);
	if (!samples) return NULL;
	memset(samples, 255, (size_t)stride * h);

	fz_page *page = NULL;
	fz_pixmap *pix = NULL;
	fz_device *dev = NULL;
	fz_var(page); fz_var(pix); fz_var(dev);
	fz_try(g_ctx) {
		page = fz_load_page(g_ctx, g_doc, pg);
		fz_matrix ctm = fz_concat(page_ctm(pg), fz_translate((float)-r.left, (float)-r.top));
		pix = fz_new_pixmap_with_data(g_ctx, fz_device_bgr(g_ctx), w, h, NULL, 0, stride, samples);
		dev = fz_new_draw_device(g_ctx, fz_identity, pix);
		fz_run_page(g_ctx, page, dev, ctm, NULL);
		fz_close_device(g_ctx, dev);
		if (g_dark) fz_invert_pixmap_luminance(g_ctx, pix);
	}
	fz_always(g_ctx) {
		fz_drop_device(g_ctx, dev);
		fz_drop_pixmap(g_ctx, pix);   /* does not free caller-owned samples */
		fz_drop_page(g_ctx, page);
	}
	fz_catch(g_ctx) {
		fz_report_error(g_ctx);   /* keep whatever was drawn (usually a partial page) */
	}

	t->page = pg; t->rot = g_rotate; t->dark = g_dark; t->scale = s;
	t->r = r; t->stride = stride; t->samples = samples; t->used = ++g_tick;
	return t;
}

/* ------------------------------------------------------------------ */
/* Layout & scrolling                                                  */
/* ------------------------------------------------------------------ */

static void update_status(void);

static void update_scrollbars(void)
{
	int vw, vh;
	client_size(&vw, &vh);
	SCROLLINFO si = { sizeof si };
	si.fMask = SIF_RANGE | SIF_PAGE | SIF_POS | SIF_DISABLENOSCROLL;
	si.nMin = 0; si.nMax = g_doc_h > 0 ? g_doc_h - 1 : 0; si.nPage = vh; si.nPos = g_scroll_y;
	SetScrollInfo(g_canvas, SB_VERT, &si, TRUE);
	si.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;
	si.nMax = g_doc_w > vw ? g_doc_w - 1 : 0; si.nPage = vw; si.nPos = g_scroll_x;
	SetScrollInfo(g_canvas, SB_HORZ, &si, TRUE);
}

static void clamp_scroll(int *x, int *y)
{
	int vw, vh;
	client_size(&vw, &vh);
	int mx = g_doc_w - vw, my = g_doc_h - vh;
	if (*x > mx) *x = mx;
	if (*y > my) *y = my;
	if (*x < 0) *x = 0;
	if (*y < 0) *y = 0;
}

static void layout(void)
{
	int vw, vh, gap = S(8);
	client_size(&vw, &vh);
	if (!g_doc || g_npages <= 0) {
		g_doc_w = g_doc_h = 0;
		update_scrollbars();
		return;
	}
	float maxw = 1, maxh = 1;
	for (int i = 0; i < g_npages; i++) {
		if (page_w_pt(i) > maxw) maxw = page_w_pt(i);
		if (page_h_pt(i) > maxh) maxh = page_h_pt(i);
	}
	float k = g_dpi / 72.0f;
	if (g_zoom_mode == ZM_FIT_WIDTH) {
		g_zoom = (vw - 2 * gap) / (maxw * k);
	} else if (g_zoom_mode == ZM_FIT_PAGE) {
		float zw = (vw - 2 * gap) / (maxw * k), zh = (vh - 2 * gap) / (maxh * k);
		g_zoom = zw < zh ? zw : zh;
	}
	if (g_zoom < 0.05f) g_zoom = 0.05f;
	if (g_zoom > 32.0f) g_zoom = 32.0f;

	float s = scale();
	int y = gap, w = 0;
	for (int i = 0; i < g_npages; i++) {
		g_page_w[i] = (int)(page_w_pt(i) * s + 0.5f);
		g_page_h[i] = (int)(page_h_pt(i) * s + 0.5f);
		if (g_page_w[i] < 1) g_page_w[i] = 1;
		if (g_page_h[i] < 1) g_page_h[i] = 1;
		g_page_y[i] = y;
		y += g_page_h[i] + gap;
		if (g_page_w[i] + 2 * gap > w) w = g_page_w[i] + 2 * gap;
	}
	g_doc_w = w;
	g_doc_h = y;
	clamp_scroll(&g_scroll_x, &g_scroll_y);
	update_scrollbars();
}

static void scroll_to(int x, int y)
{
	clamp_scroll(&x, &y);
	int dx = g_scroll_x - x, dy = g_scroll_y - y;
	if (!dx && !dy) return;
	g_scroll_x = x; g_scroll_y = y;
	ScrollWindowEx(g_canvas, dx, dy, NULL, NULL, NULL, NULL, SW_INVALIDATE);
	update_scrollbars();
	update_status();
	UpdateWindow(g_canvas);
}

/* An anchor remembers a document location so the view can be restored after relayout. */
typedef struct { int page; fz_point pt; int cx, cy; } Anchor;

static Anchor anchor_at(int cx, int cy)
{
	Anchor a = { -1 };
	a.cx = cx; a.cy = cy;
	if (g_doc) a.page = client_to_page(cx, cy, &a.pt, FALSE);
	return a;
}

static void anchor_restore(Anchor a)
{
	if (a.page < 0 || !g_doc) { InvalidateRect(g_canvas, NULL, FALSE); return; }
	fz_point p = fz_transform_point(a.pt, page_ctm(a.page));
	int x = page_x(a.page) + (int)p.x - a.cx;
	int y = g_page_y[a.page] + (int)p.y - a.cy;
	clamp_scroll(&x, &y);
	g_scroll_x = x; g_scroll_y = y;
	update_scrollbars();
	InvalidateRect(g_canvas, NULL, FALSE);
	update_status();
}

static void relayout_keep(int cx, int cy)
{
	Anchor a = anchor_at(cx, cy);
	layout();
	anchor_restore(a);
}

static void relayout_keep_center(void)
{
	int vw, vh;
	client_size(&vw, &vh);
	relayout_keep(vw / 2, vh / 3);
}

/* Show page pg; if y >= 0 it is a y coordinate in page points to bring to the top. */
static void go_to(int pg, float x, float y)
{
	if (!g_doc) return;
	if (pg < 0) pg = 0;
	if (pg >= g_npages) pg = g_npages - 1;
	int top = g_page_y[pg] - S(8);
	int left = g_scroll_x;
	if (y >= 0 || x >= 0) {
		fz_point p = fz_transform_point(fz_make_point(x >= 0 ? x : g_bounds[pg].x0, y >= 0 ? y : g_bounds[pg].y0), page_ctm(pg));
		if (y >= 0) top = g_page_y[pg] + (int)p.y - S(8);
		if (x >= 0) {
			int vw, vh;
			client_size(&vw, &vh);
			int px = page_x(pg) + (int)p.x;
			if (px < g_scroll_x || px > g_scroll_x + vw) left = px - S(8);
		}
	}
	scroll_to(left, top);
	update_status();
}

static void history_push(void)
{
	if (!g_doc) return;
	int pg = current_page();
	HistEntry e = { pg, (float)(g_scroll_y - g_page_y[pg]) / (g_page_h[pg] ? g_page_h[pg] : 1) };
	if (g_hist_pos < MAX_HISTORY) {
		g_hist[g_hist_pos++] = e;
	} else {
		memmove(g_hist, g_hist + 1, sizeof(HistEntry) * (MAX_HISTORY - 1));
		g_hist[MAX_HISTORY - 1] = e;
	}
	g_hist_len = g_hist_pos;
}

static void history_go(int dir)
{
	if (!g_doc) return;
	if (dir < 0 && g_hist_pos > 0) {
		if (g_hist_pos == g_hist_len) { history_push(); g_hist_pos--; }
		g_hist_pos--;
	} else if (dir > 0 && g_hist_pos + 1 < g_hist_len) {
		g_hist_pos++;
	} else return;
	HistEntry e = g_hist[g_hist_pos];
	if (e.page >= g_npages) return;
	scroll_to(g_scroll_x, g_page_y[e.page] + (int)(e.y * g_page_h[e.page]));
}

/* ------------------------------------------------------------------ */
/* Text (selection, search)                                            */
/* ------------------------------------------------------------------ */

static fz_stext_page *get_stext(int pg)
{
	if (g_stext && g_stext_page == pg) return g_stext;
	fz_drop_stext_page(g_ctx, g_stext);
	g_stext = NULL;
	g_stext_page = -1;
	fz_page *page = NULL;
	fz_var(page);
	fz_try(g_ctx) {
		fz_stext_options opts;
		memset(&opts, 0, sizeof opts);
		page = fz_load_page(g_ctx, g_doc, pg);
		g_stext = fz_new_stext_page_from_page(g_ctx, page, &opts);
		g_stext_page = pg;
	}
	fz_always(g_ctx) fz_drop_page(g_ctx, page);
	fz_catch(g_ctx) { fz_report_error(g_ctx); g_stext = NULL; }
	return g_stext;
}

static void invalidate_page(int pg)
{
	if (!g_doc || pg < 0 || pg >= g_npages) return;
	RECT rc = { page_x(pg) - g_scroll_x, g_page_y[pg] - g_scroll_y, 0, 0 };
	rc.right = rc.left + g_page_w[pg];
	rc.bottom = rc.top + g_page_h[pg];
	InvalidateRect(g_canvas, &rc, FALSE);
}

static void clear_selection(void)
{
	if (g_sel_page >= 0) invalidate_page(g_sel_page);
	g_sel_page = -1;
}

static void copy_selection(void)
{
	if (g_sel_page < 0) return;
	fz_stext_page *st = get_stext(g_sel_page);
	if (!st) return;
	char *text = NULL;
	fz_var(text);
	fz_try(g_ctx) text = fz_copy_selection(g_ctx, st, g_sel_a, g_sel_b, 1);
	fz_catch(g_ctx) { fz_report_error(g_ctx); return; }
	wchar_t *w = w_from_utf8(text ? text : "");
	fz_free(g_ctx, text);
	if (!w) return;
	size_t n = (wcslen(w) + 1) * sizeof(wchar_t);
	if (OpenClipboard(g_main)) {
		EmptyClipboard();
		HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, n);
		if (h) {
			memcpy(GlobalLock(h), w, n);
			GlobalUnlock(h);
			if (!SetClipboardData(CF_UNICODETEXT, h)) GlobalFree(h);
		}
		CloseClipboard();
	}
	free(w);
	SendMessageW(g_status, SB_SETTEXTW, 2, (LPARAM)L"Copied to clipboard");
}

static void select_all_page(void)
{
	if (!g_doc) return;
	clear_selection();
	g_sel_page = current_page();
	g_sel_a = fz_make_point(g_bounds[g_sel_page].x0 - 1, g_bounds[g_sel_page].y0 - 1);
	g_sel_b = fz_make_point(g_bounds[g_sel_page].x1 + 1, g_bounds[g_sel_page].y1 + 1);
	invalidate_page(g_sel_page);
}

static void reset_search(void)
{
	if (g_hit_page >= 0) invalidate_page(g_hit_page);
	g_hit_page = -1;
	g_nhits = 0;
}

static int search_page(int pg)
{
	fz_page *page = NULL;
	int n = 0;
	fz_var(page);
	fz_try(g_ctx) {
		page = fz_load_page(g_ctx, g_doc, pg);
		n = fz_search_page(g_ctx, page, g_needle, g_hit_marks, g_hit_quads, MAX_HITS);
	}
	fz_always(g_ctx) fz_drop_page(g_ctx, page);
	fz_catch(g_ctx) { fz_report_error(g_ctx); n = 0; }
	g_nquads = n;
	g_nhits = 0;
	for (int i = 0; i < n; i++)
		if (i == 0 || g_hit_marks[i]) g_hit_start[g_nhits++] = i;
	g_hit_start[g_nhits] = n;
	return g_nhits;
}

static void set_find_status(const wchar_t *s)
{
	if (g_find_dlg) SetDlgItemTextW(g_find_dlg, IDC_FIND_STATUS, s);
	SendMessageW(g_status, SB_SETTEXTW, 2, (LPARAM)s);
}

static void show_hit(void)
{
	int vw, vh;
	client_size(&vw, &vh);
	fz_rect r = fz_empty_rect;
	for (int q = g_hit_start[g_hit_cur]; q < g_hit_start[g_hit_cur + 1]; q++)
		r = fz_union_rect(r, fz_rect_from_quad(g_hit_quads[q]));
	RECT rc = page_rect_to_client(g_hit_page, r);
	int x = g_scroll_x, y = g_scroll_y;
	if (rc.top < 0 || rc.bottom > vh) y = g_scroll_y + rc.top - vh / 3;
	if (rc.left < 0 || rc.right > vw) x = g_scroll_x + rc.left - vw / 3;
	scroll_to(x, y);
	invalidate_page(g_hit_page);
	wchar_t buf[128];
	_snwprintf(buf, 127, L"Match %d of %d on page %d", g_hit_cur + 1, g_nhits, g_hit_page + 1);
	buf[127] = 0;
	set_find_status(buf);
}

static void find(int dir)
{
	if (!g_doc) return;
	if (g_find_dlg) {
		wchar_t w[512];
		GetDlgItemTextW(g_find_dlg, IDC_FIND_TEXT, w, 512);
		char *u = utf8_from_w(w);
		if (!u) return;
		if (strcmp(u, g_needle)) {
			reset_search();
			strncpy(g_needle, u, sizeof g_needle - 1);
			g_needle[sizeof g_needle - 1] = 0;
		}
		free(u);
	}
	if (!g_needle[0]) return;

	if (g_hit_page >= 0) {
		int next = g_hit_cur + dir;
		if (next >= 0 && next < g_nhits) { g_hit_cur = next; show_hit(); return; }
	}
	int start = g_hit_page >= 0 ? g_hit_page + dir : current_page();
	int old = g_hit_page;
	HCURSOR prev = SetCursor(LoadCursor(NULL, IDC_WAIT));
	g_hit_page = -1;
	if (old >= 0) invalidate_page(old);
	for (int n = 0; n < g_npages; n++) {
		int pg = ((start + n * dir) % g_npages + g_npages) % g_npages;
		if (n && n % 16 == 0) {
			wchar_t buf[64];
			_snwprintf(buf, 63, L"Searching page %d... (Esc to stop)", pg + 1);
			buf[63] = 0;
			set_find_status(buf);
			if (GetAsyncKeyState(VK_ESCAPE) & 0x8000) { set_find_status(L"Search stopped"); SetCursor(prev); return; }
		}
		if (search_page(pg) > 0) {
			g_hit_page = pg;
			g_hit_cur = dir > 0 ? 0 : g_nhits - 1;
			SetCursor(prev);
			show_hit();
			return;
		}
	}
	SetCursor(prev);
	g_nhits = 0;
	set_find_status(L"No matches found");
	MessageBeep(MB_ICONASTERISK);
}

/* ------------------------------------------------------------------ */
/* Links                                                               */
/* ------------------------------------------------------------------ */

static void drop_links(void)
{
	fz_drop_link(g_ctx, g_links);
	fz_drop_page(g_ctx, g_links_pg);
	g_links = NULL;
	g_links_pg = NULL;
	g_links_page = -1;
}

static fz_link *link_at(int cx, int cy)
{
	fz_point pt;
	int pg = client_to_page(cx, cy, &pt, TRUE);
	if (pg < 0) return NULL;
	if (pg != g_links_page) {
		drop_links();
		fz_try(g_ctx) {
			g_links_pg = fz_load_page(g_ctx, g_doc, pg);
			g_links = fz_load_links(g_ctx, g_links_pg);
			g_links_page = pg;
		}
		fz_catch(g_ctx) { fz_report_error(g_ctx); drop_links(); return NULL; }
	}
	for (fz_link *l = g_links; l; l = l->next)
		if (fz_is_point_inside_rect(pt, l->rect)) return l;
	return NULL;
}

static void follow_uri(const char *uri)
{
	if (!uri || !*uri) return;
	if (fz_is_external_link(g_ctx, uri)) {
		wchar_t *w = w_from_utf8(uri);
		if (!w) return;
		BOOL safe = !_wcsnicmp(w, L"http://", 7) || !_wcsnicmp(w, L"https://", 8) || !_wcsnicmp(w, L"mailto:", 7);
		if (!safe) {
			wchar_t msg[1200];
			_snwprintf(msg, 1199, L"This document wants to open:\n\n%.1000s\n\nOpen it?", w);
			msg[1199] = 0;
			safe = MessageBoxW(g_main, msg, APP_NAME, MB_ICONWARNING | MB_YESNO | MB_DEFBUTTON2) == IDYES;
		}
		if (safe) ShellExecuteW(g_main, L"open", w, NULL, NULL, SW_SHOWNORMAL);
		free(w);
		return;
	}
	fz_try(g_ctx) {
		float x = -1, y = -1;
		fz_location loc = fz_resolve_link(g_ctx, g_doc, uri, &x, &y);
		int pg = fz_page_number_from_location(g_ctx, g_doc, loc);
		if (pg >= 0) {
			history_push();
			go_to(pg, -1, (isnan(y) ? -1 : y));
		}
	}
	fz_catch(g_ctx) fz_report_error(g_ctx);
}

/* ------------------------------------------------------------------ */
/* Painting                                                            */
/* ------------------------------------------------------------------ */

static void fill_quads(HDC hdc, int pg, const fz_quad *q, int n, COLORREF color)
{
	HBRUSH br = CreateSolidBrush(color);
	HBRUSH old = SelectObject(hdc, br);
	/* DPa (multiply) on light pages, DPo (screen) on dark pages: text stays readable. */
	DWORD rop = g_dark ? 0x00FA0089 : 0x00A000C9;
	for (int i = 0; i < n; i++) {
		RECT rc = page_rect_to_client(pg, fz_rect_from_quad(q[i]));
		PatBlt(hdc, rc.left, rc.top, rc.right - rc.left, rc.bottom - rc.top, rop);
	}
	SelectObject(hdc, old);
	DeleteObject(br);
}

static void paint_canvas(HDC hdc, RECT *upd)
{
	int vw, vh;
	client_size(&vw, &vh);
	COLORREF bg = g_dark ? RGB(30, 30, 30) : RGB(0x5f, 0x63, 0x68);

	if (g_doc && g_npages > 0) {
		unsigned int frame = g_tick + 1;
		int first = page_at_y(g_scroll_y + upd->top);
		BITMAPINFO bmi;
		memset(&bmi, 0, sizeof bmi);
		bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
		bmi.bmiHeader.biPlanes = 1;
		bmi.bmiHeader.biBitCount = 24;
		bmi.bmiHeader.biCompression = BI_RGB;

		for (int i = first; i >= 0 && i < g_npages; i++) {
			int x = page_x(i) - g_scroll_x, y = g_page_y[i] - g_scroll_y;
			if (y >= upd->bottom) break;
			RECT prc = { x, y, x + g_page_w[i], y + g_page_h[i] }, vis;
			if (!IntersectRect(&vis, &prc, upd)) continue;
			RECT need = { vis.left - x, vis.top - y, vis.right - x, vis.bottom - y };
			Tile *t = get_tile(i, need, vw, vh);
			if (t) {
				int tw = t->r.right - t->r.left, th = t->r.bottom - t->r.top;
				bmi.bmiHeader.biWidth = tw;
				bmi.bmiHeader.biHeight = -th;
				SetDIBitsToDevice(hdc, x + t->r.left, y + t->r.top, tw, th, 0, 0, 0, th, t->samples, &bmi, DIB_RGB_COLORS);
			} else {
				FillRect(hdc, &prc, GetStockObject(WHITE_BRUSH));
			}
			if (i == g_hit_page && g_nhits > 0) {
				fill_quads(hdc, i, g_hit_quads, g_nquads, g_dark ? RGB(110, 90, 0) : RGB(255, 236, 120));
				int a = g_hit_start[g_hit_cur], b = g_hit_start[g_hit_cur + 1];
				fill_quads(hdc, i, g_hit_quads + a, b - a, g_dark ? RGB(150, 70, 0) : RGB(255, 160, 40));
			}
			if (i == g_sel_page) {
				fz_stext_page *st = get_stext(i);
				if (st) {
					fz_quad quads[1000];
					int n = 0;
					fz_try(g_ctx) n = fz_highlight_selection(g_ctx, st, g_sel_a, g_sel_b, quads, 1000);
					fz_catch(g_ctx) { fz_report_error(g_ctx); n = 0; }
					fill_quads(hdc, i, quads, n, g_dark ? RGB(30, 70, 140) : RGB(150, 200, 255));
				}
			}
			ExcludeClipRect(hdc, prc.left, prc.top, prc.right, prc.bottom);
		}
		cache_trim(frame);
	}

	HBRUSH br = CreateSolidBrush(bg);
	FillRect(hdc, upd, br);
	DeleteObject(br);

	if (!g_doc) {
		SelectClipRgn(hdc, NULL);
		RECT rc = { 0, 0, vw, vh };
		SetBkMode(hdc, TRANSPARENT);
		SetTextColor(hdc, RGB(230, 230, 230));
		HFONT old = SelectObject(hdc, g_ui_font);
		DrawTextW(hdc, L"Open a PDF with Ctrl+O, or drop a file here.", -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
		SelectObject(hdc, old);
	}
}

/* ------------------------------------------------------------------ */
/* Status, title, menus                                                */
/* ------------------------------------------------------------------ */

static void update_status(void)
{
	wchar_t buf[128];
	if (g_doc) {
		_snwprintf(buf, 127, L" Page %d of %d", current_page() + 1, g_npages);
		buf[127] = 0;
		SendMessageW(g_status, SB_SETTEXTW, 0, (LPARAM)buf);
		_snwprintf(buf, 127, L" %d%%%s", (int)(g_zoom * 100 + 0.5f),
			g_zoom_mode == ZM_FIT_WIDTH ? L" (fit width)" : g_zoom_mode == ZM_FIT_PAGE ? L" (fit page)" : L"");
		buf[127] = 0;
		SendMessageW(g_status, SB_SETTEXTW, 1, (LPARAM)buf);
	} else {
		SendMessageW(g_status, SB_SETTEXTW, 0, (LPARAM)L"");
		SendMessageW(g_status, SB_SETTEXTW, 1, (LPARAM)L"");
	}
}

static void update_title(void)
{
	wchar_t buf[1024];
	if (g_doc) _snwprintf(buf, 1023, L"%s - %s", base_name(g_path), APP_NAME);
	else wcscpy(buf, APP_NAME);
	buf[1023] = 0;
	SetWindowTextW(g_main, buf);
}

static void update_menu_checks(void)
{
	CheckMenuItem(g_menu, IDM_FITWIDTH, g_zoom_mode == ZM_FIT_WIDTH ? MF_CHECKED : MF_UNCHECKED);
	CheckMenuItem(g_menu, IDM_FITPAGE, g_zoom_mode == ZM_FIT_PAGE ? MF_CHECKED : MF_UNCHECKED);
	CheckMenuItem(g_menu, IDM_OUTLINE, g_sidebar ? MF_CHECKED : MF_UNCHECKED);
	CheckMenuItem(g_menu, IDM_DARK, g_dark ? MF_CHECKED : MF_UNCHECKED);
	CheckMenuItem(g_menu, IDM_HAND, g_hand ? MF_CHECKED : MF_UNCHECKED);
	CheckMenuItem(g_menu, IDM_FULLSCREEN, g_fullscreen ? MF_CHECKED : MF_UNCHECKED);
}

static void rebuild_recent_menu(void)
{
	while (GetMenuItemCount(g_recent_menu) > 0) DeleteMenu(g_recent_menu, 0, MF_BYPOSITION);
	for (int i = 0; i < g_nrecent; i++) {
		wchar_t buf[600];
		const wchar_t *p = g_recent[i].path;
		size_t len = wcslen(p);
		if (len > 80) _snwprintf(buf, 599, L"&%d  ...%s", (i + 1) % 10, p + len - 77);
		else _snwprintf(buf, 599, L"&%d  %s", (i + 1) % 10, p);
		buf[599] = 0;
		AppendMenuW(g_recent_menu, MF_STRING, IDM_RECENT0 + i, buf);
	}
	if (g_nrecent == 0) AppendMenuW(g_recent_menu, MF_STRING | MF_GRAYED, 0, L"(empty)");
	else {
		AppendMenuW(g_recent_menu, MF_SEPARATOR, 0, NULL);
		AppendMenuW(g_recent_menu, MF_STRING, IDM_RECENT_CLEAR, L"Clear list");
	}
}

/* ------------------------------------------------------------------ */
/* Settings                                                            */
/* ------------------------------------------------------------------ */

static void init_ini_path(void)
{
	/* Portable mode: if FeatherPDF.ini sits next to the exe, use it. */
	wchar_t exe[32768];
	DWORD n = GetModuleFileNameW(NULL, exe, 32768);
	if (n > 0 && n < 32768) {
		wchar_t *slash = wcsrchr(exe, L'\\');
		if (slash) {
			wcscpy(slash + 1, L"FeatherPDF.ini");
			if (GetFileAttributesW(exe) != INVALID_FILE_ATTRIBUTES) { wcscpy(g_ini, exe); return; }
		}
	}
	wchar_t appdata[32768];
	if (GetEnvironmentVariableW(L"APPDATA", appdata, 32000)) {
		_snwprintf(g_ini, 32767, L"%s\\FeatherPDF", appdata);
		CreateDirectoryW(g_ini, NULL);
		wcscat(g_ini, L"\\FeatherPDF.ini");
	}
}

static int ini_int(const wchar_t *key, int def)
{
	return g_ini[0] ? (int)GetPrivateProfileIntW(L"FeatherPDF", key, def, g_ini) : def;
}

static void ini_set_int(const wchar_t *key, int v)
{
	wchar_t buf[32];
	_snwprintf(buf, 31, L"%d", v);
	buf[31] = 0;
	if (g_ini[0]) WritePrivateProfileStringW(L"FeatherPDF", key, buf, g_ini);
}

static void load_settings(void)
{
	g_dark = ini_int(L"Dark", 0);
	g_sidebar = ini_int(L"Sidebar", 1);
	g_zoom_mode = ini_int(L"ZoomMode", ZM_FIT_WIDTH);
	if (g_zoom_mode < 0 || g_zoom_mode > ZM_FIT_PAGE) g_zoom_mode = ZM_FIT_WIDTH;
	g_zoom = ini_int(L"Zoom", 100) / 100.0f;
	g_sidebar_w = ini_int(L"SidebarWidth", 230);
	if (g_sidebar_w < 80 || g_sidebar_w > 1200) g_sidebar_w = 230;
	g_nrecent = 0;
	for (int i = 0; i < MAX_RECENT && g_ini[0]; i++) {
		wchar_t key[16], val[32800];
		_snwprintf(key, 15, L"Recent%d", i);
		key[15] = 0;
		GetPrivateProfileStringW(L"Recent", key, L"", val, 32800, g_ini);
		wchar_t *bar = wcschr(val, L'|');
		if (!bar) continue;
		*bar = 0;
		g_recent[g_nrecent].page = _wtoi(val);
		g_recent[g_nrecent].path = _wcsdup(bar + 1);
		g_nrecent++;
	}
}

static void save_recent(void)
{
	if (!g_ini[0]) return;
	WritePrivateProfileStringW(L"Recent", NULL, NULL, g_ini);
	for (int i = 0; i < g_nrecent; i++) {
		wchar_t key[16];
		size_t n = wcslen(g_recent[i].path) + 32;
		wchar_t *val = malloc(n * sizeof(wchar_t));
		if (!val) continue;
		_snwprintf(key, 15, L"Recent%d", i);
		key[15] = 0;
		_snwprintf(val, n, L"%d|%s", g_recent[i].page, g_recent[i].path);
		val[n - 1] = 0;
		WritePrivateProfileStringW(L"Recent", key, val, g_ini);
		free(val);
	}
}

static void save_settings(void)
{
	ini_set_int(L"Dark", g_dark);
	ini_set_int(L"Sidebar", g_sidebar);
	ini_set_int(L"ZoomMode", g_zoom_mode);
	ini_set_int(L"Zoom", (int)(g_zoom * 100 + 0.5f));
	ini_set_int(L"SidebarWidth", g_sidebar_w);
	if (!g_fullscreen) {
		WINDOWPLACEMENT wp = { sizeof wp };
		if (GetWindowPlacement(g_main, &wp)) {
			ini_set_int(L"WinX", wp.rcNormalPosition.left);
			ini_set_int(L"WinY", wp.rcNormalPosition.top);
			ini_set_int(L"WinW", wp.rcNormalPosition.right - wp.rcNormalPosition.left);
			ini_set_int(L"WinH", wp.rcNormalPosition.bottom - wp.rcNormalPosition.top);
			ini_set_int(L"WinMax", wp.showCmd == SW_SHOWMAXIMIZED);
		}
	}
	save_recent();
}

static int recent_find(const wchar_t *path)
{
	for (int i = 0; i < g_nrecent; i++)
		if (!_wcsicmp(g_recent[i].path, path)) return i;
	return -1;
}

static void recent_touch(const wchar_t *path, int page)
{
	int i = recent_find(path);
	Recent r;
	if (i >= 0) {
		r = g_recent[i];
		memmove(g_recent + 1, g_recent, sizeof(Recent) * i);
	} else {
		r.path = _wcsdup(path);
		if (!r.path) return;
		if (g_nrecent == MAX_RECENT) free(g_recent[--g_nrecent].path);
		memmove(g_recent + 1, g_recent, sizeof(Recent) * g_nrecent);
		g_nrecent++;
	}
	if (page >= 0) r.page = page;
	g_recent[0] = r;
	rebuild_recent_menu();
}

/* ------------------------------------------------------------------ */
/* Sidebar (outline)                                                   */
/* ------------------------------------------------------------------ */

static void layout_children(void);

static void add_outline(fz_outline *o, HTREEITEM parent, int depth)
{
	for (; o; o = o->next) {
		wchar_t *w = w_from_utf8(o->title ? o->title : "");
		TVINSERTSTRUCTW is;
		memset(&is, 0, sizeof is);
		is.hParent = parent;
		is.hInsertAfter = TVI_LAST;
		is.item.mask = TVIF_TEXT | TVIF_PARAM;
		is.item.pszText = w ? w : L"";
		is.item.lParam = (LPARAM)o;
		HTREEITEM item = (HTREEITEM)SendMessageW(g_tree, TVM_INSERTITEMW, 0, (LPARAM)&is);
		free(w);
		if (o->down && depth < 32) {
			add_outline(o->down, item, depth + 1);
			if (o->is_open) SendMessageW(g_tree, TVM_EXPAND, TVE_EXPAND, (LPARAM)item);
		}
	}
}

static void outline_go(fz_outline *o)
{
	if (!o) return;
	if (o->uri) { follow_uri(o->uri); return; }
	int pg = -1;
	fz_try(g_ctx) pg = fz_page_number_from_location(g_ctx, g_doc, o->page);
	fz_catch(g_ctx) { fz_report_error(g_ctx); pg = -1; }
	if (pg >= 0) { history_push(); go_to(pg, -1, (o->y > 0 && !isnan(o->y)) ? o->y : -1); }
}

/* ------------------------------------------------------------------ */
/* Document open/close                                                 */
/* ------------------------------------------------------------------ */

static INT_PTR CALLBACK password_proc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp)
{
	switch (msg) {
	case WM_INITDIALOG:
		SetWindowLongPtrW(dlg, GWLP_USERDATA, lp);
		if (((char *)lp)[0]) SetDlgItemTextW(dlg, IDC_PASSWORD_LABEL, L"Wrong password. Try again:");
		((char *)lp)[0] = 0;
		return TRUE;
	case WM_COMMAND:
		if (LOWORD(wp) == IDOK) {
			wchar_t w[512];
			GetDlgItemTextW(dlg, IDC_PASSWORD_EDIT, w, 512);
			char *u = utf8_from_w(w);
			char *dst = (char *)GetWindowLongPtrW(dlg, GWLP_USERDATA);
			if (u) { strncpy(dst, u, 511); dst[511] = 0; free(u); }
			SecureZeroMemory(w, sizeof w);
			EndDialog(dlg, IDOK);
			return TRUE;
		}
		if (LOWORD(wp) == IDCANCEL) { EndDialog(dlg, IDCANCEL); return TRUE; }
		break;
	}
	return FALSE;
}

static void close_document(void)
{
	if (g_doc && g_path[0]) {
		int i = recent_find(g_path);
		if (i >= 0) g_recent[i].page = current_page();
		save_recent();
	}
	drop_links();
	fz_drop_stext_page(g_ctx, g_stext);
	g_stext = NULL; g_stext_page = -1;
	cache_clear();
	TreeView_DeleteAllItems(g_tree);
	fz_drop_outline(g_ctx, g_outline);
	g_outline = NULL;
	fz_drop_document(g_ctx, g_doc);
	g_doc = NULL;
	free(g_bounds); free(g_page_y); free(g_page_w); free(g_page_h);
	g_bounds = NULL; g_page_y = g_page_w = g_page_h = NULL;
	g_npages = 0;
	g_sel_page = -1;
	g_hit_page = -1; g_nhits = 0;
	g_hist_len = g_hist_pos = 0;
	g_path[0] = 0;
	g_scroll_x = g_scroll_y = 0;
	fz_empty_store(g_ctx);
	layout();
	layout_children();
	update_title();
	update_status();
	InvalidateRect(g_canvas, NULL, TRUE);
	/* give freed memory back to the OS */
	SetProcessWorkingSetSize(GetCurrentProcess(), (SIZE_T)-1, (SIZE_T)-1);
}

static BOOL open_document(const wchar_t *path_in, int start_page, BOOL keep_view)
{
	wchar_t full[32768];
	if (!GetFullPathNameW(path_in, 32768, full, NULL)) wcscpy(full, path_in);
	char *u8 = utf8_from_w(full);
	if (!u8) return FALSE;

	fz_document *doc = NULL;
	fz_var(doc);
	fz_try(g_ctx) doc = fz_open_document(g_ctx, u8);
	fz_catch(g_ctx) {
		error_box(L"Could not open the file.\n\n%s", fz_caught_message(g_ctx));
		fz_ignore_error(g_ctx);
		free(u8);
		return FALSE;
	}
	free(u8);

	fz_try(g_ctx) {
		if (fz_needs_password(g_ctx, doc)) {
			char pw[512] = "";
			BOOL ok = FALSE;
			for (int attempt = 0; attempt < 5 && !ok; attempt++) {
				pw[0] = attempt ? 1 : 0;   /* signals "wrong password" to the dialog */
				if (DialogBoxParamW(g_inst, MAKEINTRESOURCEW(IDD_PASSWORD), g_main, password_proc, (LPARAM)pw) != IDOK)
					break;
				ok = fz_authenticate_password(g_ctx, doc, pw);
			}
			SecureZeroMemory(pw, sizeof pw);
			if (!ok) fz_throw(g_ctx, FZ_ERROR_GENERIC, "password required");
		}
	}
	fz_catch(g_ctx) {
		fz_drop_document(g_ctx, doc);
		fz_ignore_error(g_ctx);
		return FALSE;
	}

	int n = 0;
	fz_try(g_ctx) n = fz_count_pages(g_ctx, doc);
	fz_catch(g_ctx) { fz_report_error(g_ctx); n = 0; }
	if (n <= 0) {
		fz_drop_document(g_ctx, doc);
		error_box(L"The document has no readable pages.%s", "");
		return FALSE;
	}

	fz_rect *bounds = malloc(sizeof(fz_rect) * n);
	int *py = malloc(sizeof(int) * n), *pw = malloc(sizeof(int) * n), *ph = malloc(sizeof(int) * n);
	if (!bounds || !py || !pw || !ph) {
		free(bounds); free(py); free(pw); free(ph);
		fz_drop_document(g_ctx, doc);
		error_box(L"Out of memory.%s", "");
		return FALSE;
	}
	HCURSOR oldc = SetCursor(LoadCursor(NULL, IDC_WAIT));
	for (int i = 0; i < n; i++) {
		fz_page *page = NULL;
		fz_rect b = fz_make_rect(0, 0, 612, 792);
		fz_var(page);
		fz_try(g_ctx) {
			page = fz_load_page(g_ctx, doc, i);
			b = fz_bound_page(g_ctx, page);
		}
		fz_always(g_ctx) fz_drop_page(g_ctx, page);
		fz_catch(g_ctx) { fz_ignore_error(g_ctx); b = fz_make_rect(0, 0, 612, 792); }
		if (fz_is_empty_rect(b) || b.x1 - b.x0 < 1 || b.y1 - b.y0 < 1) b = fz_make_rect(0, 0, 612, 792);
		bounds[i] = b;
	}
	SetCursor(oldc);

	/* Keep view state across a reload of the same file. */
	int keep_page = keep_view ? current_page() : -1;
	int keep_x = g_scroll_x, keep_y = g_scroll_y;
	close_document();

	g_doc = doc;
	g_npages = n;
	g_bounds = bounds; g_page_y = py; g_page_w = pw; g_page_h = ph;
	wcscpy(g_path, full);
	if (!keep_view) g_rotate = 0;

	fz_try(g_ctx) g_outline = fz_load_outline(g_ctx, g_doc);
	fz_catch(g_ctx) { fz_report_error(g_ctx); g_outline = NULL; }
	SendMessageW(g_tree, WM_SETREDRAW, FALSE, 0);
	add_outline(g_outline, TVI_ROOT, 0);
	SendMessageW(g_tree, WM_SETREDRAW, TRUE, 0);

	int ri = recent_find(full);
	if (start_page < 0 && ri >= 0) start_page = g_recent[ri].page;
	recent_touch(full, start_page >= 0 ? start_page : 0);

	layout_children();
	layout();
	update_title();
	if (keep_view && keep_page >= 0) {
		g_scroll_x = keep_x; g_scroll_y = keep_y;
		clamp_scroll(&g_scroll_x, &g_scroll_y);
		update_scrollbars();
	} else {
		go_to(start_page > 0 ? start_page : 0, -1, -1);
	}
	update_status();
	InvalidateRect(g_canvas, NULL, FALSE);
	SetFocus(g_canvas);
	return TRUE;
}

static void open_dialog(void)
{
	wchar_t file[32768] = L"";
	OPENFILENAMEW ofn;
	memset(&ofn, 0, sizeof ofn);
	ofn.lStructSize = sizeof ofn;
	ofn.hwndOwner = g_main;
	ofn.lpstrFilter = L"PDF documents (*.pdf)\0*.pdf\0All files (*.*)\0*.*\0";
	ofn.lpstrFile = file;
	ofn.nMaxFile = 32768;
	ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY;
	if (GetOpenFileNameW(&ofn)) open_document(file, -1, FALSE);
}

/* ------------------------------------------------------------------ */
/* Printing                                                            */
/* ------------------------------------------------------------------ */

static void print_document(void)
{
	if (!g_doc) return;
	PRINTDLGW pd;
	memset(&pd, 0, sizeof pd);
	pd.lStructSize = sizeof pd;
	pd.hwndOwner = g_main;
	pd.Flags = PD_RETURNDC | PD_USEDEVMODECOPIESANDCOLLATE | PD_NOSELECTION | PD_ALLPAGES;
	pd.nFromPage = 1; pd.nToPage = (WORD)(g_npages > 65535 ? 65535 : g_npages);
	pd.nMinPage = 1; pd.nMaxPage = pd.nToPage;
	pd.nCopies = 1;
	if (!PrintDlgW(&pd)) return;
	HDC dc = pd.hDC;
	int from = 0, to = g_npages - 1;
	if (pd.Flags & PD_PAGENUMS) { from = pd.nFromPage - 1; to = pd.nToPage - 1; }
	if (pd.Flags & PD_CURRENTPAGE) from = to = current_page();

	int resx = GetDeviceCaps(dc, LOGPIXELSX), resy = GetDeviceCaps(dc, LOGPIXELSY);
	int areaw = GetDeviceCaps(dc, HORZRES), areah = GetDeviceCaps(dc, VERTRES);
	DOCINFOW di;
	memset(&di, 0, sizeof di);
	di.cbSize = sizeof di;
	di.lpszDocName = base_name(g_path);
	HCURSOR oldc = SetCursor(LoadCursor(NULL, IDC_WAIT));
	BOOL ok = StartDocW(dc, &di) > 0;
	for (int pg = from; ok && pg <= to && pg < g_npages; pg++) {
		wchar_t msg[64];
		_snwprintf(msg, 63, L"Printing page %d of %d...", pg - from + 1, to - from + 1);
		msg[63] = 0;
		SendMessageW(g_status, SB_SETTEXTW, 2, (LPARAM)msg);
		if (StartPage(dc) <= 0) { ok = FALSE; break; }

		fz_rect b = g_bounds[pg];
		float pwpt = b.x1 - b.x0, phpt = b.y1 - b.y0;
		int rot = 0;
		/* auto-rotate landscape pages onto portrait paper (and vice versa) */
		if ((pwpt > phpt) != (areaw > areah)) { rot = 90; float t = pwpt; pwpt = phpt; phpt = t; }
		float sx = areaw / (pwpt * resx / 72.0f), sy = areah / (phpt * resy / 72.0f);
		float fit = sx < sy ? sx : sy;
		if (fit > 1.0f) fit = 1.0f; /* shrink to fit, never enlarge */
		float s = fit * resx / 72.0f;
		float ys = (float)resy / resx;   /* non-square printer dpi */
		int w = (int)(pwpt * s + 0.5f), h = (int)(phpt * s * ys + 0.5f);
		int ox = (areaw - w) / 2, oy = (areah - h) / 2;

		fz_page *page = NULL;
		fz_display_list *list = NULL;
		fz_var(page); fz_var(list);
		fz_try(g_ctx) {
			page = fz_load_page(g_ctx, g_doc, pg);
			list = fz_new_display_list_from_page(g_ctx, page);
			fz_matrix base = fz_pre_rotate(fz_scale(s, s * ys), (float)rot);
			fz_rect tb = fz_transform_rect(b, base);
			base = fz_concat(base, fz_translate(-tb.x0, -tb.y0));
			const int band = 256;
			int stride = (w * 3 + 3) & ~3;
			unsigned char *samples = fz_malloc(g_ctx, (size_t)stride * band);
			fz_try(g_ctx) {
				BITMAPINFO bmi;
				memset(&bmi, 0, sizeof bmi);
				bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
				bmi.bmiHeader.biPlanes = 1;
				bmi.bmiHeader.biBitCount = 24;
				bmi.bmiHeader.biCompression = BI_RGB;
				for (int y = 0; y < h; y += band) {
					int bh = h - y < band ? h - y : band;
					fz_pixmap *pix = fz_new_pixmap_with_data(g_ctx, fz_device_bgr(g_ctx), w, bh, NULL, 0, stride, samples);
					fz_device *dev = NULL;
					fz_try(g_ctx) {
						fz_clear_pixmap_with_value(g_ctx, pix, 255);
						dev = fz_new_draw_device(g_ctx, fz_identity, pix);
						fz_run_display_list(g_ctx, list, dev, fz_concat(base, fz_translate(0, (float)-y)),
							fz_make_rect(0, 0, (float)w, (float)bh), NULL);
						fz_close_device(g_ctx, dev);
					}
					fz_always(g_ctx) { fz_drop_device(g_ctx, dev); fz_drop_pixmap(g_ctx, pix); }
					fz_catch(g_ctx) fz_rethrow(g_ctx);
					bmi.bmiHeader.biWidth = w;
					bmi.bmiHeader.biHeight = -bh;
					StretchDIBits(dc, ox, oy + y, w, bh, 0, 0, w, bh, samples, &bmi, DIB_RGB_COLORS, SRCCOPY);
				}
			}
			fz_always(g_ctx) fz_free(g_ctx, samples);
			fz_catch(g_ctx) fz_rethrow(g_ctx);
		}
		fz_always(g_ctx) { fz_drop_display_list(g_ctx, list); fz_drop_page(g_ctx, page); }
		fz_catch(g_ctx) fz_report_error(g_ctx);

		if (EndPage(dc) <= 0) ok = FALSE;
	}
	if (ok) EndDoc(dc); else AbortDoc(dc);
	DeleteDC(dc);
	if (pd.hDevMode) GlobalFree(pd.hDevMode);
	if (pd.hDevNames) GlobalFree(pd.hDevNames);
	SetCursor(oldc);
	SendMessageW(g_status, SB_SETTEXTW, 2, (LPARAM)(ok ? L"Sent to printer" : L"Printing failed or cancelled"));
}

/* ------------------------------------------------------------------ */
/* Commands                                                            */
/* ------------------------------------------------------------------ */

static const float zoom_steps[] = { 0.1f, 0.25f, 0.333f, 0.5f, 0.667f, 0.75f, 0.9f, 1.0f, 1.1f, 1.25f, 1.5f, 1.75f,
	2.0f, 2.5f, 3.0f, 4.0f, 6.0f, 8.0f, 12.0f, 16.0f, 24.0f, 32.0f };

static void set_zoom(float z, int mode, int cx, int cy)
{
	if (!g_doc) { g_zoom_mode = mode; g_zoom = z; update_menu_checks(); return; }
	Anchor a = anchor_at(cx, cy);
	g_zoom_mode = mode;
	if (mode == ZM_CUSTOM) g_zoom = z;
	layout();
	anchor_restore(a);
	update_menu_checks();
	update_status();
}

static void zoom_step(int dir, int cx, int cy)
{
	int n = sizeof zoom_steps / sizeof zoom_steps[0];
	float z = g_zoom;
	if (dir > 0) { for (int i = 0; i < n; i++) if (zoom_steps[i] > z * 1.01f) { z = zoom_steps[i]; break; } }
	else { for (int i = n - 1; i >= 0; i--) if (zoom_steps[i] < z * 0.99f) { z = zoom_steps[i]; break; } }
	set_zoom(z, ZM_CUSTOM, cx, cy);
}

static void zoom_center(int dir)
{
	int vw, vh;
	client_size(&vw, &vh);
	zoom_step(dir, vw / 2, vh / 2);
}

static void rotate_by(int deg)
{
	if (!g_doc) return;
	int pg = current_page();
	g_rotate = ((g_rotate + deg) % 360 + 360) % 360;
	clear_selection();
	layout();
	go_to(pg, -1, -1);
	InvalidateRect(g_canvas, NULL, FALSE);
}

static void set_fullscreen(BOOL on)
{
	if (on == g_fullscreen) return;
	if (on) {
		GetWindowPlacement(g_main, &g_saved_wp);
		g_saved_style = GetWindowLongPtrW(g_main, GWL_STYLE);
		MONITORINFO mi = { sizeof mi };
		GetMonitorInfoW(MonitorFromWindow(g_main, MONITOR_DEFAULTTONEAREST), &mi);
		g_fullscreen = TRUE;
		SetMenu(g_main, NULL);
		SetWindowLongPtrW(g_main, GWL_STYLE, (g_saved_style & ~WS_OVERLAPPEDWINDOW) | WS_POPUP);
		SetWindowPos(g_main, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top,
			mi.rcMonitor.right - mi.rcMonitor.left, mi.rcMonitor.bottom - mi.rcMonitor.top,
			SWP_NOOWNERZORDER | SWP_FRAMECHANGED | SWP_SHOWWINDOW);
	} else {
		g_fullscreen = FALSE;
		SetWindowLongPtrW(g_main, GWL_STYLE, g_saved_style);
		SetMenu(g_main, g_menu);
		SetWindowPlacement(g_main, &g_saved_wp);
		SetWindowPos(g_main, NULL, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
	}
	layout_children();
	update_menu_checks();
}

static void page_step(int dir)
{
	if (!g_doc) return;
	int pg = current_page();
	/* if the current page's top is above the viewport, "previous" first goes to its top */
	if (dir < 0 && g_page_y[pg] - S(8) < g_scroll_y - 2 && g_page_y[pg] + g_page_h[pg] > g_scroll_y) { go_to(pg, -1, -1); return; }
	go_to(pg + dir, -1, -1);
}

static void screen_step(int dir)
{
	int vw, vh;
	client_size(&vw, &vh);
	if (g_zoom_mode == ZM_FIT_PAGE) { page_step(dir); return; }
	scroll_to(g_scroll_x, g_scroll_y + dir * (vh - S(40)));
}

static INT_PTR CALLBACK goto_proc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp)
{
	(void)lp;
	switch (msg) {
	case WM_INITDIALOG: {
		wchar_t buf[64];
		_snwprintf(buf, 63, L"Page number (1-%d):", g_npages);
		buf[63] = 0;
		SetDlgItemTextW(dlg, IDC_GOTO_LABEL, buf);
		SetDlgItemInt(dlg, IDC_GOTO_EDIT, current_page() + 1, FALSE);
		SendDlgItemMessageW(dlg, IDC_GOTO_EDIT, EM_SETSEL, 0, -1);
		return TRUE;
	}
	case WM_COMMAND:
		if (LOWORD(wp) == IDOK) {
			BOOL ok;
			UINT v = GetDlgItemInt(dlg, IDC_GOTO_EDIT, &ok, FALSE);
			EndDialog(dlg, ok ? (INT_PTR)v : 0);
			return TRUE;
		}
		if (LOWORD(wp) == IDCANCEL) { EndDialog(dlg, 0); return TRUE; }
		break;
	}
	return FALSE;
}

static INT_PTR CALLBACK find_proc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp)
{
	(void)lp;
	switch (msg) {
	case WM_INITDIALOG:
		return TRUE;
	case WM_COMMAND:
		switch (LOWORD(wp)) {
		case IDOK:
		case IDC_FIND_NEXT: find(+1); return TRUE;
		case IDC_FIND_PREV: find(-1); return TRUE;
		case IDCANCEL:
			ShowWindow(dlg, SW_HIDE);
			SetFocus(g_canvas);
			return TRUE;
		}
		break;
	}
	return FALSE;
}

static void show_find(void)
{
	if (!g_find_dlg) {
		g_find_dlg = CreateDialogW(g_inst, MAKEINTRESOURCEW(IDD_FIND), g_main, find_proc);
		if (!g_find_dlg) return;
		RECT mr, dr;
		GetWindowRect(g_canvas, &mr);
		GetWindowRect(g_find_dlg, &dr);
		SetWindowPos(g_find_dlg, NULL, mr.right - (dr.right - dr.left) - S(24), mr.top + S(8), 0, 0, SWP_NOSIZE | SWP_NOZORDER);
	}
	ShowWindow(g_find_dlg, SW_SHOW);
	SetFocus(GetDlgItem(g_find_dlg, IDC_FIND_TEXT));
	SendDlgItemMessageW(g_find_dlg, IDC_FIND_TEXT, EM_SETSEL, 0, -1);
}

static void show_properties(void)
{
	if (!g_doc) return;
	static const struct { const char *key; const wchar_t *label; } keys[] = {
		{ FZ_META_INFO_TITLE, L"Title" }, { FZ_META_INFO_AUTHOR, L"Author" },
		{ FZ_META_INFO_SUBJECT, L"Subject" }, { FZ_META_INFO_CREATOR, L"Creator" },
		{ FZ_META_INFO_PRODUCER, L"Producer" }, { FZ_META_INFO_CREATIONDATE, L"Created" },
		{ FZ_META_INFO_MODIFICATIONDATE, L"Modified" }, { FZ_META_FORMAT, L"Format" },
		{ FZ_META_ENCRYPTION, L"Encryption" },
	};
	size_t cap = 8192, len = 0;
	wchar_t *out = malloc(cap * sizeof(wchar_t));
	if (!out) return;
	len += _snwprintf(out, cap, L"File:\t%s\nPages:\t%d\n", g_path, g_npages);
	if (len >= cap) len = cap - 1;
	for (size_t i = 0; i < sizeof keys / sizeof keys[0]; i++) {
		char buf[1024];
		buf[0] = 0;
		int n = -1;
		fz_try(g_ctx) n = fz_lookup_metadata(g_ctx, g_doc, keys[i].key, buf, sizeof buf);
		fz_catch(g_ctx) { fz_ignore_error(g_ctx); n = -1; }
		if (n > 0 && buf[0]) {
			wchar_t *w = w_from_utf8(buf);
			int k = _snwprintf(out + len, cap - len, L"%s:\t%s\n", keys[i].label, w ? w : L"");
			free(w);
			if (k > 0) len += k; else break;
		}
	}
	out[cap - 1] = 0;
	MessageBoxW(g_main, out, L"Document Properties", MB_OK | MB_ICONINFORMATION);
	free(out);
}

static void show_shortcuts(void)
{
	MessageBoxW(g_main,
		L"Open\tCtrl+O  (or drag a file onto the window)\n"
		L"Print\tCtrl+P\n"
		L"Reload\tF5\n"
		L"Close\tCtrl+W\n\n"
		L"Next / previous page\tPage Down / Page Up, Space, N / P\n"
		L"First / last page\tHome / End\n"
		L"Go to page\tCtrl+G\n"
		L"Back / forward\tAlt+Left / Alt+Right\n\n"
		L"Zoom in / out\tCtrl+Plus / Ctrl+Minus, Ctrl+Mouse wheel\n"
		L"Fit page\tCtrl+0\n"
		L"Actual size\tCtrl+1\n"
		L"Fit width\tCtrl+2\n"
		L"Rotate right / left\tCtrl+R / Ctrl+Shift+R\n\n"
		L"Find\tCtrl+F, then Enter / F3 / Shift+F3\n"
		L"Select text\tDrag with the mouse (double-click selects a word)\n"
		L"Copy\tCtrl+C\n"
		L"Select page text\tCtrl+A\n"
		L"Hand (pan) tool\tH  (middle-drag always pans)\n\n"
		L"Contents sidebar\tF9\n"
		L"Full screen\tF11 (Esc to leave)\n"
		L"Dark pages\tCtrl+D",
		L"Keyboard Shortcuts", MB_OK);
}

static void command(int id)
{
	int vw, vh;
	client_size(&vw, &vh);
	if (id >= IDM_RECENT0 && id < IDM_RECENT0 + MAX_RECENT) {
		int i = id - IDM_RECENT0;
		if (i < g_nrecent) {
			wchar_t *p = _wcsdup(g_recent[i].path);
			if (p) {
				if (GetFileAttributesW(p) == INVALID_FILE_ATTRIBUTES) {
					MessageBoxW(g_main, L"That file no longer exists.", APP_NAME, MB_ICONWARNING);
					free(g_recent[i].path);
					memmove(g_recent + i, g_recent + i + 1, sizeof(Recent) * (g_nrecent - i - 1));
					g_nrecent--;
					rebuild_recent_menu();
					save_recent();
				} else open_document(p, -1, FALSE);
				free(p);
			}
		}
		return;
	}
	switch (id) {
	case IDM_OPEN: open_dialog(); break;
	case IDM_CLOSE: close_document(); break;
	case IDM_PRINT: print_document(); break;
	case IDM_RELOAD:
		if (g_doc) {
			wchar_t *p = _wcsdup(g_path);
			if (p) { open_document(p, -1, TRUE); free(p); }
		}
		break;
	case IDM_PROPERTIES: show_properties(); break;
	case IDM_EXIT: PostMessageW(g_main, WM_CLOSE, 0, 0); break;
	case IDM_RECENT_CLEAR:
		for (int i = 0; i < g_nrecent; i++) free(g_recent[i].path);
		g_nrecent = 0;
		rebuild_recent_menu();
		save_recent();
		break;
	case IDM_COPY: copy_selection(); break;
	case IDM_SELECTALL: select_all_page(); break;
	case IDM_FIND: if (g_doc) show_find(); break;
	case IDM_FINDNEXT: if (g_needle[0]) find(+1); else if (g_doc) show_find(); break;
	case IDM_FINDPREV: if (g_needle[0]) find(-1); else if (g_doc) show_find(); break;
	case IDM_ZOOMIN: zoom_center(+1); break;
	case IDM_ZOOMOUT: zoom_center(-1); break;
	case IDM_FITPAGE: {
		int pg = current_page();
		set_zoom(g_zoom, ZM_FIT_PAGE, vw / 2, vh / 2);
		go_to(pg, -1, -1);
		break;
	}
	case IDM_FITWIDTH: set_zoom(g_zoom, ZM_FIT_WIDTH, 0, 0); break;
	case IDM_ACTUAL: set_zoom(1.0f, ZM_CUSTOM, vw / 2, vh / 2); break;
	case IDM_ROTL: rotate_by(-90); break;
	case IDM_ROTR: rotate_by(90); break;
	case IDM_OUTLINE: g_sidebar = !g_sidebar; layout_children(); update_menu_checks(); break;
	case IDM_FULLSCREEN: set_fullscreen(!g_fullscreen); break;
	case IDM_DARK: g_dark = !g_dark; InvalidateRect(g_canvas, NULL, FALSE); update_menu_checks(); break;
	case IDM_HAND: g_hand = !g_hand; update_menu_checks(); break;
	case IDM_NEXTPAGE: page_step(+1); break;
	case IDM_PREVPAGE: page_step(-1); break;
	case IDM_FIRSTPAGE: if (g_doc) { history_push(); go_to(0, -1, -1); } break;
	case IDM_LASTPAGE: if (g_doc) { history_push(); go_to(g_npages - 1, -1, -1); } break;
	case IDM_GOTO:
		if (g_doc) {
			INT_PTR v = DialogBoxParamW(g_inst, MAKEINTRESOURCEW(IDD_GOTO), g_main, goto_proc, 0);
			if (v > 0) { history_push(); go_to((int)v - 1, -1, -1); }
		}
		break;
	case IDM_BACK: history_go(-1); break;
	case IDM_FORWARD: history_go(+1); break;
	case IDM_SHORTCUTS: show_shortcuts(); break;
	case IDM_ABOUT:
		MessageBoxW(g_main,
			APP_NAME L" " APP_VERSION L"\n\nA tiny, fast PDF reader for Windows.\n\n"
			L"Rendering by MuPDF (c) Artifex Software, licensed under the GNU AGPL v3.\n"
			L"Source code: https://github.com/donna49/pdf-reader",
			L"About " APP_NAME, MB_OK | MB_ICONINFORMATION);
		break;
	}
}

/* ------------------------------------------------------------------ */
/* Canvas window                                                       */
/* ------------------------------------------------------------------ */

static void on_scroll(int bar, int code)
{
	SCROLLINFO si = { sizeof si, SIF_ALL };
	GetScrollInfo(g_canvas, bar, &si);
	int pos = bar == SB_VERT ? g_scroll_y : g_scroll_x;
	int line = S(48);
	switch (code) {
	case SB_LINEUP: pos -= line; break;
	case SB_LINEDOWN: pos += line; break;
	case SB_PAGEUP: pos -= si.nPage; break;
	case SB_PAGEDOWN: pos += si.nPage; break;
	case SB_THUMBTRACK:
	case SB_THUMBPOSITION: pos = si.nTrackPos; break;
	case SB_TOP: pos = 0; break;
	case SB_BOTTOM: pos = INT_MAX / 2; break;
	default: return;
	}
	if (bar == SB_VERT) scroll_to(g_scroll_x, pos); else scroll_to(pos, g_scroll_y);
}

static void update_selection_to(int cx, int cy)
{
	fz_point pt;
	int pg = client_to_page(cx, cy, &pt, FALSE);
	if (pg < 0 || g_sel_page < 0) return;
	if (pg != g_sel_page) {
		/* clamp to the start page: before it -> its top, after it -> its bottom */
		fz_matrix inv = fz_invert_matrix(page_ctm(g_sel_page));
		int dy = cy + g_scroll_y - g_page_y[g_sel_page], dx = cx + g_scroll_x - page_x(g_sel_page);
		pt = fz_transform_point(fz_make_point((float)dx, (float)dy), inv);
	}
	g_sel_b = pt;
	invalidate_page(g_sel_page);
}

static LRESULT CALLBACK canvas_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
	switch (msg) {
	case WM_ERASEBKGND:
		return 1;
	case WM_PAINT: {
		PAINTSTRUCT ps;
		HDC hdc = BeginPaint(hwnd, &ps);
		paint_canvas(hdc, &ps.rcPaint);
		EndPaint(hwnd, &ps);
		return 0;
	}
	case WM_SIZE:
		if (g_doc) {
			if (g_zoom_mode != ZM_CUSTOM) relayout_keep(0, 0);
			else { layout(); InvalidateRect(hwnd, NULL, FALSE); }
		} else {
			layout();
			InvalidateRect(hwnd, NULL, FALSE);
		}
		update_status();
		return 0;
	case WM_VSCROLL: on_scroll(SB_VERT, LOWORD(wp)); return 0;
	case WM_HSCROLL: on_scroll(SB_HORZ, LOWORD(wp)); return 0;
	case WM_MOUSEWHEEL:
	case WM_MOUSEHWHEEL: {
		int delta = GET_WHEEL_DELTA_WPARAM(wp);
		POINT p = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
		ScreenToClient(hwnd, &p);
		if (msg == WM_MOUSEWHEEL && (GET_KEYSTATE_WPARAM(wp) & MK_CONTROL)) {
			static int acc;
			acc += delta;
			while (acc >= WHEEL_DELTA) { zoom_step(+1, p.x, p.y); acc -= WHEEL_DELTA; }
			while (acc <= -WHEEL_DELTA) { zoom_step(-1, p.x, p.y); acc += WHEEL_DELTA; }
			return 0;
		}
		UINT lines = 3;
		SystemParametersInfoW(SPI_GETWHEELSCROLLLINES, 0, &lines, 0);
		if (lines == WHEEL_PAGESCROLL) lines = 10;
		int amount = MulDiv(delta, (int)lines * S(30), WHEEL_DELTA);
		if (msg == WM_MOUSEHWHEEL) scroll_to(g_scroll_x + amount, g_scroll_y);
		else if (GET_KEYSTATE_WPARAM(wp) & MK_SHIFT) scroll_to(g_scroll_x - amount, g_scroll_y);
		else scroll_to(g_scroll_x, g_scroll_y - amount);
		return 0;
	}
	case WM_KEYDOWN: {
		BOOL ctrl = GetKeyState(VK_CONTROL) < 0, shift = GetKeyState(VK_SHIFT) < 0;
		int vw, vh;
		client_size(&vw, &vh);
		switch (wp) {
		case VK_DOWN: scroll_to(g_scroll_x, g_scroll_y + S(48)); break;
		case VK_UP: scroll_to(g_scroll_x, g_scroll_y - S(48)); break;
		case VK_LEFT:
			if (g_doc_w <= vw) page_step(-1); else scroll_to(g_scroll_x - S(48), g_scroll_y);
			break;
		case VK_RIGHT:
			if (g_doc_w <= vw) page_step(+1); else scroll_to(g_scroll_x + S(48), g_scroll_y);
			break;
		case VK_NEXT: screen_step(+1); break;
		case VK_PRIOR: screen_step(-1); break;
		case VK_SPACE: screen_step(shift ? -1 : +1); break;
		case VK_HOME: command(IDM_FIRSTPAGE); break;
		case VK_END: command(IDM_LASTPAGE); break;
		case 'N': if (!ctrl) page_step(+1); break;
		case 'P': if (!ctrl) page_step(-1); break;
		case 'H': if (!ctrl) command(IDM_HAND); break;
		case VK_ESCAPE:
			if (g_fullscreen) set_fullscreen(FALSE);
			else if (g_sel_page >= 0) clear_selection();
			else reset_search();
			break;
		}
		return 0;
	}
	case WM_LBUTTONDOWN:
	case WM_LBUTTONDBLCLK:
	case WM_MBUTTONDOWN: {
		SetFocus(hwnd);
		SetCapture(hwnd);
		g_down_pt.x = GET_X_LPARAM(lp); g_down_pt.y = GET_Y_LPARAM(lp);
		g_mouse_down = TRUE;
		g_selecting = FALSE;
		g_panning = (msg == WM_MBUTTONDOWN) || g_hand;
		g_pan_origin = g_down_pt;
		g_pan_sx = g_scroll_x; g_pan_sy = g_scroll_y;
		if (g_panning) { SetCursor(LoadCursor(NULL, IDC_SIZEALL)); return 0; }
		if (msg == WM_LBUTTONDBLCLK && g_doc) {
			fz_point pt;
			int pg = client_to_page(g_down_pt.x, g_down_pt.y, &pt, TRUE);
			fz_stext_page *st = pg >= 0 ? get_stext(pg) : NULL;
			if (st) {
				clear_selection();
				fz_point a = pt, b = pt;
				fz_try(g_ctx) fz_snap_selection(g_ctx, st, &a, &b, FZ_SELECT_WORDS);
				fz_catch(g_ctx) fz_report_error(g_ctx);
				g_sel_page = pg; g_sel_a = a; g_sel_b = b;
				invalidate_page(pg);
			}
			g_mouse_down = FALSE;
			ReleaseCapture();
		}
		return 0;
	}
	case WM_MOUSEMOVE: {
		int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp);
		if (!g_mouse_down) {
			if (g_doc && !g_hand) {
				fz_point pt;
				if (link_at(x, y)) SetCursor(LoadCursor(NULL, IDC_HAND));
				else if (client_to_page(x, y, &pt, TRUE) >= 0) SetCursor(LoadCursor(NULL, IDC_IBEAM));
				else SetCursor(LoadCursor(NULL, IDC_ARROW));
			} else SetCursor(LoadCursor(NULL, g_hand && g_doc ? IDC_HAND : IDC_ARROW));
			return 0;
		}
		if (g_panning) {
			scroll_to(g_pan_sx - (x - g_pan_origin.x), g_pan_sy - (y - g_pan_origin.y));
			return 0;
		}
		if (!g_doc) return 0;
		if (!g_selecting && (abs(x - g_down_pt.x) > S(3) || abs(y - g_down_pt.y) > S(3))) {
			fz_point pt;
			int pg = client_to_page(g_down_pt.x, g_down_pt.y, &pt, FALSE);
			if (pg >= 0) {
				clear_selection();
				g_selecting = TRUE;
				g_sel_page = pg;
				g_sel_a = g_sel_b = pt;
			}
		}
		if (g_selecting) {
			int vw, vh;
			client_size(&vw, &vh);
			/* auto-scroll when dragging past the edge */
			if (y < 0) scroll_to(g_scroll_x, g_scroll_y + y);
			else if (y > vh) scroll_to(g_scroll_x, g_scroll_y + (y - vh));
			update_selection_to(x, y);
		}
		return 0;
	}
	case WM_LBUTTONUP:
	case WM_MBUTTONUP: {
		int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp);
		BOOL was_click = g_mouse_down && !g_selecting && !g_panning;
		BOOL was_pan_click = g_mouse_down && g_panning && abs(x - g_down_pt.x) <= S(3) && abs(y - g_down_pt.y) <= S(3);
		g_mouse_down = FALSE;
		g_panning = FALSE;
		ReleaseCapture();
		if (g_selecting) { g_selecting = FALSE; return 0; }
		if (msg == WM_LBUTTONUP && g_doc && (was_click || (g_hand && was_pan_click))) {
			fz_link *l = link_at(x, y);
			if (l) {
				char *uri = fz_strdup(g_ctx, l->uri);
				follow_uri(uri);
				fz_free(g_ctx, uri);
			} else if (was_click) clear_selection();
		}
		return 0;
	}
	case WM_RBUTTONUP: {
		HMENU m = CreatePopupMenu();
		AppendMenuW(m, MF_STRING | (g_sel_page >= 0 ? 0 : MF_GRAYED), IDM_COPY, L"&Copy\tCtrl+C");
		AppendMenuW(m, MF_STRING | (g_doc ? 0 : MF_GRAYED), IDM_SELECTALL, L"Select &page text\tCtrl+A");
		AppendMenuW(m, MF_SEPARATOR, 0, NULL);
		AppendMenuW(m, MF_STRING | (g_hist_pos > 0 ? 0 : MF_GRAYED), IDM_BACK, L"&Back\tAlt+Left");
		AppendMenuW(m, MF_STRING | (g_doc ? 0 : MF_GRAYED), IDM_FIND, L"&Find...\tCtrl+F");
		AppendMenuW(m, MF_STRING | (g_doc ? 0 : MF_GRAYED), IDM_GOTO, L"&Go to page...\tCtrl+G");
		AppendMenuW(m, MF_SEPARATOR, 0, NULL);
		AppendMenuW(m, MF_STRING | (g_hand ? MF_CHECKED : 0), IDM_HAND, L"&Hand tool\tH");
		AppendMenuW(m, MF_STRING | (g_fullscreen ? MF_CHECKED : 0), IDM_FULLSCREEN, L"F&ull screen\tF11");
		POINT p = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
		ClientToScreen(hwnd, &p);
		int cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, p.x, p.y, 0, hwnd, NULL);
		DestroyMenu(m);
		if (cmd) command(cmd);
		return 0;
	}
	case WM_CAPTURECHANGED:
		g_mouse_down = FALSE; g_panning = FALSE; g_selecting = FALSE;
		return 0;
	case WM_GETDLGCODE:
		return DLGC_WANTARROWS | DLGC_WANTCHARS;
	}
	return DefWindowProcW(hwnd, msg, wp, lp);
}

/* ------------------------------------------------------------------ */
/* Main window                                                         */
/* ------------------------------------------------------------------ */

static BOOL sidebar_visible(void)
{
	return g_sidebar && g_outline && !g_fullscreen;
}

static void layout_children(void)
{
	RECT rc;
	if (!g_main) return;
	GetClientRect(g_main, &rc);
	int sh = 0;
	if (!g_fullscreen) {
		ShowWindow(g_status, SW_SHOW);
		SendMessageW(g_status, WM_SIZE, 0, 0);
		RECT sr;
		GetWindowRect(g_status, &sr);
		sh = sr.bottom - sr.top;
		int parts[3] = { S(150), S(320), -1 };
		SendMessageW(g_status, SB_SETPARTS, 3, (LPARAM)parts);
	} else ShowWindow(g_status, SW_HIDE);
	int h = rc.bottom - sh;
	int sw = 0;
	if (sidebar_visible()) {
		sw = S(g_sidebar_w);
		if (sw > rc.right - S(100)) sw = rc.right - S(100);
		if (sw < 0) sw = 0;
		MoveWindow(g_tree, 0, 0, sw, h, TRUE);
		ShowWindow(g_tree, SW_SHOW);
		sw += S(5); /* splitter */
	} else ShowWindow(g_tree, SW_HIDE);
	MoveWindow(g_canvas, sw, 0, rc.right - sw > 0 ? rc.right - sw : 0, h > 0 ? h : 0, TRUE);
}

static BOOL over_splitter(int x, int y)
{
	if (!sidebar_visible()) return FALSE;
	RECT tr;
	GetWindowRect(g_tree, &tr);
	MapWindowPoints(NULL, g_main, (POINT *)&tr, 2);
	return x >= tr.right && x < tr.right + S(5) && y < tr.bottom;
}

static void create_menu(void)
{
	g_menu = CreateMenu();
	g_file_menu = CreatePopupMenu();
	g_recent_menu = CreatePopupMenu();
	AppendMenuW(g_file_menu, MF_STRING, IDM_OPEN, L"&Open...\tCtrl+O");
	AppendMenuW(g_file_menu, MF_POPUP, (UINT_PTR)g_recent_menu, L"Open &Recent");
	AppendMenuW(g_file_menu, MF_STRING, IDM_RELOAD, L"Re&load\tF5");
	AppendMenuW(g_file_menu, MF_STRING, IDM_CLOSE, L"&Close\tCtrl+W");
	AppendMenuW(g_file_menu, MF_SEPARATOR, 0, NULL);
	AppendMenuW(g_file_menu, MF_STRING, IDM_PRINT, L"&Print...\tCtrl+P");
	AppendMenuW(g_file_menu, MF_STRING, IDM_PROPERTIES, L"Propert&ies\tCtrl+I");
	AppendMenuW(g_file_menu, MF_SEPARATOR, 0, NULL);
	AppendMenuW(g_file_menu, MF_STRING, IDM_EXIT, L"E&xit\tAlt+F4");
	AppendMenuW(g_menu, MF_POPUP, (UINT_PTR)g_file_menu, L"&File");

	HMENU edit = CreatePopupMenu();
	AppendMenuW(edit, MF_STRING, IDM_COPY, L"&Copy\tCtrl+C");
	AppendMenuW(edit, MF_STRING, IDM_SELECTALL, L"Select &Page Text\tCtrl+A");
	AppendMenuW(edit, MF_SEPARATOR, 0, NULL);
	AppendMenuW(edit, MF_STRING, IDM_FIND, L"&Find...\tCtrl+F");
	AppendMenuW(edit, MF_STRING, IDM_FINDNEXT, L"Find &Next\tF3");
	AppendMenuW(edit, MF_STRING, IDM_FINDPREV, L"Find Pre&vious\tShift+F3");
	AppendMenuW(g_menu, MF_POPUP, (UINT_PTR)edit, L"&Edit");

	g_view_menu = CreatePopupMenu();
	AppendMenuW(g_view_menu, MF_STRING, IDM_ZOOMIN, L"Zoom &In\tCtrl++");
	AppendMenuW(g_view_menu, MF_STRING, IDM_ZOOMOUT, L"Zoom &Out\tCtrl+-");
	AppendMenuW(g_view_menu, MF_STRING, IDM_FITPAGE, L"Fit &Page\tCtrl+0");
	AppendMenuW(g_view_menu, MF_STRING, IDM_ACTUAL, L"&Actual Size\tCtrl+1");
	AppendMenuW(g_view_menu, MF_STRING, IDM_FITWIDTH, L"Fit &Width\tCtrl+2");
	AppendMenuW(g_view_menu, MF_SEPARATOR, 0, NULL);
	AppendMenuW(g_view_menu, MF_STRING, IDM_ROTR, L"Rotate &Right\tCtrl+R");
	AppendMenuW(g_view_menu, MF_STRING, IDM_ROTL, L"Rotate &Left\tCtrl+Shift+R");
	AppendMenuW(g_view_menu, MF_SEPARATOR, 0, NULL);
	AppendMenuW(g_view_menu, MF_STRING, IDM_OUTLINE, L"&Contents Sidebar\tF9");
	AppendMenuW(g_view_menu, MF_STRING, IDM_DARK, L"&Dark Pages\tCtrl+D");
	AppendMenuW(g_view_menu, MF_STRING, IDM_HAND, L"&Hand Tool\tH");
	AppendMenuW(g_view_menu, MF_STRING, IDM_FULLSCREEN, L"F&ull Screen\tF11");
	AppendMenuW(g_menu, MF_POPUP, (UINT_PTR)g_view_menu, L"&View");

	HMENU go = CreatePopupMenu();
	AppendMenuW(go, MF_STRING, IDM_NEXTPAGE, L"&Next Page\tPage Down");
	AppendMenuW(go, MF_STRING, IDM_PREVPAGE, L"&Previous Page\tPage Up");
	AppendMenuW(go, MF_STRING, IDM_FIRSTPAGE, L"&First Page\tHome");
	AppendMenuW(go, MF_STRING, IDM_LASTPAGE, L"&Last Page\tEnd");
	AppendMenuW(go, MF_STRING, IDM_GOTO, L"&Go to Page...\tCtrl+G");
	AppendMenuW(go, MF_SEPARATOR, 0, NULL);
	AppendMenuW(go, MF_STRING, IDM_BACK, L"&Back\tAlt+Left");
	AppendMenuW(go, MF_STRING, IDM_FORWARD, L"F&orward\tAlt+Right");
	AppendMenuW(g_menu, MF_POPUP, (UINT_PTR)go, L"&Go");

	HMENU help = CreatePopupMenu();
	AppendMenuW(help, MF_STRING, IDM_SHORTCUTS, L"&Keyboard Shortcuts\tF1");
	AppendMenuW(help, MF_STRING, IDM_ABOUT, L"&About");
	AppendMenuW(g_menu, MF_POPUP, (UINT_PTR)help, L"&Help");

	rebuild_recent_menu();
}

static void create_accels(void)
{
	ACCEL a[] = {
		{ FVIRTKEY | FCONTROL, 'O', IDM_OPEN },
		{ FVIRTKEY | FCONTROL, 'W', IDM_CLOSE },
		{ FVIRTKEY | FCONTROL, 'P', IDM_PRINT },
		{ FVIRTKEY | FCONTROL, 'I', IDM_PROPERTIES },
		{ FVIRTKEY, VK_F5, IDM_RELOAD },
		{ FVIRTKEY | FCONTROL, 'Q', IDM_EXIT },
		{ FVIRTKEY | FCONTROL, 'C', IDM_COPY },
		{ FVIRTKEY | FCONTROL, VK_INSERT, IDM_COPY },
		{ FVIRTKEY | FCONTROL, 'A', IDM_SELECTALL },
		{ FVIRTKEY | FCONTROL, 'F', IDM_FIND },
		{ FVIRTKEY, VK_F3, IDM_FINDNEXT },
		{ FVIRTKEY | FSHIFT, VK_F3, IDM_FINDPREV },
		{ FVIRTKEY | FCONTROL, VK_OEM_PLUS, IDM_ZOOMIN },
		{ FVIRTKEY | FCONTROL | FSHIFT, VK_OEM_PLUS, IDM_ZOOMIN },
		{ FVIRTKEY | FCONTROL, VK_ADD, IDM_ZOOMIN },
		{ FVIRTKEY | FCONTROL, VK_OEM_MINUS, IDM_ZOOMOUT },
		{ FVIRTKEY | FCONTROL, VK_SUBTRACT, IDM_ZOOMOUT },
		{ FVIRTKEY | FCONTROL, '0', IDM_FITPAGE },
		{ FVIRTKEY | FCONTROL, VK_NUMPAD0, IDM_FITPAGE },
		{ FVIRTKEY | FCONTROL, '1', IDM_ACTUAL },
		{ FVIRTKEY | FCONTROL, '2', IDM_FITWIDTH },
		{ FVIRTKEY | FCONTROL, 'R', IDM_ROTR },
		{ FVIRTKEY | FCONTROL | FSHIFT, 'R', IDM_ROTL },
		{ FVIRTKEY, VK_F9, IDM_OUTLINE },
		{ FVIRTKEY, VK_F11, IDM_FULLSCREEN },
		{ FVIRTKEY | FCONTROL, 'D', IDM_DARK },
		{ FVIRTKEY | FCONTROL, 'G', IDM_GOTO },
		{ FVIRTKEY | FALT, VK_LEFT, IDM_BACK },
		{ FVIRTKEY | FALT, VK_RIGHT, IDM_FORWARD },
		{ FVIRTKEY, VK_F1, IDM_SHORTCUTS },
	};
	g_accel = CreateAcceleratorTableW(a, sizeof a / sizeof a[0]);
}

static void update_fonts(void)
{
	NONCLIENTMETRICSW ncm = { sizeof ncm };
	if (g_ui_font) DeleteObject(g_ui_font);
	g_ui_font = NULL;
	if (SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof ncm, &ncm, 0, g_dpi))
		g_ui_font = CreateFontIndirectW(&ncm.lfMessageFont);
	if (!g_ui_font) g_ui_font = GetStockObject(DEFAULT_GUI_FONT);
	if (g_tree) SendMessageW(g_tree, WM_SETFONT, (WPARAM)g_ui_font, TRUE);
	if (g_status) SendMessageW(g_status, WM_SETFONT, (WPARAM)g_ui_font, TRUE);
}

static LRESULT CALLBACK main_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
	switch (msg) {
	case WM_CREATE:
		g_main = hwnd;
		g_dpi = GetDpiForWindow(hwnd);
		g_tree = CreateWindowExW(0, WC_TREEVIEWW, L"",
			WS_CHILD | WS_TABSTOP | TVS_HASBUTTONS | TVS_LINESATROOT | TVS_SHOWSELALWAYS | TVS_TRACKSELECT | TVS_FULLROWSELECT,
			0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)IDC_TREE, g_inst, NULL);
		TreeView_SetExtendedStyle(g_tree, TVS_EX_DOUBLEBUFFER, TVS_EX_DOUBLEBUFFER);
		g_canvas = CreateWindowExW(0, CANVAS_CLASS, L"", WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_HSCROLL | WS_TABSTOP,
			0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)IDC_CANVAS, g_inst, NULL);
		g_status = CreateWindowExW(0, STATUSCLASSNAMEW, L"", WS_CHILD | WS_VISIBLE | SBARS_SIZEGRIP,
			0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)IDC_STATUS, g_inst, NULL);
		update_fonts();
		DragAcceptFiles(hwnd, TRUE);
		return 0;
	case WM_SIZE:
		layout_children();
		return 0;
	case WM_DPICHANGED: {
		Anchor a = { -1 };
		int vw, vh;
		client_size(&vw, &vh);
		if (g_doc) a = anchor_at(vw / 2, vh / 3);
		g_dpi = HIWORD(wp);
		update_fonts();
		cache_clear();
		RECT *r = (RECT *)lp;
		SetWindowPos(hwnd, NULL, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
		layout_children();
		layout();
		client_size(&vw, &vh);
		a.cx = vw / 2; a.cy = vh / 3;
		anchor_restore(a);
		return 0;
	}
	case WM_SETFOCUS:
		SetFocus(g_canvas);
		return 0;
	case WM_COMMAND:
		command(LOWORD(wp));
		return 0;
	case WM_NOTIFY: {
		NMHDR *nh = (NMHDR *)lp;
		if (nh->hwndFrom == g_tree && nh->code == TVN_SELCHANGEDW) {
			NMTREEVIEWW *tv = (NMTREEVIEWW *)lp;
			if (tv->action == TVC_BYMOUSE || tv->action == TVC_BYKEYBOARD)
				outline_go((fz_outline *)tv->itemNew.lParam);
		} else if (nh->hwndFrom == g_tree && nh->code == NM_CLICK) {
			/* clicking the already-selected item should still navigate */
			TVHITTESTINFO ht;
			DWORD pos = GetMessagePos();
			ht.pt.x = GET_X_LPARAM(pos); ht.pt.y = GET_Y_LPARAM(pos);
			ScreenToClient(g_tree, &ht.pt);
			HTREEITEM it = TreeView_HitTest(g_tree, &ht);
			if (it && (ht.flags & TVHT_ONITEM) && it == TreeView_GetSelection(g_tree)) {
				TVITEMW item = { TVIF_PARAM };
				item.hItem = it;
				if (TreeView_GetItem(g_tree, &item)) outline_go((fz_outline *)item.lParam);
			}
		}
		return 0;
	}
	case WM_DROPFILES: {
		HDROP drop = (HDROP)wp;
		UINT n = DragQueryFileW(drop, 0, NULL, 0);
		wchar_t *p = malloc((n + 1) * sizeof(wchar_t));
		if (p && DragQueryFileW(drop, 0, p, n + 1)) open_document(p, -1, FALSE);
		free(p);
		DragFinish(drop);
		return 0;
	}
	case WM_SETCURSOR:
		if ((HWND)wp == hwnd && LOWORD(lp) == HTCLIENT) {
			POINT p;
			GetCursorPos(&p);
			ScreenToClient(hwnd, &p);
			if (over_splitter(p.x, p.y)) { SetCursor(LoadCursor(NULL, IDC_SIZEWE)); return TRUE; }
		}
		break;
	case WM_LBUTTONDOWN:
		if (over_splitter(GET_X_LPARAM(lp), GET_Y_LPARAM(lp))) { g_split_drag = TRUE; SetCapture(hwnd); }
		return 0;
	case WM_MOUSEMOVE:
		if (g_split_drag) {
			int x = GET_X_LPARAM(lp);
			int w = MulDiv(x, 96, g_dpi);
			if (w < 80) w = 80;
			if (w > 1200) w = 1200;
			g_sidebar_w = w;
			layout_children();
		}
		return 0;
	case WM_LBUTTONUP:
		if (g_split_drag) { g_split_drag = FALSE; ReleaseCapture(); }
		return 0;
	case WM_CLOSE:
		save_settings();
		close_document();
		DestroyWindow(hwnd);
		return 0;
	case WM_DESTROY:
		PostQuitMessage(0);
		return 0;
	}
	return DefWindowProcW(hwnd, msg, wp, lp);
}

/* ------------------------------------------------------------------ */
/* Entry point                                                         */
/* ------------------------------------------------------------------ */

static void quiet(void *user, const char *message) { (void)user; (void)message; }

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, PWSTR cmdline, int show)
{
	(void)prev; (void)cmdline;
	g_inst = inst;
	for (int i = 0; i < MAX_TILES; i++) g_tiles[i].page = -1;

	INITCOMMONCONTROLSEX icc = { sizeof icc, ICC_TREEVIEW_CLASSES | ICC_BAR_CLASSES };
	InitCommonControlsEx(&icc);

	g_ctx = fz_new_context(NULL, NULL, STORE_LIMIT);
	if (!g_ctx) {
		MessageBoxW(NULL, L"Could not initialise the PDF engine.", APP_NAME, MB_ICONERROR);
		return 1;
	}
	fz_set_warning_callback(g_ctx, quiet, NULL);
	fz_set_error_callback(g_ctx, quiet, NULL);
	fz_try(g_ctx) fz_register_document_handlers(g_ctx);
	fz_catch(g_ctx) {
		MessageBoxW(NULL, L"Could not initialise the PDF engine.", APP_NAME, MB_ICONERROR);
		return 1;
	}

	init_ini_path();
	load_settings();

	WNDCLASSEXW wc;
	memset(&wc, 0, sizeof wc);
	wc.cbSize = sizeof wc;
	wc.lpfnWndProc = canvas_proc;
	wc.hInstance = inst;
	wc.hCursor = NULL;   /* the canvas sets its own cursor (I-beam, hand, ...) */
	wc.style = CS_DBLCLKS;
	wc.lpszClassName = CANVAS_CLASS;
	RegisterClassExW(&wc);

	wc.lpfnWndProc = main_proc;
	wc.style = 0;
	wc.hCursor = LoadCursor(NULL, IDC_ARROW);
	wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(IDI_APP));
	wc.hIconSm = wc.hIcon;
	wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
	wc.lpszClassName = MAIN_CLASS;
	RegisterClassExW(&wc);

	create_menu();
	create_accels();

	int x = ini_int(L"WinX", CW_USEDEFAULT), y = ini_int(L"WinY", CW_USEDEFAULT);
	int w = ini_int(L"WinW", 1000), h = ini_int(L"WinH", 800);
	if (w < 300 || h < 200) { w = 1000; h = 800; }
	/* make sure a saved position is still on some monitor */
	if (x != CW_USEDEFAULT) {
		RECT r = { x, y, x + w, y + h };
		if (!MonitorFromRect(&r, MONITOR_DEFAULTTONULL)) x = y = CW_USEDEFAULT;
	}
	CreateWindowExW(0, MAIN_CLASS, APP_NAME, WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
		x, y, w, h, NULL, g_menu, inst, NULL);
	if (!g_main) return 1;
	update_menu_checks();
	ShowWindow(g_main, ini_int(L"WinMax", 0) && show != SW_SHOWMINNOACTIVE && show != SW_SHOWMINIMIZED ? SW_SHOWMAXIMIZED : show);
	UpdateWindow(g_main);
	layout_children();

	/* command line: FeatherPDF.exe [-page N] file.pdf */
	int argc = 0;
	wchar_t **argv = CommandLineToArgvW(GetCommandLineW(), &argc);
	int start_page = -1;
	for (int i = 1; argv && i < argc; i++) {
		if ((!wcscmp(argv[i], L"-page") || !wcscmp(argv[i], L"/page")) && i + 1 < argc) {
			start_page = _wtoi(argv[++i]) - 1;
			continue;
		}
		open_document(argv[i], start_page, FALSE);
		break;
	}
	if (argv) LocalFree(argv);
	update_status();

	MSG m;
	while (GetMessageW(&m, NULL, 0, 0) > 0) {
		if (g_find_dlg && IsWindowVisible(g_find_dlg) && IsDialogMessageW(g_find_dlg, &m)) {
			/* let F3 work while typing in the find box */
			if (m.message == WM_KEYDOWN && m.wParam == VK_F3) find(GetKeyState(VK_SHIFT) < 0 ? -1 : +1);
			continue;
		}
		if (TranslateAcceleratorW(g_main, g_accel, &m)) continue;
		TranslateMessage(&m);
		DispatchMessageW(&m);
	}

	if (g_ui_font) DeleteObject(g_ui_font);
	fz_drop_context(g_ctx);
	return 0;
}
