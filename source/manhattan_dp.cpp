#include <tr1/unordered_map>
#include <tr1/functional>

#include <LU.h>

#include "manhattan_dp.h"
#include "common_types.h"
#include "map.h"
#include "camera.h"
#include "timer.h"
#include "clipping.h"
#include "canvas.h"
#include "bld_helpers.h"
#include "geom_utils.h"
#include "line_segment.h"
#include "monocular_payoffs.h"

#include "fill_polygon.tpp"
#include "numeric_utils.tpp"
#include "io_utils.tpp"
#include "image_utils.tpp"
#include "image_transforms.tpp"
#include "range_utils.tpp"
#include "vector_utils.tpp"
#include "histogram.tpp"
#include "matrix_traits.tpp"

namespace indoor_context {
using namespace toon;

	lazyvar<Vec2> gvGridSize("ManhattanDP.GridSize");
	lazyvar<float> gvLineJumpThreshold("ManhattanDP.LineJumpThreshold");

	////////////////////////////////////////////////////////////////////////////////
	DPState::DPState() : row(-1), col(-1), axis(-1), dir(-1) { }
	DPState::DPState(int r, int c, int a, int d)  // removed 'b' before 'd' here
		: row(r), col(c), axis(a), dir(d) {	}
	bool DPState::operator==(const DPState& other) const {
		return
			row == other.row &&
			col == other.col &&
			axis == other.axis &&
			dir == other.dir;
	}
	bool DPState::operator!=(const DPState& other) const {
		return
			row != other.row ||
			col != other.col ||
			axis != other.axis ||
			dir != other.dir;
	}
	const DPState DPState::none;

	size_t DPStateHasher::operator()(const DPState& dpstate) const {
		return tr1::_Fnv_hash::hash(reinterpret_cast<const char*>(&dpstate),
																sizeof(dpstate));
	}

	ostream& operator<<(ostream& s, const DPState& x) {
		s << "{r=" << x.row << ",c=" << x.col<< ",axis=" << x.axis << ",dir=";
		switch (x.dir) {
		case DPState::DIR_IN: s << "DIR_IN"; break;
		case DPState::DIR_OUT: s << "DIR_OUT"; break;
		case DPState::DIR_UP: s << "DIR_UP"; break;
		case DPState::DIR_DOWN: s << "DIR_DOWN"; break;
		}
		s << "}";
		return s;
	}

	////////////////////////////////////////////////////////////////////////////////
	DPSubSolution::DPSubSolution()
	: score(numeric_limits<double>::quiet_NaN()), src(DPState::none) { }
	DPSubSolution::DPSubSolution(double s)
	: score(s), src(DPState::none) { }
	DPSubSolution::DPSubSolution(double s, const DPState& state)
	: score(s), src(state) { }

	bool DPSubSolution::ReplaceIfSuperior(const DPSubSolution& other,
																				const DPState& state,
																				double delta) {
		if (other.score+delta > score) {
			score = other.score+delta;
			src = state;
			return true;
		}
		return false;
	}

	ostream& operator<<(ostream& s, const DPSubSolution& x) {
		s << "<score=" << x.score << ", src=" << x.src << ">";
		return s;
	}


	////////////////////////////////////////////////////////////////////////////////
	void DPCache::reset(const Vector<2,int>& grid_size) {
		// This ordering helps the OS to do locality-based caching
		// We have one more column in the table than in the grid
		table.Resize(grid_size[0]+1, grid_size[1], 2, 4);
		// Resize is a no-op if the size is the same as last time, so do a clear()
		clear();
	}

	void DPCache::clear() {
		table.Fill(DPSubSolution());
	}

	DPCache::const_iterator DPCache::find(const DPState& x) const {
		// don't be tempted to cache the value of end() here as it will
		// change from one iteration to the next.
		iterator y = &(const_cast<DPCache&>(*this)[x]);
		return isnan(y->score) ? end() : y;
	}

	DPCache::iterator DPCache::find(const DPState& x) {
		// don't be tempted to cache the value of end() here as it will
		// change from one iteration to the next.
		iterator y = &(*this)[x];
		return isnan(y->score) ? end() : y;
	}

	DPSubSolution& DPCache::operator[](const DPState& x) {
		// This particular ordering helps the OS to do locality-based caching
		return table(x.col, x.row, x.axis, x.dir);
	}


