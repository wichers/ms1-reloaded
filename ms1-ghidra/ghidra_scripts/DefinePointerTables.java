// Data the IDA database kept as code, and code-pointer tables feeding a computed JMP or JSR:
// clear the listing, define each table as code pointers, and tie the dispatch instruction to
// exactly those targets. Without this, switch recovery cannot bound the index and reads
// "targets" past the table end, and data regions disassembled as code invent references.
// Runs before auto-analysis.
//   DATA_REGIONS: cleared of instructions and functions; IDA's data definitions stay, and every
//                 remaining undefined run becomes a byte array so no analyzer disassembles it.
//   JUMP tables:  COMPUTED_JUMP references plus a decompiler jump-table override. Entries that
//                 point back into the table are not code (unused indices) and are skipped.
//   JUMP_REFS:    COMPUTED_JUMP references only, where the override makes the decompiler die.
//   CALL tables:  COMPUTED_CALL references, and a function at every target.
//   DATA tables:  pointers only (no known dispatch), and a function at every target.
// A table value resolves in the dispatch instruction's address space when that space holds it,
// otherwise in the default space.
// Headless: -preScript DefinePointerTables.java
//@category MS1

import java.util.ArrayList;
import java.util.LinkedHashSet;
import java.util.Set;

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.address.AddressSet;
import ghidra.program.model.data.ArrayDataType;
import ghidra.program.model.data.ByteDataType;
import ghidra.program.model.data.PointerDataType;
import ghidra.program.model.listing.Data;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.pcode.JumpTable;
import ghidra.program.model.symbol.RefType;
import ghidra.program.model.symbol.Reference;
import ghidra.program.model.symbol.SourceType;
import ghidra.program.model.symbol.Symbol;

public class DefinePointerTables extends GhidraScript {

	private static final int POINTER_SIZE = 2;

	private enum Kind { JUMP, JUMP_REFS, CALL, DATA }

	private record Region(String start, String end, String evidence) {}

	private record Table(String dispatch, String start, int count, Kind kind, String evidence) {}

	private static final Region[] DATA_REGIONS = {
		new Region("e0a4", "e0af",
			"the string \"End of .text\": the 56 unused interrupt vectors' trampolines jump here; code resumes at 0xE0B0"),
		new Region("e11b", "f73f",
			"UI strings (EN/DE/FR/NL), menu and dispatch tables, LCD segment/font/charset tables, " +
				"signal-decode state pointers, MM trit tables, version strings, BCI header"),
		new Region("ff00", "ff7f",
			"bootloader configuration block: vendor string, IDs, expected CRC, scales, bootloader entry pointer"),
	};

	private static final Table[] TABLES = {
		new Table("c641", "c643", 22, Kind.JUMP,
			"signal_gen send-buffer executor: ASLD; ADDD #table; XGDY; LDY 0,Y; JMP 0,Y"),
		new Table("c87a", "c87c", 14, Kind.JUMP,
			"signal_gen command consumer, by command type: ASLD; ADDD #table; XGDY; LDY 0,Y; ...; JMP 0,Y"),
		new Table("ca1c", "ca1e", 8, Kind.JUMP,
			"signal_gen command consumer, type 9 (Motorola) sub-dispatch: LSLB; ADDD #table; XGDY; LDY 0,Y; ...; JMP 0,Y"),
		new Table("c866", "c899", 14, Kind.CALL,
			"signal_gen command consumer, by previous command type: ASLD; ADDD #table; XGDY; LDY 0,Y; ...; JSR 0,Y"),
		new Table("be3b", "ec6b", 53, Kind.JUMP,
			"sprintf conversion character 'D'..'x': SUBD #'D'; CPD #$34; ASLD; TFR D,Y; LDX -$1395,Y; JMP 0,X"),
		new Table(null, "f352", 4, Kind.DATA, "signal-decode state handlers"),
		// Page-3D switches indexing tables in fixed flash: CPD #n; BHI default; ASLD; TFR D,Y;
		// LDX -disp,Y; JMP 0,X. The table is at 0x10000 - disp (16-bit wrap), n + 1 entries.
		new Table("8c14", "ea42", 6, Kind.JUMP, "CPD #5; LDX -$15BE,Y"),
		new Table("a1dc", "ea6e", 9, Kind.JUMP, "CPD #8; LDX -$1592,Y"),
		new Table("a8d3", "ea92", 8, Kind.JUMP, "CPD #7; LDX -$156E,Y"),
		new Table("b506", "ead7", 10, Kind.JUMP, "CPD #9; LDX -$1529,Y"),
		new Table("b6bf", "eaeb", 9, Kind.JUMP, "CPD #8; LDX -$1515,Y"),
		new Table("bafc", "bafe", 16, Kind.JUMP_REFS,
			"rotary-encoder transition: ASLD; ASLD; ORAB flags; ADDD #table; TFR D,Y; LDY 0,Y; JMP 0,Y; table follows the jump"),
	};

