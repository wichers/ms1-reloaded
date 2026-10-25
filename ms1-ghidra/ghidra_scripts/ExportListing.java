// Export every instruction of the program as TSV: address, instruction bytes (hex), mnemonic,
// and the instruction as Ghidra prints it. Overlay blocks are left out (their addresses repeat
// the PPAGE window's); everything else is the CPU-visible address space with page 3D in the
// window. The emulator's decoder check (ms1-emu tests) compares against this listing.
// Headless: -postScript ExportListing.java <out.tsv>
//@category MS1

import java.io.PrintWriter;

import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.Instruction;

public class ExportListing extends GhidraScript {

	@Override
	protected void run() throws Exception {
		int n = 0;
		try (PrintWriter w = new PrintWriter(getScriptArgs()[0])) {
			for (Instruction i : currentProgram.getListing().getInstructions(true)) {
				if (i.getAddress().getAddressSpace().isOverlaySpace()) {
					continue;
				}
				StringBuilder bytes = new StringBuilder();
				for (byte b : i.getBytes()) {
					bytes.append(String.format("%02x", b & 0xff));
				}
				w.printf("%s\t%s\t%s\t%s%n", i.getAddress(), bytes, i.getMnemonicString(), i);
				n++;
			}
		}
		println("ExportListing: " + n + " instructions");
	}
}
