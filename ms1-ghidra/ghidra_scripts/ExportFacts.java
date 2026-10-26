// Export per-function facts as JSON for the firmware map: entry, name and its source, size,
// called functions, and every memory location the function's instructions load or store at a
// constant address (direct, extended and register-free addressing; indexed accesses whose
// address depends on a register are not resolved). The address of each LOAD/STORE is folded
// from the instruction's own p-code (COPY / INT_ADD / INT_ZEXT / INT_OR of constants), so
// direct-page register accesses count even where Ghidra creates no reference.
// Headless: -postScript ExportFacts.java <out.json>
//@category MS1

import java.io.FileWriter;
import java.util.*;

import com.google.gson.GsonBuilder;

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.*;
import ghidra.program.model.pcode.PcodeOp;
import ghidra.program.model.pcode.Varnode;
import ghidra.program.model.symbol.Reference;
import ghidra.program.model.symbol.Symbol;

public class ExportFacts extends GhidraScript {

	@Override
	protected void run() throws Exception {
		Listing listing = currentProgram.getListing();
		List<Map<String, Object>> out = new ArrayList<>();
		for (Function f : currentProgram.getFunctionManager().getFunctions(true)) {
			monitor.checkCancelled();
			Set<String> calls = new TreeSet<>();
			Map<String, Set<String>> access = new TreeMap<>(); // address -> {r, w}
			for (Instruction insn : listing.getInstructions(f.getBody(), true)) {
				for (Reference r : insn.getReferencesFrom()) {
					if (r.getReferenceType().isCall()) {
						Function callee = getFunctionAt(r.getToAddress());
						calls.add(callee != null ? callee.getName() : r.getToAddress().toString());
					}
				}
				Map<Integer, Long> consts = new HashMap<>();
				for (PcodeOp op : insn.getPcode()) {
					fold(op, consts);
					if (op.getOpcode() == PcodeOp.LOAD || op.getOpcode() == PcodeOp.STORE) {
						Long a = value(op.getInput(1), consts);
						if (a != null) {
							access.computeIfAbsent(String.format("%04x", a), k -> new TreeSet<>())
									.add(op.getOpcode() == PcodeOp.LOAD ? "r" : "w");
						}
					}
				}
			}
			Map<String, Object> rec = new LinkedHashMap<>();
			rec.put("entry", f.getEntryPoint().toString());
			rec.put("name", f.getName());
			rec.put("source", f.getSymbol().getSource().toString());
			rec.put("bytes", f.getBody().getNumAddresses());
			Function thunked = f.isThunk() ? f.getThunkedFunction(false) : null;
			rec.put("thunk_of", thunked == null ? null : thunked.getName());
			rec.put("calls", calls);
			Map<String, String> acc = new LinkedHashMap<>();
			access.forEach((a, rw) -> acc.put(a, String.join("", rw)));
			rec.put("access", acc);
			out.add(rec);
		}
		Map<String, String> labels = new TreeMap<>();
		for (Symbol s : currentProgram.getSymbolTable().getAllSymbols(false)) {
			Address a = s.getAddress();
			if (a.isMemoryAddress() && !a.getAddressSpace().isOverlaySpace() && a.getOffset() < 0x4000) {
				String key = String.format("%04x", a.getOffset());
				if (s.isPrimary())
					labels.put(key, s.getName());
				else
					labels.putIfAbsent(key, s.getName());
			}
		}
		Map<String, Object> doc = new LinkedHashMap<>();
		doc.put("program", currentProgram.getName());
		doc.put("functions", out);
		doc.put("labels_below_0x4000", labels);
		try (FileWriter w = new FileWriter(getScriptArgs()[0])) {
			new GsonBuilder().setPrettyPrinting().create().toJson(doc, w);
			w.write("\n");
		}
		println(String.format("ExportFacts: %d functions, %d labels below 0x4000", out.size(), labels.size()));
	}

	private static void fold(PcodeOp op, Map<Integer, Long> consts) {
		Varnode out = op.getOutput();
		if (out == null || !out.isUnique()) {
			return;
		}
		Long a = op.getNumInputs() > 0 ? value(op.getInput(0), consts) : null;
		Long b = op.getNumInputs() > 1 ? value(op.getInput(1), consts) : null;
		Long v = switch (op.getOpcode()) {
			case PcodeOp.COPY, PcodeOp.INT_ZEXT -> a;
			case PcodeOp.INT_ADD -> a != null && b != null ? a + b : null;
			case PcodeOp.INT_OR -> a != null && b != null ? a | b : null;
			default -> null;
		};
		if (v != null) {
			consts.put((int) out.getOffset(), v & 0xffff);
		}
		else {
			consts.remove((int) out.getOffset()); // reassigned from a register or memory
		}
	}

	private static Long value(Varnode v, Map<Integer, Long> consts) {
		if (v.isConstant()) {
			return v.getOffset() & 0xffff;
		}
		if (v.isUnique()) {
			return consts.get((int) v.getOffset());
		}
		return null;
	}
}