	////////////////////////////////////////////////////////////////////////////////
	double DPSolution::GetTotalPayoff(const DPPayoffs& payoffs,
																		bool subtract_penalties) const {
		CHECK_EQ(path_ys.Size(), payoffs.wall_scores[0].Cols());
		CHECK_EQ(path_axes.Size(), payoffs.wall_scores[0].Cols());
		CHECK_SAME_SIZE(payoffs.wall_scores[0], payoffs.wall_scores[1]);

		double score = 0.0;
		for (int x = 0; x < path_ys.Size(); x++) {
			score += payoffs.wall_scores[ path_axes[x] ][ path_ys[x] ][ x ];
		}
		if (subtract_penalties) {
			score -= num_walls * payoffs.wall_penalty;
			score -= num_occlusions * payoffs.occl_penalty;
		}
		return score;
	}

	double DPSolution::GetPathSum(const MatF& payoffs) const {
		CHECK_EQ(path_ys.Size(), payoffs.Cols());
		CHECK_EQ(path_axes.Size(), payoffs.Cols());

		double score = 0.0;
		for (int x = 0; x < path_ys.Size(); x++) {
			score += payoffs[ path_ys[x] ][ x ];
		}
		return score;
	}

	const MatD& DPSolution::GetDepthMap(const DPGeometryWithScale& geometry) {
		CHECK(!wall_segments.empty());
		CHECK_NOT_NULL(geometry.camera);

		// Push each polygon through the renderer
		renderer.Configure(*geometry.camera);
		renderer.RenderInfinitePlane(geometry.zfloor, kVerticalAxis);
		renderer.RenderInfinitePlane(geometry.zceil, kVerticalAxis); 
		BOOST_FOREACH(const LineSeg& wall, wall_segments) {
			Vec3 l = geometry.BackProject(wall.start);
			Vec3 r = geometry.BackProject(wall.end);
			Vec3 tl = makeVector(l[0], l[1], geometry.zceil);
			Vec3 tr = makeVector(r[0], r[1], geometry.zceil);
			Vec3 bl = makeVector(l[0], l[1], geometry.zfloor);
			Vec3 br = makeVector(r[0], r[1], geometry.zfloor);
			renderer.Render(tl, br, tr, 0);  // we're only interested in depth
			renderer.Render(tl, br, bl, 0);
		}

		// Hack to remove infinities near horizon or at image borders caused
		// by clipping errors
		renderer.SmoothInfiniteDepths();

		// Return the depth buffer
		return renderer.depthbuffer();
	}

	////////////////////////////////////////////////////////////////////////////////
	DPGeometry::DPGeometry() : camera(NULL) {
		grid_size = *gvGridSize;
	}

	DPGeometry::DPGeometry(const PosedCamera& camera, const Mat3& floorToCeil) {
		Configure(camera, floorToCeil);
	}

	void DPGeometry::Configure(const PosedCamera& cam, const Mat3& fToC) {
		Configure(cam, fToC, *gvGridSize);
	}

	void DPGeometry::Configure(const PosedCamera& cam,
														 const Mat3& fToC,
														 const Vec2I& gridsize) {
		camera = &cam;
		floorToCeil = fToC;
		grid_size = gridsize;

		// Compute the rectification homography
		imageToGrid = GetVerticalRectifier(*camera, Bounds2D<>::FromTightSize(grid_size));
		gridToImage = LU<>(imageToGrid).get_inverse();

		// Compute floor to ceiling mapping in grid coordinates
		grid_floorToCeil = imageToGrid * floorToCeil * gridToImage;  // floorToCeil in grid coords
		grid_ceilToFloor = LU<>(grid_floorToCeil).get_inverse();

		// Locate the vanishing points in grid coordinates
		for (int i = 0; i < 3; i++) {
			Vec3 grid_vpt = imageToGrid * cam.GetImageVpt(i);
			if (abs(grid_vpt[2]) < 1e-8) {
				// hack to avoid vanishing points at infinity
				vpt_cols[i] = HalfSign(grid_vpt[0]*grid_vpt[2]) * 1e+8;
			} else {
				vpt_cols[i] = project(grid_vpt)[0];
			}
		}

		// Calculate the horizon row
		// Note that H_canon breaks the orthogonality of the vanishing
		// points, so the horzon is not guaranteed to be in the middle of
		// the image even though the vertical vanishing point is
		// guaranteed to be at infinity. The horizon is, however,
		// guaranteed to be horizontal in the image.
		Vec3 vpt0 = imageToGrid * cam.GetImageVpt(0);
		Vec3 vpt1 = imageToGrid * cam.GetImageVpt(1);
		Vec3 horizon = vpt0 ^ vpt1;

		// If the horizon is horizontal then its equation is independent of x-coord
		CHECK_LE(abs(horizon[0]), 1e-8) << "The horizon is not horizontal in the image.";

		// Compute the horizon at the left and right of the image
		Vec2 horizon_at_left = project(horizon ^ makeVector(-1,0,0));
		Vec2 horizon_at_right = project(horizon ^ makeVector(-1,0,cam.nx()));
		CHECK(!isnan(horizon_at_left))
			<< "horizon does not intersect the left image boundary!";
		CHECK(!isnan(horizon_at_right))
			<< "horizon does not intersect the right image boundary!";
		horizon_row = roundi(0.5*horizon_at_left[1] + 0.5*horizon_at_right[1]);

		// Check that image is not flipped. This should be guaranteed by
		// GetVerticalRectifier
		Vec2 floor_pt = makeVector(0, horizon_row+1);
		// Note that PosedCamera::GetImageHorizon always returns a line
		// with positive half on the floor...
		CHECK_GT(GridToImage(floor_pt) * cam.GetImageHorizon(), 0)
			<< "The matrix returned by GetVerticalRectifier flips the image!";
	}

