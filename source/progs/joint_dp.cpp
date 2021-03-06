#include <boost/filesystem.hpp>

#include "entrypoint_types.h"
#include "guided_line_detector.h"
#include "line_sweeper.h"
#include "joint_payoffs.h"
#include "manhattan_dp.h"
#include "map.h"
#include "map_io.h"
#include "bld_helpers.h"
#include "canvas.h"
#include "manhattan_ground_truth.h"
#include "safe_stream.h"
#include "timer.h"

#include "counted_foreach.tpp"
#include "format_utils.tpp"
#include "io_utils.tpp"
#include "image_utils.tpp"

lazyvar<string> gvStereoOffsets("JointDP.Stereo.AuxOffsets");
lazyvar<int> gvDrawPayoffs("JointDP.Output.DrawPayoffs");

void OutputPayoffsViz(const MatF& payoffs,
											const DPGeometry& geometry,
											const ManhattanDPReconstructor& recon,
											const ImageRGB<byte>& grid_image,
											const string& file) {
	// Draw payoffs
	ImageRGB<byte> payoff_image(geometry.grid_size[0], geometry.grid_size[1]);
	DrawMatrixRescaled(payoffs, payoff_image);
	recon.dp.DrawWireframeGridSolution(payoff_image);

	// Blend together
	FileCanvas canvas(file, grid_image);
	canvas.DrawImage(payoff_image, 0.6);
}

