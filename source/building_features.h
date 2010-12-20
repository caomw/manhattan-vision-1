#include <boost/ptr_container/ptr_vector.hpp>

#include "common_types.h"
#include "camera.h"
#include "filters.h"
#include "guided_line_detector.h"
#include "line_sweeper.h"

namespace indoor_context {
	class BuildingFeatures {
	public:
		const PosedImage* input;

		// The enabled feature components (e.g. "hsv", "gabor")
		set<string> components;
		// The features generated on last call to Compute()
		vector<const MatF*> features;
		// A textual explanation for each matrix in the above
		vector<string> feature_strings;

		// RGB and HSV features
		boost::ptr_vector<MatF> rgb_features;
		boost::ptr_vector<MatF> hsv_features;

		// Gabor features
		GaborFilters gabor;
		boost::ptr_vector<MatF> gabor_features;

		// Line sweeps
		GuidedLineDetector line_detector;
		IsctGeomLabeller line_sweeper;
		boost::ptr_vector<MatF> sweep_features;

		// Ground truth
		boost::ptr_vector<MatF> gt_features;

		// Initialize empty
		BuildingFeatures();
		// Initialize with the given feature set (see Configure() below)
		BuildingFeatures(const string& config);

		// Configure the features with a string describing the feature set
		// Example feature set:
		//   "rgb,hsv,sweeps"
		//   "all,-rgb,-hsv"   -- all except RGB and HSV
		//   "all,gt"          -- all including ground truth orientations
		//   "default"         -- use the feature spec in the relevant gvar
		void Configure(const string& config);
		// Return true if the given component was active in the config string
		bool IsActive(const string& component);

		// Compute features. If ground truth is not provided then the
		// feature set must not include "gt".
		void Compute(const PosedImage& image, const MatI* gt_orients);
	};
}