	Vec3 DPGeometry::GridToImage(const Vec2& x) const {
		return gridToImage * unproject(x);
	}

	Vec2 DPGeometry::ImageToGrid(const Vec3& x) const {
		return project(imageToGrid * x);
	}

	Vec2 DPGeometry::Transfer(const Vec2& grid_pt) const {
		return project(Transfer(unproject(grid_pt)));
	}

	Vec3 DPGeometry::Transfer(const Vec3& grid_pt) const {
		const Mat3& m = grid_pt[1] < horizon_row ? grid_ceilToFloor : grid_floorToCeil;
		return m * grid_pt;
	}

	void DPGeometry::TransformDataToGrid(const MatF& in, MatF& out) const {
		CHECK_EQ(matrix_size(in), camera->image_size());
		out.Resize(grid_size[1], grid_size[0], 0.);

		// Check that the four corners project within the grid bounds
		CHECK_POS(ImageToGrid(makeVector(0, 0, 1)), out)
			<< "in size="<<matrix_size(in);
		CHECK_POS(ImageToGrid(makeVector(0, in.Rows()-1, 1)), out)
			<< "in size="<<matrix_size(in);;
		CHECK_POS(ImageToGrid(makeVector(in.Cols()-1, 0, 1)), out)
			<< "in size="<<matrix_size(in);;
		CHECK_POS(ImageToGrid(makeVector(in.Cols()-1, in.Rows()-1, 1)), out)
			<< "in size="<<matrix_size(in);;

		// Do the transform
		for (int y = 0; y < in.Rows(); y++) {
			const float* inrow = in[y];
			for (int x = 0; x < in.Cols(); x++) {
				Vec2I grid_pt = RoundVector(ImageToGrid(makeVector(x, y, 1.0)));
				out[ grid_pt[1] ][ grid_pt[0] ] += inrow[x];
			}
		}
	}

	void DPGeometry::ComputeGridImportances(MatF& out) const {
		MatF ones(camera->image_size()[1], camera->image_size()[0], 1.);
		TransformDataToGrid(ones, out);
	}

	void DPGeometry::TransformToGrid(const ImageRGB<byte>& in,
																	 ImageRGB<byte>& out) const {
		ResizeImage(out, grid_size[0], grid_size[1]);
		out.Clear(Colors::black());
		TransformImage(in, out, imageToGrid);
	}

	void DPGeometry::GetWallExtent(const Vec2& grid_pt, int& y0, int& y1) const {
		Vec2 opp_pt = Transfer(grid_pt);
		int opp_y = Clamp<int>(opp_pt[1], 0, grid_size[1]-1);

		// Rounding and clamping must come after Transfer()
		int y = Clamp<int>(roundi(grid_pt[1]), 0, grid_size[1]-1);

		// Swap if necessary
		y0 = min(y, opp_y);
		y1 = max(y, opp_y);
	}

	void DPGeometry::GetWallExtentUnclamped(const Vec2& grid_pt,
																					float& y0,
																					float& y1) const {
		float opp_y = Transfer(grid_pt)[1];
		y0 = min(grid_pt[1], opp_y);
		y1 = max(grid_pt[1], opp_y);
	}
		

