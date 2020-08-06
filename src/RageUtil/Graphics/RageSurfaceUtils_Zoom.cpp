#include "Etterna/Globals/global.h"
#include "RageSurface.h"
#include "RageSurfaceUtils_Zoom.h"
#include "RageUtil/Utils/RageUtil.h"
#define STB_IMAGE_RESIZE_IMPLEMENTATION
#include <stb/stb_image_resize.h>
#include <algorithm>

static void
InitVectors(vector<int>& s0,
			vector<int>& s1,
			vector<uint32_t>& percent,
			int src,
			int dst)
{
	if (src >= dst) {
		float sx = float(src) / dst;
		for (int x = 0; x < dst; x++) {
			/* sax is the exact (floating-point) x coordinate in the source
			 * that the destination pixel at x should from.  For example, if
			 * we're going 512->256, then dst[0] should come from the pixels
			 * from 0..1 and 1..2, so sax[0] is 1. sx is the total number of
			 * pixels, so sx/2 is the distance from the start of the sample to
			 * its center. */
			const float sax = sx * x + sx / 2.0f;

			/* sx/2 is the distance from the start of the sample to the center;
			 * sx/4 is the distance from the center of the sample to the center
			 * of either pixel. */
			const float xstep = sx / 4.0f;

			// source x coordinates of left and right pixels to sample
			s0.emplace_back(static_cast<int>(sax - xstep));
			s1.emplace_back(static_cast<int>(sax + xstep));

			if (s0[x] == s1[x]) {
				/* If the sampled pixels happen to be the same, the distance
				 * will be 0.  Avoid division by zero. */
				percent.emplace_back(1 << 24);
			} else {
				const int xdist = s1[x] - s0[x];

				// fleft is the left pixel sampled; +.5 is the center:
				const float fleft = s0[x] + .5f;

				/* sax is somewhere between the centers of both sampled
				 * pixels; find the percentage: */
				percent.emplace_back(
				  uint32_t((1.0f - (sax - fleft) / xdist) * 16777216.0f));
			}
		}
	} else {
		/* Fencepost: If we have source:
		 *    abcd
		 * and dest:
		 *    xyz
		 * then we want x to be sampled entirely from a, and z entirely from d;
		 * the inner pixels are interpolated.  (This behavior mimics Photoshop's
		 * resize.) */
		float sx = float(src - 1) / (dst - 1);
		for (int x = 0; x < dst; x++) {
			const float sax = sx * x;

			// source x coordinates of left and right pixels to sample
			s0.emplace_back(std::clamp(static_cast<int>(sax), 0, src - 1));
			s1.emplace_back(std::clamp(static_cast<int>(sax + 1), 0, src - 1));
			percent.emplace_back(
			  uint32_t((1.0f - (sax - floorf(sax))) * 16777216.0f));
		}
	}
}

static void
ZoomSurface(const RageSurface* src, RageSurface* dst)
{
	/* For each destination coordinate, two source rows, two source columns
	 * and the percentage of the first row and first column: */
	vector<int> esx0, esx1, esy0, esy1;
	vector<uint32_t> ex0, ey0;

	InitVectors(esx0, esx1, ex0, src->w, dst->w);
	InitVectors(esy0, esy1, ey0, src->h, dst->h);

	// This is where all of the real work is done.
	const uint8_t* sp = (uint8_t*)src->pixels;
	const int height = dst->h;
	const int width = dst->w;
	for (int y = 0; y < height; y++) {
		uint8_t* dp = (uint8_t*)(dst->pixels + dst->pitch * y);
		/* current source pointer and next source pointer (first and second
		 * rows sampled for this row): */
		const uint8_t* csp = sp + esy0[y] * src->pitch;
		const uint8_t* ncsp = sp + esy1[y] * src->pitch;

		for (int x = 0; x < width; x++) {
			// Grab pointers to the sampled pixels:
			const uint8_t* c00 = csp + esx0[x] * 4;
			const uint8_t* c01 = csp + esx1[x] * 4;
			const uint8_t* c10 = ncsp + esx0[x] * 4;
			const uint8_t* c11 = ncsp + esx1[x] * 4;

			for (int c = 0; c < 4; ++c) {
				uint32_t x0 = uint32_t(c00[c]) * ex0[x];
				x0 += uint32_t(c01[c]) * (16777216 - ex0[x]);
				x0 >>= 24;
				uint32_t x1 = uint32_t(c10[c]) * ex0[x];
				x1 += uint32_t(c11[c]) * (16777216 - ex0[x]);
				x1 >>= 24;

				const uint32_t res =
				  ((x0 * ey0[y]) + (x1 * (16777216 - ey0[y])) + 8388608) >> 24;
				dp[c] = uint8_t(res);
			}

			// Advance destination pointer.
			dp += 4;
		}
	}
}

void
RageSurfaceUtils::Zoom(RageSurface*& src, int dstwidth, int dstheight)
{
	ASSERT_M(dstwidth > 0, ssprintf("%i", dstwidth));
	ASSERT_M(dstheight > 0, ssprintf("%i", dstheight));
	if (src == nullptr)
		return;

	if (src->w == dstwidth && src->h == dstheight)
		return;

	RageSurface* dst = nullptr;
	if (src->stb_loadpoint) {
		dst = CreateSurface(dstwidth,
							dstheight,
							32,
							src->fmt.Rmask,
							src->fmt.Gmask,
							src->fmt.Bmask,
							src->fmt.Amask);
		stbir_resize_uint8(src->pixels,
						   src->w,
						   src->h,
						   0,
						   dst->pixels,
						   dstwidth,
						   dstheight,
						   0,
						   4);
	} else if (src->svg_loaded) {
		// the old algorithm here did an iterative process to make it less bad
		// but we take shortcuts around here at the cost of it looking good
		dst = CreateSurface(dstwidth,
							dstheight,
							32,
							src->fmt.Rmask,
							src->fmt.Gmask,
							src->fmt.Bmask,
							src->fmt.Amask);
		ZoomSurface(src, dst);
	}

	delete src;
	src = dst;
}
