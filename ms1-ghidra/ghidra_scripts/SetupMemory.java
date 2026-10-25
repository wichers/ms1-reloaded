// Lay out and set permissions on the memory blocks the IDA XML gets wrong, before auto-analysis.
//
// Low memory follows the application's CRT init (0x4000): INITRG=$00 puts the registers at
// 0x0000, INITEE=$09 the 1 KB EEPROM at 0x0800, INITRM=$10 the 4 KB RAM at 0x1000 (stack top
// LDS #$2000). The imported blocks below 0x4000 are split and renamed to that map; the rest of
// it is unmapped.
// Uninitialized blocks (registers, EEPROM, RAM, unmapped) are writable, initialized flash is
// executable, and the I/O registers (FSR) are volatile so the decompiler never folds their
// reads. DATA_PAGES are flash pages the CPU never executes: page 3C is only mapped by the
// bootloader's CRC scan (MOVB #$3C,PPAGE at 0xF895, data reads), so any code the import
// placed there is cleared.
// Headless: -preScript SetupMemory.java
//@category MS1

import java.util.Set;

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.mem.Memory;
import ghidra.program.model.mem.MemoryBlock;

public class SetupMemory extends GhidraScript {

	private static final Set<String> DATA_PAGES = Set.of("FLASH_3C");

	/** { start, block name } for 0x0000-0x3FFF, each block running to the next start. */
	private static final Object[][] LOW_MAP = {
		{ 0x0000L, "FSR" },
		{ 0x0400L, "UNMAPPED_0400" },
		{ 0x0800L, "EEPROM" },
		{ 0x0C00L, "UNMAPPED_0C00" },
		{ 0x1000L, "RAM" },
		{ 0x2000L, "UNMAPPED_2000" },
	};

	@Override
	protected void run() throws Exception {
		Memory memory = currentProgram.getMemory();
		for (Object[] entry : LOW_MAP) {
			Address start = toAddr((Long) entry[0]);
			MemoryBlock block = memory.getBlock(start);
			if (block == null) {
				printerr("SetupMemory: no block at " + start);
				continue;
			}
			if (!block.getStart().equals(start)) {
				memory.split(block, start);
				block = memory.getBlock(start);
			}
			block.setName((String) entry[1]);
		}

		for (MemoryBlock block : memory.getBlocks()) {
			String name = block.getName();
			boolean dataPage = DATA_PAGES.contains(name);
			block.setRead(true);
			block.setWrite(!block.isInitialized());
			block.setExecute(block.isInitialized() && name.startsWith("FLASH_") && !dataPage);
			block.setVolatile(name.equals("FSR"));
			if (dataPage) {
				for (var f : currentProgram.getFunctionManager().getFunctions(block.getStart(), true)) {
					if (!block.contains(f.getEntryPoint())) {
						break;
					}
					removeFunction(f);
				}
				currentProgram.getListing().clearCodeUnits(block.getStart(), block.getEnd(), false);
			}
			println(String.format("%-13s %s-%s %s%s%s%s%s", name, block.getStart(), block.getEnd(),
				block.isRead() ? "r" : "-", block.isWrite() ? "w" : "-", block.isExecute() ? "x" : "-",
				block.isVolatile() ? " volatile" : "", dataPage ? " data page, code cleared" : ""));
		}
	}
}