	void DPGeometry::PathToOrients(const VecI& path_ys,
																 const VecI& path_axes,
																 MatI& grid_orients) const {
		CHECK_EQ(path_ys.Size(), grid_size[0]);
		CHECK_EQ(path_axes.Size(), grid_size[0]);
		grid_orients.Resize(grid_size[1], grid_size[0]);
		VecI y0s(grid_size[0]);
		VecI y1s(grid_size[0]);
		for (int x = 0; x < grid_size[0]; x++) {
			CHECK_NE(path_ys[x], -1) << "This should have been caught in ComputeSolutionPath";
			CHECK_NE(path_axes[x], -1) << "This should have been caught in ComputeSolutionPath";
			GetWallExtent(makeVector(x, path_ys[x]), y0s[x], y1s[x]);
		}
		for (int y = 0; y < grid_size[1]; y++) {
			int* row = grid_orients[y];
			for (int x = 0; x < grid_size[0]; x++) {
				int orient = 1-path_axes[x];  // axes and orients are inverses of one another
				row[x] = (y >= y0s[x] && y <= y1s[x]) ? orient : kVerticalAxis;
			}
		}
	}

	////////////////////////////////////////////////////////////////////////////////
	DPGeometryWithScale::DPGeometryWithScale() {
	}

	DPGeometryWithScale::DPGeometryWithScale(const PosedCamera& camera,
																					 double zfloor,
																					 double zceil) {
		Configure(camera, zfloor, zceil);
	}

	DPGeometryWithScale::DPGeometryWithScale(const DPGeometry& geom,
																					 double zfloor,
																					 double zceil) {
		Configure(geom, zfloor, zceil);
	}

	void DPGeometryWithScale::Configure(const PosedCamera& cam,
																			double zf,
																			double zc) {
		zfloor = zf;
		zceil = zc;
		DPGeometry::Configure(cam, GetManhattanHomology(cam, zfloor, zceil));
	}

	void DPGeometryWithScale::Configure(const PosedCamera& cam,
																			double zf,
																			double zc,
																			const Vec2I& grid_size) {
		zfloor = zf;
		zceil = zc;
		DPGeometry::Configure(cam,
													GetManhattanHomology(cam, zfloor, zceil),
													grid_size);
	}

	void DPGeometryWithScale::Configure(const DPGeometry& geom,
																			double zfloor,
																			double zceil) {
		Configure(*geom.camera, zfloor, zceil, geom.grid_size);
	}

	Vec3 DPGeometryWithScale::BackProject(const Vec3& image_point) const {
		double y0 = ImageToGrid(image_point)[1];
		double plane_z = y0 < horizon_row ? zceil : zfloor;
		Vec4 plane = makeVector(0, 0, 1, -plane_z);
		return IntersectRay(image_point, *camera, plane);
	}

	Vec3 DPGeometryWithScale::BackProjectFromGrid(const Vec2& grid_point) const {
		return BackProject(GridToImage(grid_point));
	}

	////////////////////////////////////////////////////////////////////////////////
	ManhattanDP::ManhattanDP() : geom(NULL) {
		jump_thresh = *gvLineJumpThreshold;
	}

	void ManhattanDP::Compute(const DPPayoffs& po,
														const DPGeometry& geometry) {
		geom = &geometry;
		payoffs = &po;
		if (payoffs->wall_penalty < 0) {
			DLOG << "Warning: wall_penalty less than zero: " << payoffs->wall_penalty;
		}
		// Note that occlusion penalty could feasibly be negative
		// (e.g if occlusions are more likely than normal corners)
		if (payoffs->wall_penalty + payoffs->occl_penalty < 0) {
			DLOG << "Warning: wall_penalty + occl_penalty less than zero: "
					 << payoffs->wall_penalty + payoffs->occl_penalty;
		}

		// Reset the cache
		cache_lookups = 0;
		cache_hits = 0;
		cache.reset(geom->grid_size);

		// Begin the search
		DPSubSolution best(-INFINITY);
		int x_init = geom->grid_size[0]; //yes, x-coord is _past_ the image boundary
		DPState init(-1, x_init, -1, DPState::DIR_OUT);
		max_depth = cur_depth = 0;
		bool feasible = false;
		for (init.axis = 0; init.axis <= 1; init.axis++) {
			for (init.row = 0; init.row < geom->grid_size[1]; init.row++) {
				// Need to account for the penalty for the first wall here since
				// Solve_Impl() adds penalties on DIR_IN nodes.
				if (best.ReplaceIfSuperior(Solve(init), init, -payoffs->wall_penalty)) {
					feasible = true;
				}
			}
		}
		CHECK(feasible) << "No feasible solution found";

		// Backtrack from the solution
		PopulateSolution(best);	
	}

	const DPSubSolution& ManhattanDP::Solve(const DPState& state) {
		cache_lookups++;

		// unordered_map::insert actually does a lookup first and returns
		// an existing element if there is one, or returns an iterator
		// pointing to a newly inserted element if not.
		DPCache::iterator it = cache.find(state);
		if (it == cache.end()) {
			cur_depth++;
			max_depth = max(max_depth, cur_depth);
			const DPSubSolution& soln = (cache[state] = Solve_Impl(state));  // single equals sign intended!
			cur_depth--;
			return soln;
		} else {
			// The key was already in the map, return the precomputed value
			cache_hits++;
			return *it;
		}
	}