	@Override
	protected void run() throws Exception {
		for (Region r : DATA_REGIONS) {
			Address start = toAddr(r.start);
			Address end = toAddr(r.end);
			removeFunctionsIn(start, end);
			int cleared = 0;
			for (Instruction insn : currentProgram.getListing().getInstructions(new AddressSet(start, end), true)) {
				clearListing(insn.getMinAddress(), insn.getMaxAddress());
				cleared++;
			}
			int gaps = 0;
			Address a = start;
			while (a != null && a.compareTo(end) <= 0) {
				Data undefined = getUndefinedDataAt(a);
				if (undefined == null) {
					a = currentProgram.getListing().getCodeUnitAt(a) == null ? a.next()
							: currentProgram.getListing().getCodeUnitAt(a).getMaxAddress().next();
					continue;
				}
				Address run = a;
				while (run.compareTo(end) < 0 && getUndefinedDataAt(run.next()) != null) {
					run = run.next();
				}
				createData(a, new ArrayDataType(ByteDataType.dataType, (int) run.subtract(a) + 1, 1));
				gaps++;
				a = run.next();
			}
			println(String.format("DefinePointerTables: %s-%s data: %d instructions cleared, %d undefined runs made byte arrays [%s]",
				start, end, cleared, gaps, r.evidence));
		}
		for (Table t : TABLES) {
			define(t);
		}
	}

	private void define(Table t) throws Exception {
		Address dispatch = t.dispatch == null ? null : toAddr(t.dispatch);
		Address start = toAddr(t.start);
		Address end = start.add((long) POINTER_SIZE * t.count - 1);

		Symbol name = getSymbolAt(start);
		String tableName = name == null ? null : name.getName();
		SourceType tableSource = name == null ? null : name.getSource();
		removeFunctionsIn(start, end);
		clearListing(start, end);
		createData(start, new ArrayDataType(new PointerDataType(), t.count, POINTER_SIZE));
		if (tableName != null && getSymbolAt(start) == null) {
			createLabel(start, tableName, true, tableSource);
		}

		Set<Address> targets = new LinkedHashSet<>();
		int notCode = 0;
		for (int i = 0; i < t.count; i++) {
			long value = getShort(start.add((long) POINTER_SIZE * i)) & 0xffff;
			Address target = resolve(dispatch, value);
			if (target.getOffset() >= start.getOffset() && target.getOffset() <= end.getOffset()) {
				notCode++;
			}
			else {
				targets.add(target);
			}
		}

		if (t.kind == Kind.CALL || t.kind == Kind.DATA) {
			for (Address target : targets) {
				if (getFunctionAt(target) == null) {
					disassemble(target);
					createFunction(target, null);
				}
			}
		}
		String where = "pointers";
		if (dispatch != null) {
			Instruction insn = getInstructionAt(dispatch);
			for (Reference r : insn.getReferencesFrom()) {
				if (r.getReferenceType().isFlow()) {
					removeReference(r);
				}
			}
			RefType type = t.kind == Kind.CALL ? RefType.COMPUTED_CALL : RefType.COMPUTED_JUMP;
			for (Address target : targets) {
				insn.addOperandReference(0, target, type, SourceType.ANALYSIS);
			}
			where = t.kind == Kind.CALL ? "call targets" : "jump references";
			if (t.kind == Kind.JUMP) {
				Function owner = getFunctionContaining(dispatch);
				if (owner == null) {
					printerr(String.format("DefinePointerTables: %s is in no function; no override written", dispatch));
					return;
				}
				new JumpTable(dispatch, new ArrayList<>(targets), true, 0).writeOverride(owner);
				where = "override in " + owner.getName();
			}
		}
		println(String.format("DefinePointerTables: %s %d pointers at %s -> %d distinct, %s (%d entries point into the table) [%s]",
			tableName, t.count, start, targets.size(), where, notCode, t.evidence));
	}

	private Address resolve(Address dispatch, long value) {
		if (dispatch != null && dispatch.getAddressSpace().isOverlaySpace()) {
			Address inSpace = dispatch.getNewAddress(value);
			if (currentProgram.getMemory().contains(inSpace)) {
				return inSpace;
			}
		}
		return toAddr(value);
	}

	private void removeFunctionsIn(Address start, Address end) {
		for (Function f : currentProgram.getFunctionManager().getFunctions(start, true)) {
			if (f.getEntryPoint().compareTo(end) > 0) {
				break;
			}
			removeFunction(f);
		}
	}
}