int main(int argc, char **argv) {
	InitVars(argc, argv);
	AssertionManager::SetExceptionMode();

	if (argc < 3) {
		DLOG << "Usage: joint_dp SEQUENCE FRAMES [-q|RESULTS_DIR]";
		DLOG << "           if specified, results will be appended to results/RESULTS_DIR/";
		exit(-1);
	}

	// Read parameters
	string sequence = argv[1];
	vector<int> frame_ids = ParseMultiRange<int>(argv[2]);
	bool quiet = argc > 3 && string(argv[3]) == "-q";

		// Set up results dir
	fs::path results_dir;
	if (!quiet) {
		if (argc < 4) {
			results_dir = fs::initial_path();
		} else {
			results_dir = argv[3];
		}
		CHECK_PRED1(fs::exists, results_dir);
	}

	ofstream stats_out;
	format stats_fmt("\"%s\",\"%s\",%d,%f,%f\n");

	// Create vizualization dir
	fs::path viz_dir = results_dir / "out";
	if (!quiet && !fs::create_directory(viz_dir)) {
		DLOG << "Created output directory: " << viz_dir;
	}
	format filepat("%s/%s_frame%03d_%s.%s");
	// Pattern for output filenames
	filepat.bind_arg(1, viz_dir.string());
	filepat.bind_arg(2, sequence);

	if (!quiet) {
		// Open the CSV file
		string stats_fname = fmt("performance_%s.csv", results_dir.filename());
		fs::path stats_path = results_dir / stats_fname;
		stats_out.open(stats_path.string().c_str(), ios::app);

		// Write the parameters
		string params_fname = fmt("parameters_%s.csv", results_dir.filename());
		fs::path params_path = results_dir / params_fname;
		ofstream params_out(params_path.string().c_str());
		GV3::print_var_list(params_out);
		params_out.close();
	}

	// Load the map
	Map map;
	proto::TruthedMap gt_map;
	LoadXmlMapWithGroundTruth(GetMapPath(sequence), map, gt_map);

	// Initialize the payoff generator
	JointPayoffGen joint;
	vector<int> stereo_offsets = ParseMultiRange<int>(*gvStereoOffsets);
	vector<Vec3> point_cloud;  // must be outside scope as
														 // PointCloudPayoffs keeps a pointer

	// Initialize statistics
	double sum_acc = 0;
	double sum_err = 0;
	int num_frames = 0;

	// Process each frame
	BOOST_FOREACH(int frame_id, frame_ids) {
		TITLE("Frame "<<frame_id);
		ScopedTimer t("Process frame");

		// Get the frame
		Frame& frame = *map.GetFrameById(frame_id);
		if (&frame == NULL) continue;
		frame.LoadImage();
		num_frames++;  // for computing average performance, in case one
									 // or more of the frames is missing

		// Setup geometry
		DPGeometryWithScale geom(frame.image.pc(),
														 gt_map.floorplan().zfloor(),
														 gt_map.floorplan().zceil());

		// Get point cloud
		point_cloud.clear();
		frame.GetMeasuredPoints(point_cloud);

		// Get auxiliary frames
		vector<const PosedImage*> aux_images;
		COUNTED_FOREACH(int i, int offset, stereo_offsets) {
			Frame* aux_frame = map.GetFrameById(frame_id+offset);
			if (aux_frame != NULL) {
				aux_frame->LoadImage();
				aux_images.push_back(&aux_frame->image);
			}
		}

		// Compute joint payoffs
		joint.Compute(frame.image, geom, point_cloud, aux_images);

		// Reconstruct
		ManhattanDPReconstructor recon;
		recon.Compute(frame.image, geom, joint.payoffs);
		const DPSolution& soln = recon.dp.solution;

		// Compute ground truth
		ManhattanGroundTruth gt(gt_map.floorplan(), frame.image.pc());

		// Compute payoff without penalties
		double gross_payoffs = soln.GetTotalPayoff(joint.payoffs, false);
		double penalties = gross_payoffs - soln.score;
		double mono_payoffs = soln.GetTotalPayoff(joint.mono_gen.payoffs, false);
		double pt_agree_payoffs = soln.GetPathSum(joint.point_cloud_gen.agreement_payoffs);
		double pt_occl_payoffs = soln.GetPathSum(joint.point_cloud_gen.occlusion_payoffs);
		double stereo_payoffs = 0.0;
		for (int i = 0; i < joint.stereo_gens.size(); i++) {
			stereo_payoffs += soln.GetPathSum(joint.stereo_gens[i].payoffs);
		}
		mono_payoffs *= joint.mono_weight;
		pt_agree_payoffs *= joint.agreement_weight;
		pt_occl_payoffs *= joint.occlusion_weight;
		stereo_payoffs *= (joint.stereo_weight / aux_images.size());  // yes this is correct

		// Compute performance
		double pixel_acc = recon.ReportAccuracy(gt) * 100;
		sum_acc += pixel_acc;
		double mean_err = recon.ReportDepthError(gt) * 100;
		sum_err += mean_err;

		if (!quiet) {
			// Visualize
			filepat.bind_arg(3, frame_id);
			filepat.bind_arg(5, "png");

			// Copy original
			string dest = str(filepat % "orig");
			if (!fs::exists(dest)) {
				fs::copy_file(frame.image_file, dest);
			}

			// Draw the solution
			recon.OutputSolution(str(filepat % "dp"));

			// Draw payoffs
			if (*gvDrawPayoffs) {
				ImageRGB<byte> grid_image;
				geom.TransformToGrid(frame.image.rgb, grid_image);
				for (int i = 0; i < 2; i++) {
					OutputPayoffsViz(joint.payoffs.wall_scores[i],
													 geom, recon, grid_image,
													 str(filepat % fmt("payoffs%d", i)));
				}

				for (int i = 0; i < 2; i++) {
					OutputPayoffsViz(joint.mono_gen.payoffs.wall_scores[i],
													 geom, recon, grid_image,
													 str(filepat % fmt("monopayoffs%d", i)));
				}			

				for (int i = 0; i < joint.stereo_gens.size(); i++) {
					OutputPayoffsViz(joint.stereo_gens[i].payoffs,
													 geom, recon, grid_image,
													 str(filepat % fmt("stereopayoffs_aux%d", i)));
				}
			}

			// Write results to CSV file
			time_t now = time(NULL);
			string timestamp = asctime(localtime(&now));
			stats_out << stats_fmt
				% timestamp.substr(0,timestamp.length()-1)
				% sequence
				% frame_id
				% pixel_acc
				% mean_err;

			// Write results to individual file
			// this must come last because of call to bind_arg()
			filepat.bind_arg(5, "txt");
			sofstream info_out(str(filepat % "stats"));
			info_out << format("Labelling accuracy: %|40t|%f%%\n") % pixel_acc;
			info_out << format("Mean depth error: %|40t|%f%%\n") % mean_err;
			info_out << format("Net score: %|40t|%f\n") % soln.score;
			info_out << format("  Penalties: %|40t|%f%%\n") % (100.0*penalties/gross_payoffs);
			info_out << format("  Gross payoffs: %|40t|%f\n") % gross_payoffs;
			info_out << format("    Mono payoffs: %|40t|%.1f%%\n")
				% (100*mono_payoffs/gross_payoffs);
			info_out << format("    Stereo payoffs: %|40t|%.1f%%\n")
				% (100*stereo_payoffs/gross_payoffs);
			info_out << format("    3D (agreement) payoffs: %|40t|%.1f%%\n")
				% (100.0*pt_agree_payoffs/gross_payoffs);
			info_out << format("    3D (occlusion) payoffs: %|40t|%.1f%%\n")
				% (100.0*pt_occl_payoffs/gross_payoffs);
		}
	}

	// Note that if one or more frames weren't found then num_frames
	// will not equal frame_ids.size()
  // *these are already multipled by 100!
	double av_err = sum_err / num_frames;
	double av_labelling_err = 100. - sum_acc / num_frames;
	if (quiet) {
		DLOG << av_err;
	} else {
		DLOG << format("AVERAGE DEPTH ERROR: %|40t|%.1f%%") % av_err;
		DLOG << format("AVERAGE LABELLING ERROR: %|40t|%.1f%%") % av_labelling_err;
	}
	
	return 0;
}
