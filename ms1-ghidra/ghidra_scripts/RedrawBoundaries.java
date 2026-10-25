// Redraw function boundaries from control-flow evidence instead of the imported (IDA) splits.
//
// A true entry is called (CALL reference from an instruction), has its address taken (a DATA or
// PARAM reference from an instruction, or from data other than a jump table — a READ/WRITE is a
// memory access, not an address taken), is an
// external entry point, or is the target of a thunk (an interrupt trampoline is a thunk of
// its handler; thunk functions get no region of their own). Every other function start is a candidate fragment. The region of a
// true function is everything reachable from its entry through non-call flow, stopping at
// other true entries.
//   - a fragment entered only by unconditional jumps from several regions is a tail-called
//     function: it is promoted to a true entry and the regions are recomputed
//   - a fragment inside exactly one region is merged into that function (name kept as label)
//   - a fragment inside several regions (shared code) or none (orphan) is kept and reported
//   - an unconditional jump from a region into another true entry becomes a tail call
//     (CALL_RETURN flow override); conditional branches and fall-throughs into another true
//     entry cannot be overridden and are reported
// A jump that analysis already turned into a tail call is still a jump here: its call
// reference is no evidence of a call, and if its target turns out to be a fragment the
// override is removed again ("untailcall").
// Every action is written to the report given as the script argument.
// Headless: -postScript RedrawBoundaries.java <report.tsv>
//@category MS1

import java.io.FileWriter;
import java.io.PrintWriter;
import java.util.*;

import ghidra.app.cmd.function.CreateFunctionCmd;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.*;

public class RedrawBoundaries extends GhidraScript {

	private Listing listing;
	private ReferenceManager refs;
	private final Set<Address> trueEntries = new HashSet<>();
	private final Set<List<String>> report = new LinkedHashSet<>();
	private Set<List<String>> flowNotes;
	private Map<Address, Instruction> tailJumps;
	private Map<Address, Instruction> plainJumps;

	@Override
	protected void run() throws Exception {
		listing = currentProgram.getListing();
		refs = currentProgram.getReferenceManager();
		FunctionManager functions = currentProgram.getFunctionManager();

		List<Function> all = new ArrayList<>();
		functions.getFunctions(true).forEach(all::add);
		for (Function f : all) {
			if (isTrueEntry(f.getEntryPoint())) {
				trueEntries.add(f.getEntryPoint());
			}
		}

		Map<Address, List<Function>> owners;
		boolean promoted;
		do {
			owners = regions(all);
			promoted = false;
			for (Function f : all) {
				Address entry = f.getEntryPoint();
				if (!trueEntries.contains(entry) && owners.getOrDefault(entry, List.of()).size() > 1 &&
					onlyJumpedTo(entry)) {
					trueEntries.add(entry);
					record("promoted", entry, f.getName(), "tail-jumped to from " + names(owners.get(entry)));
					promoted = true;
				}
			}
		}
		while (promoted);
		report.addAll(flowNotes);

		for (Function f : all) {
			Address entry = f.getEntryPoint();
			if (trueEntries.contains(entry)) {
				continue;
			}
			List<Function> in = owners.getOrDefault(entry, List.of());
			if (in.size() == 1) {
				merge(f, in.get(0));
			}
			else if (in.isEmpty()) {
				record("orphan", entry, f.getName(), "no incoming call, pointer or flow from a true entry");
			}
			else {
				record("shared", entry, f.getName(), names(in));
			}
		}

		for (Instruction insn : plainJumps.values()) {
			Address target = insn.getDefaultFlows()[0];
			insn.setFlowOverride(FlowOverride.NONE);
			record("untailcall", insn.getAddress(), nameAt(insn.getAddress()), "-> " + nameAt(target) + " (fragment, jump restored)");
		}
		for (Instruction insn : tailJumps.values()) {
			Address target = insn.getDefaultFlows().length == 1 ? insn.getDefaultFlows()[0] : insn.getFlows()[0];
			insn.setFlowOverride(FlowOverride.CALL_RETURN);
			record("tailcall", insn.getAddress(), nameAt(insn.getAddress()), "-> " + nameAt(target));
		}

		for (Function f : functions.getFunctions(true)) {
			CreateFunctionCmd.fixupFunctionBody(currentProgram, f, monitor);
		}
		for (Address entry : trueEntries) {
			if (functions.getFunctionAt(entry) == null) {
				printerr("RedrawBoundaries: true entry lost its function: " + entry);
			}
		}

		try (PrintWriter w = new PrintWriter(new FileWriter(getScriptArgs()[0]))) {
			w.println("action\taddress\tname\tdetail");
			for (List<String> row : report) {
				w.println(String.join("\t", row));
			}
		}
		Map<String, Integer> counts = new TreeMap<>();
		report.forEach(r -> counts.merge(r.get(0), 1, Integer::sum));
		println("RedrawBoundaries: " + counts);
	}