	DPSubSolution ManhattanDP::Solve_Impl(const DPState& state) {
		// This function is one of the few places where micro-optimizations
		// make a real difference.

		DPSubSolution best(-INFINITY);
		if (state.col == 0) {
			// base case
			best.score = 0;

		} else if (state.dir == DPState::DIR_IN) {
			DPState next = state;
			double occl_wall_penalty = payoffs->wall_penalty+payoffs->occl_penalty;
			for (next.axis = 0; next.axis <= 1; next.axis++) {
				// Try going out from this point directly
				next.dir = DPState::DIR_OUT;
				best.ReplaceIfSuperior(Solve(next), next, -payoffs->wall_penalty);

				// Try going up from here
				next.dir = DPState::DIR_UP;
				if (CanMoveVert(state, next)) {
					best.ReplaceIfSuperior(Solve(next), next, -occl_wall_penalty);
				}

				// Try going down from here
				next.dir = DPState::DIR_DOWN;
				if (CanMoveVert(state, next)) {
					best.ReplaceIfSuperior(Solve(next), next, -occl_wall_penalty);
				}
			}

		} else if (state.dir == DPState::DIR_UP || state.dir == DPState::DIR_DOWN) {
			// Convention is that state.axis now indicates the axis we
			// _must_ go out on, and the validity check has already been
			// done (see DIR_IN case)

			// Try going out
			DPState next_out = state;
			next_out.dir = DPState::DIR_OUT;
			best.ReplaceIfSuperior(Solve(next_out), next_out);

			// Try going up/down. Never cross the horizon or the image bounds.
			int next_row = state.row + (state.dir == DPState::DIR_UP ? -1 : 1);
			if (next_row != geom->horizon_row &&
					next_row >= 0 &&
					next_row < geom->grid_size[1]) {
				DPState next = state;
				next.row = next_row;
				best.ReplaceIfSuperior(Solve(next), next);
			}

		} else if (state.dir == DPState::DIR_OUT) {
			DPState next = state;
			next.dir = DPState::DIR_IN;

			// TODO: we could just restrict the minimum length of a wall to N
			// pixels. If the line-jump threshold is satisfied for N then we
			// avoid a loop altogether.

			double delta_score = 0.0;
			int vpt_col = geom->vpt_cols[state.axis];
			if (state.col != vpt_col) {  // don't try to reconstruct perfectly oblique surfaces

				// *** To implement nonlinear spacing of grid rows, need to
				// replace state.row here with the corresponding y coordinate
				double m = (state.row-geom->horizon_row)/static_cast<double>(state.col-vpt_col);
				double c = geom->horizon_row - m*vpt_col;

				for (next.col = state.col-1; next.col >= 0; next.col--) {
					// Check that we don't cross the vpt
					if (next.col == vpt_col) break;

					// Compute the new row
					double next_y = m*next.col + c;

					// *** To implement nonlinear spacing of grid rows, need to
					// replace roundi() here with something that finds the closest
					// row to next_y
					// TODO: linearly interpolate between payoffs rather than rounding
					next.row = roundi(next_y);

					// Check bounds and that we don't cross the horizon
					if (next.row < 0 || next.row >= geom->grid_size[1] || next.row == geom->horizon_row) break;

					// Compute cost
					delta_score += payoffs->wall_scores[next.axis][next.row][next.col];

					// Recurse
					best.ReplaceIfSuperior(Solve(next), next, delta_score);

					// Compute the error associated with jumping to the nearest (integer-valued) pixel
					// *** for nonlinear spacing, replace next.row here and below
					double jump_error = abs(next.row - next_y);
					double dist = abs(next.row-state.row)+abs(next.col-state.col);  // L1 norm for efficiency
					double rel_jump_error = jump_error / dist;

					// If the error is sufficiently small then allow the line to
					// continue with a slight "kink". This approximation
					// reduces overall complexity from O( W*H*(W+H) ) to O(W*H)
					if (rel_jump_error < jump_thresh) {
						// we just continue from this point -- don't add an intersection
						next.dir = DPState::DIR_OUT;
						best.ReplaceIfSuperior(Solve(next), next, delta_score);
						// The above recursion has already (approximately)
						// considered all further points along the line so there is
						// no need to continue. Note that we break here regardless
						// of whether this solution replaced the best so far because
						// if this solution did not replace the best so far then
						// nothing along this line will do so.
						break;
					}
				}
			}
		}

		return best;
	}

