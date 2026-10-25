// Disable the auto-analyzers that would undo or fight the evidence-based boundaries, before
// auto-analysis and the re-analysis Ghidra runs after post-scripts.
//   Shared Return Calls:      creates functions at jump targets shared by several functions and
//                             turns the jumps into tail calls, re-splitting what
//                             RedrawBoundaries.java merges.
//   HCS12 Calling Convention: assigns the CALL/RTC convention the HCS-12:BE:16:flat language
//                             does not define, and fails on every run.
// Headless: -preScript AnalysisOptions.java
//@category MS1

import java.util.List;

import ghidra.app.script.GhidraScript;

public class AnalysisOptions extends GhidraScript {

	private static final List<String> DISABLED = List.of("Shared Return Calls", "HCS12 Calling Convention");

	@Override
	protected void run() throws Exception {
		for (String analyzer : DISABLED) {
			setAnalysisOption(currentProgram, analyzer, "false");
		}
		println("AnalysisOptions: disabled " + DISABLED);
	}
}
