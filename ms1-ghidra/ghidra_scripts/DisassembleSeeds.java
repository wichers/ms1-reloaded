// Disassemble a raw import from seed addresses, following the flow but only inside the given
// ranges: the "range LO HI" (HI exclusive) and "seed ADDR" lines (hex) that module_seeds.py
// writes for the firmware rebuild's assembly modules. With no auto-analysis, the result is
// exactly the code reachable from the seeds, so data inside a module (jump tables) stays data.
// Headless: -postScript DisassembleSeeds.java <seeds.txt>
//@category MS1

import java.nio.file.Files;
import java.nio.file.Path;

import ghidra.app.cmd.disassemble.DisassembleCommand;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.AddressSet;

public class DisassembleSeeds extends GhidraScript {

	@Override
	protected void run() throws Exception {
		AddressSet ranges = new AddressSet();
		AddressSet seeds = new AddressSet();
		for (String line : Files.readAllLines(Path.of(getScriptArgs()[0]))) {
			String[] f = line.trim().split("\\s+");
			if (f[0].equals("range")) {
				ranges.add(toAddr(Long.parseLong(f[1], 16)), toAddr(Long.parseLong(f[2], 16) - 1));
			}
			else if (f[0].equals("seed")) {
				seeds.add(toAddr(Long.parseLong(f[1], 16)));
			}
		}
		if (!ranges.contains(seeds)) {
			throw new IllegalArgumentException("DisassembleSeeds: a seed lies outside the ranges");
		}
		DisassembleCommand cmd = new DisassembleCommand(seeds, ranges, true);
		if (!cmd.applyTo(currentProgram, monitor)) {
			throw new IllegalStateException("DisassembleSeeds: " + cmd.getStatusMsg());
		}
		long n = currentProgram.getListing().getNumInstructions();
		println("DisassembleSeeds: " + seeds.getNumAddresses() + " seeds, " + n + " instructions");
	}
}