	void ManhattanDP::PopulateSolution(const DPSubSolution& soln_node) {
		full_backtrack.clear();
		abbrev_backtrack.clear();

		// Initialize the solution
		// TODO: move to DPSolution::Reset()
		solution.node = soln_node;
		solution.score = soln_node.score;
		solution.num_walls = 0;
		solution.num_occlusions = 0;
		solution.wall_segments.clear();
		solution.wall_orients.clear();
		solution.pixel_orients.Resize(geom->camera->ny(),
																	geom->camera->nx(),
																	kVerticalAxis);

		// Backtrack through the graph
		const DPState* cur = &soln_node.src;
		const DPState* out = NULL;
		do {
			const DPState& next = cache[*cur].src;
			CHECK(cache.find(*cur) != cache.end()) << "a state in the backtrack has no cached solution";

			full_backtrack.push_back(cur);
			if (cur->dir == DPState::DIR_IN && out != NULL) {
				abbrev_backtrack.push_back(cur);

				solution.num_walls++;
				if (next.dir == DPState::DIR_UP || next.dir == DPState::DIR_DOWN) {
					solution.num_occlusions++;
				}

				int orient = 1-cur->axis;
				LineSeg grid_seg(makeVector(cur->col, cur->row, 1.0),
												 makeVector(out->col, out->row, 1.0));
				LineSeg image_seg(geom->GridToImage(project(grid_seg.start)),
													geom->GridToImage(project(grid_seg.end)));
				solution.wall_segments.push_back(image_seg);
				solution.wall_orients.push_back(orient);

				Vec3 verts[] = { image_seg.start,
												 image_seg.end,
												 geom->GridToImage(geom->Transfer(project(grid_seg.end))),
												 geom->GridToImage(geom->Transfer(project(grid_seg.start))) };
				FillPolygon(array_range(verts, 4), solution.pixel_orients, orient);
				out = NULL;
			} else if (cur->dir == DPState::DIR_OUT && out == NULL) {
				abbrev_backtrack.push_back(cur);
				out = cur;
			}

			cur = &next;
		} while (*cur != DPState::none);

		// Compute the solution path
		solution.path_ys.Resize(geom->grid_size[0], -1);
		solution.path_axes.Resize(geom->grid_size[0], -1);
		for (int i = 0; i < full_backtrack.size()-1; i++) {
			const DPState& state = *full_backtrack[i];
			const DPState& next = *full_backtrack[i+1];
			CHECK(state.col <= geom->grid_size[0]);  // state.col==geom->grid_size[0] is permitted
			if (state.dir == DPState::DIR_OUT) {
				int vpt_col = geom->vpt_cols[state.axis];
				double m = (state.row - geom->horizon_row)
					/ static_cast<double>(state.col - vpt_col);
				double c = geom->horizon_row - m*vpt_col;
				for (int x = state.col-1; x >= next.col; x--) {
					solution.path_ys[x] = roundi(m*x + c);
					solution.path_axes[x] = state.axis;
				}
			}
		}

		// Check that the path spans all columns
		for (int x = 0; x < geom->grid_size[0]; x++) {
			CHECK_NE(solution.path_ys[x], -1) << "Solution misses column " << x;
			CHECK_NE(solution.path_axes[x], -1) << "Invalid orientation at column " << x;
		}
	}

	void ManhattanDP::ComputeGridOrients(MatI& grid_orients) {
		geom->PathToOrients(solution.path_ys, solution.path_axes, grid_orients);
	}

	void ManhattanDP::ComputeExactOrients(MatI& orients) {
		MatI grid_orients;
		ComputeGridOrients(grid_orients);

		orients.Resize(geom->camera->ny(),
									 geom->camera->nx());
		for (int y = 0; y < orients.Rows(); y++) {
			int* row = orients[y];
			for (int x = 0; x < orients.Cols(); x++) {
				Vec2I grid_pt = RoundVector(geom->ImageToGrid(makeVector(x, y, 1.0)));
				// each pixel *must* project inside the image bounds
				CHECK_INTERVAL(grid_pt[0], 0, geom->grid_size[0]-1);
				CHECK_INTERVAL(grid_pt[1], 0, geom->grid_size[1]-1);
				row[x] = grid_orients[ grid_pt[1] ][ grid_pt[0] ];
			}
		}
	}

