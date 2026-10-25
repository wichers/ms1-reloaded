// Name functions and their parameters where the code shows what they are, on top of the
// committed signatures: parameter storage (registers, stack offsets) stays as analysis found it,
// only names and types change, so the listed parameters must match the committed ones in number
// ("void" removes them all).
// A rename replaces an imported IDA name the code contradicts. Each entry carries its evidence;
// everything set here is User Defined.
// Headless: -postScript AnnotateFunctions.java (after CommitSignatures.java)
//@category MS1

import java.util.ArrayList;
import java.util.List;

import ghidra.app.script.GhidraScript;
import ghidra.app.services.DataTypeManagerService;
import ghidra.program.model.data.DataType;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.Function.FunctionUpdateType;
import ghidra.program.model.listing.Parameter;
import ghidra.program.model.listing.ParameterImpl;
import ghidra.program.model.symbol.SourceType;
import ghidra.util.data.DataTypeParser;
import ghidra.util.data.DataTypeParser.AllowedDataTypes;

public class AnnotateFunctions extends GhidraScript {

	/** params: null keeps them, "void" removes them, else "type name, ..." positionally. */
	private record Annotation(String entry, String name, String returns, String params, String evidence) {}

	private static final Annotation[] ANNOTATIONS = {
		new Annotation("b9d6", "main_loop", null, "void",
			"called once from the CRT path (0x4072) after app_main_init; loops forever over ADC, signal generator, encoder, " +
				"send-buffer executor, command consumer, CAN handler and task scheduler (the IY input is not an argument)"),
		// CAN (docs/firmware-map.md: vectors 0xFFB0/0xFFB2/0xFFB4)
		new Annotation("dd1b", "can0_tx_isr", null, null,
			"CAN0 transmit vector: loads the next can_tx_ring entry into the MSCAN TX buffer, disables TX interrupts when empty"),
		new Annotation("dd61", "can0_rx_isr", null, null,
			"CAN0 receive vector: copies the MSCAN RX buffer into can_rx_ring, clears RXF"),
		new Annotation("dd10", "can0_error_isr", null, null, "CAN0 errors vector: clears the overrun flag (CAN0RFLG bit 1)"),
		new Annotation("de43", null, null, "short id_low, short id_high, byte * data, byte dlc",
			"encodes id_high:id_low as a 29-bit extended ID (id_high < 0: an 11-bit standard ID from id_low) into IDR0-3, " +
				"copies 8 data bytes and dlc into can_tx_ring, enables the TX interrupt"),
		new Annotation("deac", null, "short", "can_msg_t * msg",
			"returns 0 when can_rx_ring is empty, else decodes the next entry into *msg and returns 1"),
		// EEPROM: a 4-byte sector is staged in eeprom_sector_image, erased if it differs, programmed by eeprom_write_finish
		new Annotation("8dd8", "eeprom_write_byte", null, "ushort addr, byte value",
			"stages the sector (addr & ~3) with value at addr & 3; if it differs from EEPROM, records it and erases the sector"),
		new Annotation("8e18", "eeprom_write_word", null, "ushort addr, ushort value",
			"as eeprom_write_byte for the word at addr & 2"),
		new Annotation("8e58", "eeprom_write_sector", null, "ushort * sector, ushort word0, ushort word1",
			"stages both words of the sector; erases it if it differs"),
		new Annotation("8db2", "eeprom_write_finish", null, null,
			"programs the staged sector's two words (eeprom_program_word) if one is pending, then clears the pending pointer"),
	};

	@Override
	protected void run() throws Exception {
		DataTypeParser parser = new DataTypeParser(currentProgram.getDataTypeManager(), null,
			(DataTypeManagerService) null, AllowedDataTypes.ALL);
		for (Annotation a : ANNOTATIONS) {
			Function f = getFunctionAt(toAddr(a.entry));
			if (f == null) {
				printerr("AnnotateFunctions: no function at " + a.entry);
				continue;
			}
			String before = f.getName();
			f.setCustomVariableStorage(true);
			if (a.name != null) {
				f.setName(a.name, SourceType.USER_DEFINED);
			}
			if (a.returns != null) {
				f.setReturnType(parser.parse(a.returns), SourceType.USER_DEFINED);
			}
			if (a.params != null) {
				List<Parameter> params = new ArrayList<>();
				Parameter[] existing = f.getParameters();
				String[] specs = a.params.equals("void") ? new String[0] : a.params.split(",");
				if (specs.length != existing.length && specs.length != 0) {
					printerr(String.format("AnnotateFunctions: %s has %d committed parameters, %d listed; parameters left as is",
						before, existing.length, specs.length));
				}
				else {
					for (int i = 0; i < specs.length; i++) {
						String spec = specs[i].trim();
						int sp = spec.lastIndexOf(' ');
						DataType t = parser.parse(spec.substring(0, sp).trim());
						params.add(new ParameterImpl(spec.substring(sp + 1), t, existing[i].getVariableStorage(),
							currentProgram, SourceType.USER_DEFINED));
					}
					f.updateFunction(f.getCallingConventionName(), f.getReturn(), params,
						FunctionUpdateType.CUSTOM_STORAGE, true, SourceType.USER_DEFINED);
				}
			}
			println(String.format("AnnotateFunctions: %s -> %s [%s]", before, f.getPrototypeString(false, false), a.evidence));
		}
	}
}
