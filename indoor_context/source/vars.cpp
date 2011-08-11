#include "vars.h"
#include "common_types.h"

namespace indoor_context {
	static const string kDefaultConfigFile = "config/common.cfg";

	string config_file;

	void InitVars() {
		InitVars(kDefaultConfigFile);
	}

	void InitVars(const string& file) {
		config_file = file;
		ReloadVars();
	}

	void InitVars(int argc, char **argv) {
		// Parse the arguments first
		GVars3::GUI.parseArguments(argc, argv);
		// Setup a var so that we can specify the config file with -config
		GVars3::gvar3<string> gvConfigFile("config", kDefaultConfigFile, GVars3::SILENT);
		// Now read the config file
		InitVars(*gvConfigFile);
	}

	void ReloadVars() {
		GVars3::GUI.LoadFile(config_file);	
	}
}
