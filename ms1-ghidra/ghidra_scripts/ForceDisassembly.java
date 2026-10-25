// Disassemble instructions the import gets wrong, before auto-analysis: whatever overlaps
// each listed instruction is cleared and the instruction is disassembled there, with the flow
// after it. Once placed, no other decode can start inside it.
// Headless: -preScript ForceDisassembly.java
//@category MS1

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Instruction;

public class ForceDisassembly extends GhidraScript {

	private record Start(String address, int length, String evidence) {}

	private static final Start[] STARTS = {
		new Start("bfff", 3,
			"JSR $BBEE: its opcode is page 3D's last byte, its operand the fixed bytes at 0xC000 " +
				"(IDA left the byte as data and decoded 0xC000 as an instruction)"),
	};

	@Override
	protected void run() throws Exception {
		for (Start s : STARTS) {
			Address start = toAddr(s.address);
			clearListing(start, start.add(s.length - 1));
			disassemble(start);
			Instruction insn = getInstructionAt(start);
			println(String.format("ForceDisassembly: %s %s [%s]", start, insn, s.evidence));
		}
	}
}
