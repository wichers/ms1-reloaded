// Commit each function's decompiler-derived parameters and return value once the function
// boundaries are final. Without a committed signature a caller's decompilation cannot show the
// arguments it passes in registers or on the stack, and every call reads as f(). The second
// pass sees the callee signatures the first one committed. Thunks and functions whose
// signature is user-defined or imported are left alone.
// Headless: -postScript CommitSignatures.java (after RedrawBoundaries.java)
//@category MS1

import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.Function;
import ghidra.program.model.pcode.HighFunctionDBUtil;
import ghidra.program.model.pcode.HighFunctionDBUtil.ReturnCommitOption;
import ghidra.program.model.symbol.SourceType;

public class CommitSignatures extends GhidraScript {

	private static final int PASSES = 2;

	@Override
	protected void run() throws Exception {
		DecompInterface decompiler = new DecompInterface();
		decompiler.openProgram(currentProgram);
		for (int pass = 1; pass <= PASSES; pass++) {
			int committed = 0;
			int skipped = 0;
			int failed = 0;
			for (Function f : currentProgram.getFunctionManager().getFunctions(true)) {
				monitor.checkCancelled();
				SourceType source = f.getSignatureSource();
				if (f.isThunk() || source == SourceType.USER_DEFINED || source == SourceType.IMPORTED) {
					skipped++;
					continue;
				}
				DecompileResults r = decompiler.decompileFunction(f, 120, monitor);
				if (r == null || !r.decompileCompleted()) {
					failed++;
					continue;
				}
				HighFunctionDBUtil.commitParamsToDatabase(r.getHighFunction(), true, ReturnCommitOption.COMMIT,
					SourceType.ANALYSIS);
				committed++;
			}
			println(String.format("CommitSignatures: pass %d: %d committed, %d skipped, %d failed", pass, committed,
				skipped, failed));
		}
		decompiler.dispose();
	}
}
