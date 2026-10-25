// Decompile every function of the current program into one C file.
// Headless: -postScript DecompileAll.java <output.c>
//@category MS1

import java.io.File;
import java.io.FileWriter;
import java.io.PrintWriter;

import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.Function;

public class DecompileAll extends GhidraScript {

	@Override
	protected void run() throws Exception {
		File out = new File(getScriptArgs()[0]);
		DecompInterface decompiler = new DecompInterface();
		decompiler.openProgram(currentProgram);
		int ok = 0;
		int failed = 0;
		try (PrintWriter w = new PrintWriter(new FileWriter(out))) {
			w.printf("/* %s: Ghidra decompilation of every function */%n%n", currentProgram.getName());
			for (Function f : currentProgram.getFunctionManager().getFunctions(true)) {
				monitor.checkCancelled();
				DecompileResults r = decompiler.decompileFunction(f, 120, monitor);
				w.printf("/* ===== %s @ %s ===== */%n", f.getName(), f.getEntryPoint());
				if (r != null && r.decompileCompleted()) {
					w.println(r.getDecompiledFunction().getC());
					ok++;
				}
				else {
					w.printf("/* decompile failed: %s */%n%n", r == null ? "no result" : r.getErrorMessage());
					failed++;
				}
			}
		}
		decompiler.dispose();
		println(String.format("DecompileAll: %d decompiled, %d failed -> %s", ok, failed, out));
	}
}