	private boolean isTrueEntry(Address entry) {
		if (currentProgram.getSymbolTable().isExternalEntryPoint(entry)) {
			return true;
		}
		Function at = currentProgram.getFunctionManager().getFunctionAt(entry);
		Address[] thunks = at == null ? null : at.getFunctionThunkAddresses(false);
		if (thunks != null && thunks.length > 0) {
			return true;
		}
		boolean jumpTableTarget = false;
		for (Reference r : refs.getReferencesTo(entry)) {
			if (r.getReferenceType() == RefType.COMPUTED_JUMP) {
				jumpTableTarget = true;
			}
		}
		for (Reference r : refs.getReferencesTo(entry)) {
			RefType type = r.getReferenceType();
			CodeUnit from = listing.getCodeUnitContaining(r.getFromAddress());
			if (from instanceof Instruction insn && type.isCall() && !isOverriddenJump(insn)) {
				return true;
			}
			boolean addressOf = type == RefType.DATA || type == RefType.PARAM;
			if (addressOf && (from instanceof Instruction || (from instanceof Data && !jumpTableTarget))) {
				return true;
			}
		}
		return false;
	}

	/** A jump instruction that analysis (or this script) turned into a tail call. */
	private static boolean isOverriddenJump(Instruction insn) {
		return insn.getFlowOverride() == FlowOverride.CALL_RETURN;
	}

	/** Entered by at least one unconditional jump and by no other flow (no branch, no fall-through). */
	private boolean onlyJumpedTo(Address entry) {
		boolean jumped = false;
		for (Reference r : refs.getReferencesTo(entry)) {
			RefType type = r.getReferenceType();
			if (type.isFlow()) {
				boolean jump = type.isJump() && type.isUnConditional() ||
					listing.getInstructionAt(r.getFromAddress()) instanceof Instruction insn && isOverriddenJump(insn);
				if (!jump) {
					return false;
				}
				jumped = true;
			}
		}
		Instruction before = listing.getInstructionBefore(entry);
		return jumped && (before == null || !entry.equals(before.getFallThrough()));
	}

	/** Maps every instruction reachable from a true entry to the true functions reaching it. */
	private Map<Address, List<Function>> regions(List<Function> all) throws Exception {
		flowNotes = new LinkedHashSet<>();
		tailJumps = new TreeMap<>();
		plainJumps = new TreeMap<>();
		Map<Address, List<Function>> owners = new HashMap<>();
		for (Function f : all) {
			if (trueEntries.contains(f.getEntryPoint()) && !f.isThunk()) {
				for (Address a : region(f.getEntryPoint())) {
					owners.computeIfAbsent(a, k -> new ArrayList<>()).add(f);
				}
			}
		}
		return owners;
	}

	/** Instructions reachable from entry without calls and without entering another true entry. */
	private Set<Address> region(Address entry) throws Exception {
		Set<Address> seen = new HashSet<>();
		Deque<Address> work = new ArrayDeque<>(List.of(entry));
		while (!work.isEmpty()) {
			monitor.checkCancelled();
			Address a = work.pop();
			Instruction insn = listing.getInstructionAt(a);
			if (insn == null || !seen.add(a)) {
				continue;
			}
			boolean overridden = isOverriddenJump(insn);
			FlowType flow = overridden ? insn.getPrototype().getFlowType(insn.getInstructionContext()) : insn.getFlowType();
			Address[] flows = overridden ? insn.getDefaultFlows() : insn.getFlows();
			if (!flow.isCall()) {
				for (Address t : flows) {
					if (!t.equals(entry) && trueEntries.contains(t)) {
						if (flow.isJump() && flow.isUnConditional() && flows.length == 1) {
							tailJumps.put(a, insn);
						}
						else {
							note("branch-into", a, "-> " + nameAt(t) + " (" + flow + ")");
						}
					}
					else {
						if (overridden) {
							plainJumps.put(a, insn);
						}
						work.push(t);
					}
				}
			}
			Address fall = overridden ? null : insn.getFallThrough();
			if (fall != null) {
				if (!fall.equals(entry) && trueEntries.contains(fall)) {
					note("fall-into", a, "-> " + nameAt(fall));
				}
				else {
					work.push(fall);
				}
			}
		}
		return seen;
	}

	private void merge(Function fragment, Function owner) {
		Address entry = fragment.getEntryPoint();
		String name = fragment.getName();
		SourceType source = fragment.getSymbol().getSource();
		currentProgram.getFunctionManager().removeFunction(entry);
		Symbol label = getSymbolAt(entry);
		if (source != SourceType.DEFAULT && (label == null || !label.getName().equals(name))) {
			try {
				createLabel(entry, name, true, source);
			}
			catch (Exception e) {
				printerr("RedrawBoundaries: could not keep label " + name + ": " + e.getMessage());
			}
		}
		record("merged", entry, name, "into " + owner.getName());
	}

	private String nameAt(Address a) {
		Function f = getFunctionContaining(a);
		Symbol s = getSymbolAt(a);
		return s != null && f != null && !s.getName().equals(f.getName()) ? f.getName() + ":" + s.getName()
				: s != null ? s.getName() : f != null ? f.getName() + "+" + a.subtract(f.getEntryPoint()) : a.toString();
	}

	private static String names(List<Function> fs) {
		StringJoiner j = new StringJoiner(",", "in ", "");
		fs.forEach(f -> j.add(f.getName()));
		return j.toString();
	}

	private void note(String action, Address a, String detail) {
		flowNotes.add(List.of(action, a.toString(), nameAt(a), detail));
	}

	private void record(String action, Address a, String name, String detail) {
		report.add(List.of(action, a.toString(), name, detail));
	}
}