	bool ManhattanDP::OcclusionValid(int col, int left_axis, int right_axis, int occl_side) {
		int occl_axis = occl_side < 0 ? left_axis : right_axis;
		int occl_vpt_col = geom->vpt_cols[occl_axis];
		int occl_vpt_side = occl_vpt_col < col ? -1 : 1;
		int opp_vpt_col = geom->vpt_cols[1-occl_axis];  // irrespective of whether left_axis=right_axis!
		int opp_vpt_side = opp_vpt_col < col ? -1 : 1;

		// is the occluding vpt on the same side as the occluding stripe?
		bool occl_vpt_behind = occl_side == occl_vpt_side;

		// is the opposite vpt between the div and the occluding vpt?
		bool opp_vpt_between =
			opp_vpt_side == occl_vpt_side &&
			abs(col-opp_vpt_col) < abs(col-occl_vpt_col);

		// the occlusion is valid iff occl_vpt_behind == opp_vpt_between
		return occl_vpt_behind == opp_vpt_between;
	}

	bool ManhattanDP::CanMoveVert(const DPState& cur, const DPState& next) {
		// is the occluding stripe to the left or right?
		bool on_ceil = cur.row < geom->horizon_row;
		bool going_up = next.dir == DPState::DIR_UP;
		int occl_side = (on_ceil == going_up ? -1 : 1);

		// Here we assume that we're moving right-to-left
		return OcclusionValid(cur.col, next.axis, cur.axis, occl_side);
	}

	void ManhattanDP::GetBest(MatD& best) const {
		best.Resize(geom->ny(), geom->nx());
		DPState state;
		DPCache::const_iterator it;
		for (state.row = 0; state.row < geom->ny(); state.row++) {
			for (state.col = 0; state.col < geom->nx(); state.col++) {
				best[state.row][state.col] = -INFINITY;
				for (state.axis = 0; state.axis < 2; state.axis++) {
					for (state.dir = 0; state.dir < 4; state.dir++) {
						it = cache.find(state);
						if (it != cache.end() && it->score > best[state.row][state.col]) {
							best[state.row][state.col] = it->score;
						}
					}
				}
			}
		}							
	}

	void ManhattanDP::DrawWireframeGridSolution(ImageRGB<byte>& canvas) const {
		CHECK(!solution.wall_segments.empty());
		BOOST_FOREACH(const LineSeg& image_seg, solution.wall_segments) {
			LineSeg seg(unproject(geom->ImageToGrid(image_seg.start)),
									unproject(geom->ImageToGrid(image_seg.end)));
			LineSeg opp(geom->Transfer(seg.start), geom->Transfer(seg.end));
			DrawLineClipped(canvas, seg, Colors::red());
			DrawLineClipped(canvas, opp, Colors::red());
			DrawLineClipped(canvas, project(seg.start), project(opp.start), Colors::red());
			DrawLineClipped(canvas, project(seg.end), project(opp.end), Colors::red());
		}
	}

	void ManhattanDP::DrawWireframeSolution(ImageRGB<byte>& canvas) const {
		CHECK(!solution.wall_segments.empty());
		BOOST_FOREACH(const LineSeg& seg, solution.wall_segments) {
			LineSeg opp(geom->GridToImage(geom->Transfer(geom->ImageToGrid(seg.start))),
									geom->GridToImage(geom->Transfer(geom->ImageToGrid(seg.end))));
			DrawLineClipped(canvas, seg, Colors::red());
			DrawLineClipped(canvas, opp, Colors::red());
			DrawLineClipped(canvas, project(seg.start), project(opp.start), Colors::red());
			DrawLineClipped(canvas, project(seg.end), project(opp.end), Colors::red());
		}
	}







	////////////////////////////////////////////////////////////////////////////////
	ManhattanDPReconstructor::ManhattanDPReconstructor() {
	}

	ManhattanDPReconstructor::~ManhattanDPReconstructor() {
	}

	void ManhattanDPReconstructor::Compute(const PosedImage& image,
																				 const DPGeometry& geom,
																				 const DPObjective& objective) {
		input = &image;
		geometry = geom; //.Configure(image.pc(), floorToCeil);

		if (!payoff_gen) {
			payoff_gen.reset(new ObjectivePayoffGen);
		}
		payoff_gen->Compute(objective, geometry);

		Compute(image, geometry, payoff_gen->payoffs);
	}

	void ManhattanDPReconstructor::Compute(const PosedImage& image,
																				 const DPGeometry& geom,
																				 const DPPayoffs& po) {
		input = &image;
		payoffs = &po;
		geometry = geom;  // we're copying here, but nothing big (yet)
		TIMED("Complete DP") dp.Compute(po, geometry);
	}

