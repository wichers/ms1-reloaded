// Name functions and data by the firmware rebuild (../ms1-firmware), whose names are checked
// against this program's behaviour function by function (difftest) and whose RAM variables sit
// at the original's addresses. Input: a TSV of "address<TAB>name" (rebuild_names.py writes it).
// A function or label that already has the name is left alone; otherwise the name becomes the
// primary one, User Defined, and an imported (IDA) name stays as a secondary label.
// Headless: -postScript ApplyNames.java <names.tsv> (on the imported program, not read-only)
//@category MS1

import java.nio.file.Files;
import java.nio.file.Path;

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.SourceType;
import ghidra.program.model.symbol.Symbol;

public class ApplyNames extends GhidraScript {

	@Override
	protected void run() throws Exception {
		String[] args = getScriptArgs();
		if (args.length != 1) {
			printerr("ApplyNames: usage: ApplyNames.java <names.tsv>");
			return;
		}
		int functions = 0, labels = 0, kept = 0;
		for (String line : Files.readAllLines(Path.of(args[0]))) {
			if (line.isBlank() || line.startsWith("#"))
				continue;
			String[] f = line.split("\t");
			Address a = toAddr(Long.parseLong(f[0], 16));
			String name = f[1];
			Function fn = getFunctionAt(a);
			Symbol primary = getSymbolAt(a);
			if (primary != null && primary.getName().equals(name)) {
				kept++;
				continue;
			}
			String old = primary == null ? null : primary.getName();
			boolean imported = primary != null && primary.getSource() == SourceType.IMPORTED;
			if (fn != null) {
				fn.setName(name, SourceType.USER_DEFINED);
				functions++;
			} else {
				createLabel(a, name, true, SourceType.USER_DEFINED);
				labels++;
			}
			if (fn != null && imported && old != null)
				createLabel(a, old, false, SourceType.IMPORTED);
		}
		println(String.format("ApplyNames: %d functions and %d labels named, %d already so", functions, labels, kept));
	}
}
