#include "wf_mddraw.h"

#include "wf_theme.h"

WfStyle wf_md_style(unsigned bits)
{
	const WfThemePalette *p = wf_theme_active();
	WfStyle s = {p->foreground, p->background, 0};
	unsigned level = WF_MDS_HEAD(bits);

	switch (level) {
	case 1:
		s.fg_rgb = p->accent;
		s.attributes |= WF_ATTR_BOLD | WF_ATTR_UNDERLINE;
		break;
	case 2:
		s.fg_rgb = p->accent;
		s.attributes |= WF_ATTR_BOLD;
		break;
	case 3: s.attributes |= WF_ATTR_BOLD; break;
	case 4: s.attributes |= WF_ATTR_UNDERLINE; break;
	case 5: s.attributes |= WF_ATTR_ITALIC; break;
	case 6:
		s.fg_rgb = p->muted_foreground;
		s.attributes |= WF_ATTR_DIM;
		break;
	default: break;
	}
	if (bits & WF_MDS_QUOTE) {
		s.fg_rgb = p->muted_foreground;
	}
	if (bits & WF_MDS_BOLD) {
		s.attributes |= WF_ATTR_BOLD;
	}
	if (bits & WF_MDS_ITALIC) {
		s.attributes |= WF_ATTR_ITALIC;
	}
	if (bits & WF_MDS_STRIKE) {
		s.attributes |= WF_ATTR_DIM;
	}
	if (bits & (WF_MDS_CODE | WF_MDS_BLOCK)) {
		s.fg_rgb = p->foreground;
		s.bg_rgb = p->surface;
	}
	if (bits & WF_MDS_LINK) {
		s.fg_rgb = p->accent;
		s.attributes |= WF_ATTR_UNDERLINE;
	}
	if (bits & WF_MDS_URL) {
		s.fg_rgb = p->muted_foreground;
		s.attributes |= WF_ATTR_DIM;
	}
	if (bits & (WF_MDS_MARKER | WF_MDS_IMAGE)) {
		s.fg_rgb = p->muted_foreground;
	}
	if (bits & WF_MDS_IMAGE) {
		s.attributes |= WF_ATTR_ITALIC;
	}
	return s;
}

void wf_md_draw(
        WfFrame *f, int x, int y, int w, int h, const WfMdDoc *d, size_t top)
{
	if (!f || !d || w <= 0 || h <= 0) {
		return;
	}
	for (int row = 0; row < h && top + (size_t)row < d->nlines; row++) {
		const WfMdLine *l = &d->lines[top + (size_t)row];
		int col = 0;

		if (l->flags & WF_MDL_CODE) {
			WfStyle code = wf_md_style(WF_MDS_BLOCK);

			wf_frame_fill(f, x, y + row, w, 1, ' ', &code);
		}
		for (uint32_t r = 0; r < l->nruns && col < w; r++) {
			const WfMdRun *run = &d->runs[l->run + r];
			WfStyle st = wf_md_style(run->style);

			col += wf_frame_put(f, x + col, y + row,
			        d->text + run->off, run->len, &st, w - col);
		}
	}
}