	void ManhattanDPReconstructor::ReportBacktrack() {
		BOOST_FOREACH(const DPState* cur, dp.full_backtrack) {
			switch (cur->dir) {
			case DPState::DIR_UP: DLOG_N << "UP"; break;
			case DPState::DIR_DOWN: DLOG_N << "DOWN"; break;
			case DPState::DIR_IN: DLOG_N << "IN"; break;
			case DPState::DIR_OUT: DLOG_N << "OUT"; break;
			}
			DLOG << str(format(" %d,%d (%d)") % cur->row % cur->col % dp.Solve(*cur).score);
		}
	}

	float ManhattanDPReconstructor::GetLabellingError(const MatI& gt_orients) {
		return 1. - ComputeAgreementFrac(dp.solution.pixel_orients, gt_orients);
	}

	float ManhattanDPReconstructor::GetLabellingError(const ManhattanGroundTruth& gt) {
		return GetLabellingError(gt.orientations());
	}

	float ManhattanDPReconstructor::ReportLabellingError(const ManhattanGroundTruth& gt) {
		float err = GetLabellingError(gt);
		DLOG << format("Labelling error: %|40t|%.1f%%") % (err*100.);
		return err;
	}

	void ManhattanDPReconstructor::GetDepthErrors(const ManhattanGroundTruth& gt, MatF& errors) {
		DPGeometryWithScale scalegeom(geometry, gt.zfloor(), gt.zceil());
		const MatD& gt_depth = gt.depthmap();
		const MatD& soln_depth = dp.solution.GetDepthMap(scalegeom);
		ComputeDepthErrors(gt_depth, soln_depth, errors);
	}

	float ManhattanDPReconstructor::GetDepthError(const ManhattanGroundTruth& gt) {
		MatF errors;	// TODO: move to a class variable (or external?)
		GetDepthErrors(gt, errors);
		return MeanError(errors);
	}

	float ManhattanDPReconstructor::ReportDepthError(const ManhattanGroundTruth& gt) {
		double acc = GetDepthError(gt);
		DLOG << format("Mean depth error: %|40t|%.1f%%") % (acc*100);
		return acc;
	}

	void ManhattanDPReconstructor::OutputOrigViz(const string& path) const {
		WriteImage(path, input->rgb);
	}

	void ManhattanDPReconstructor::OutputSolutionViz(const string& path) const {
		CHECK(input->loaded());
		ImageRGB<byte> canvas;
		ImageCopy(input->rgb, canvas);
		DrawOrientations(dp.solution.pixel_orients, canvas, 0.35);
		WriteImage(path, canvas);
	}

	void ManhattanDPReconstructor::OutputGridViz(const string& path) const {
		ImageRGB<byte> grid_canvas(geometry.grid_size[0], geometry.grid_size[1]);
		grid_canvas.Clear(Colors::white());
		dp.DrawWireframeGridSolution(grid_canvas);
		WriteImage(path, grid_canvas);
	}

	void ManhattanDPReconstructor::OutputManhattanHomologyViz(const string& path) const {
		CHECK(input->loaded());
		FileCanvas canvas(path, input->size());
		canvas.DrawImage(input->rgb);
		for (int i = 0; i < 20; i++) {
			int x = rand() % input->nx();
			int y = rand() % input->ny();
			Vec2 u = makeVector(x, y);
			Vec2 v = project(geometry.GridToImage(geometry.Transfer(geometry.ImageToGrid(unproject(u)))));
			if (u[1] > v[1]) swap(u,v);
			canvas.StrokeLine(u, v, Colors::black());
			canvas.DrawDot(u, 4.0, Colors::blue());
			canvas.DrawDot(v, 4.0, Colors::red());
		}
	}

	void ManhattanDPReconstructor::OutputPayoffsViz(int orient, const string& path) const {
		CHECK(input->loaded());

		// Draw payoffs
		ImageRGB<byte> payoff_image(geometry.grid_size[0], geometry.grid_size[1]);
		DrawMatrixRecentred(payoffs->wall_scores[orient], payoff_image);
		dp.DrawWireframeGridSolution(payoff_image);

		// Draw image in grid coords
		ImageRGB<byte> grid_image;
		geometry.TransformToGrid(input->rgb, grid_image);

		// Blend together
		FileCanvas canvas(path, grid_image);
		canvas.DrawImage(payoff_image, 0.6);
	}

	void ManhattanDPReconstructor::OutputDepthErrorViz(const ManhattanGroundTruth& gt,
																										 const string& path) {
		MatF depth_errors;
		GetDepthErrors(gt, depth_errors);
		WriteMatrixImageRescaled(path, depth_errors);
	}

}
