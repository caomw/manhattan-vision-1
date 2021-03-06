#include <boost/bind.hpp>
#include <boost/foreach.hpp>

#include "fast_sobel.h"
#include "common_types.h"
#include "worker.h"

#include "vw_image.tpp"

namespace indoor_context {
	using boost::bind;
	using boost::ref;

// Binary flag indicating whether or not to parallelize sobel convolutions
lazyvar<int> gvParallelize("Sobel.Parallelize");

// High speed implementation of y-sobel filter
void FastSobel::ConvolveRowX(const int w,
                             const PixelF* r1,
                             const PixelF* r2,
                             const PixelF* r3,
                             PixelF* out) {
	// Run the first one explicitly to avoid *any* branches inside the loop
	out[0].y = r1[0].y-r1[1].y + 2*(r2[0].y-r2[1].y) + r3[0].y-r3[1].y;
	for (int c = 1; c < w-1; c++) {
		out[c].y = r1[c-1].y-r1[c+1].y + 2*(r2[c-1].y-r2[c+1].y) + r3[c-1].y-r3[c+1].y;
	}
	out[w-1].y = r1[w-2].y-r1[w-1].y + 2*(r2[w-2].y-r2[w-1].y) + r3[w-2].y-r3[w-1].y;
}

// High speed implementation of y-sobel filter
void FastSobel::ConvolveRowY(const int w,
                             const PixelF* r1,
                             const PixelF* r2,
                             const PixelF* r3,
                             PixelF* out) {
	// Run the first one explicitly to avoid *any* branches inside the loop
	out[0].y = 3*r1[0].y + r1[1].y - 3*r3[0].y - r3[1].y;
	for (int c = 1; c < w-1; c++) {
		out[c].y = r1[c-1].y + 2*r1[c].y + r1[c+1].y - r3[c-1].y - 2*r3[c].y - r3[c+1].y;
	}
	out[w-1].y = r1[w-2].y + 3*r1[w-1].y - r3[w-2].y - 3*r3[w-1].y;
}

// Run an operation over a set of rows
void FastSobel::ConvolveRowRange(const ImageF& input,
                                 ImageF& output,
                                 const int direction,
                                 const int r0,
                                 const int r1) {
	//void(*rowfunc)(int,const PixelF*,const PixelF*,const PixelF*,PixelF*);
	//rowfunc = (direction == kSobelX) ? &ConvolveRowX : &ConvolveRowY;
	for (int r = r0; r <= r1; r++) {
		const PixelF* rprev = r == 0 ? input[r] : input[r-1];
		const PixelF* rnext = r == input.GetHeight()-1 ? input[r] : input[r+1];
		if (direction == kSobelX) {
			ConvolveRowX(input.GetWidth(), rprev, input[r], rnext, output[r]);
		} else {
			ConvolveRowY(input.GetWidth(), rprev, input[r], rnext, output[r]);
		}
	}
}

// Run a tweaked sobel convolution
void FastSobel::Convolve(const ImageF& input,
                         ImageF& output,
                         const int direction) {
	if (*gvParallelize) {
		ParallelPartition(input.GetHeight(),
				bind(&ConvolveRowRange,
						ref(input),
						ref(output),
						direction,
						_1,
						_2));
	} else {
		ConvolveRowRange(input, output, direction, 0, input.GetHeight()-1);
	}
}

}
