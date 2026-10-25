// Define verified structures and apply verified types and names to global variables, each with
// the code that shows its layout. Pointer globals imported as plain words make the decompiler
// print every dereference as a cast and fold address arithmetic onto unrelated symbols.
// The covered range is cleared first; names given here are User Defined (an imported IDA name
// that the code contradicts is replaced; it stays as a secondary label).
// Headless: -postScript ApplyTypes.java (before CommitSignatures.java)
//@category MS1

import ghidra.app.script.GhidraScript;
import ghidra.app.services.DataTypeManagerService;
import ghidra.program.model.address.Address;
import ghidra.program.model.data.DataType;
import ghidra.program.model.data.DataTypeConflictHandler;
import ghidra.program.model.data.DataTypeManager;
import ghidra.program.model.data.StructureDataType;
import ghidra.program.model.symbol.SourceType;
import ghidra.program.model.symbol.Symbol;
import ghidra.util.data.DataTypeParser;
import ghidra.util.data.DataTypeParser.AllowedDataTypes;

public class ApplyTypes extends GhidraScript {

	private record Struct(String name, String fields, String evidence) {}

	private record Global(String address, String type, String name, String evidence) {}

	private static final Struct[] STRUCTS = {
		new Struct("mscan_frame_t", "byte[4] idr; byte[8] dsr; byte dlr",
			"CAN0 RX ISR copies CAN0RXIDR0-3, CAN0RXDSR0-7, CAN0RXDLR in this order, 13 bytes per entry; the TX ISR loads CAN0TXIDR/DSR/DLR from the same layout"),
		new Struct("can_msg_t", "ushort id_high; ushort id_low; byte[8] data; byte dlc",
			"can_rx_dequeue_frame: 29-bit ID from IDR0-3 into words 0/1 (id_high 0xFFFF for a standard ID), data words at 4..11, dlc & 0xF at 12"),
		new Struct("pt6547_tx_t", "byte stb; byte data",
			"isr_spi0_pt6547_pump: per SPI0 interrupt, the first byte sets the PT6547 strobe (PTM bit 3, 0 = low), the second goes to SPI0DR"),
	};

	private static final Global[] GLOBALS = {
		// signal generator (docs/track-signal.md)
		new Global("1160", "ushort[100]", null,
			"staging ring of 16-bit entries; pointers wrap from 0x1228 to 0x1160, executor free space computed modulo 200"),
		new Global("1014", "ushort *", null, "ISR reads *ptr as a 16-bit staging entry, wraps at 0x1228"),
		new Global("1016", "ushort *", null, "signal_gen_staging_buf_write: STD 2,Y+, wraps at 0x1228"),
		new Global("1228", "byte[64]", null, "send buffer; signal_gen_send_buf_advance: INX; CPX #$1268; LDX #$1228"),
		new Global("1018", "byte *", null, "send-buffer read pointer, advanced byte-wise with wrap at 0x1268"),
		new Global("101a", "byte *", null, "send-buffer write pointer"),
		new Global("1021", "ushort", null, "timer ticks: TC3/TC4/TC5 = base; base += entry & 0xFFF"),
		// track feedback capture
		new Global("1b4b", "byte[96]", "track_capture_buffer",
			"timer_capture_start_bit_mode clears 48 words from 0x1B4B; the capture pointer stops at 0x1BAB"),
		new Global("1bab", "byte *", null,
			"points into track_capture_buffer: bit mode stores 16-bit shift registers, ADC mode single bytes"),
		new Global("13a0", "byte *", null, "signal_read_bit: loads *ptr++ as the next byte to shift out"),
		// CAN rings (MSCAN register images); the imported pointer names have RX and TX swapped
		new Global("1929", "mscan_frame_t[10]", "can_tx_ring",
			"can_tx_enqueue_frame writes 13-byte entries from 0x1929, wrapping at 0x19AB"),
		new Global("19ab", "mscan_frame_t[32]", "can_rx_ring",
			"CAN0 RX ISR writes 13-byte entries from 0x19AB, wrapping at 0x1B4B"),
		new Global("1921", "mscan_frame_t *", "can_rx_ring_read", "can_rx_dequeue_frame reads and advances it (imported as can_tx_buf_ptr_0)"),
		new Global("1923", "mscan_frame_t *", "can_rx_ring_write", "CAN0 RX ISR writes and advances it (imported as can_tx_buf_ptr_1)"),
		new Global("1925", "mscan_frame_t *", "can_tx_ring_read", "CAN0 TX ISR reads and advances it (imported as can_rx_buf_ptr_0)"),
		new Global("1927", "mscan_frame_t *", "can_tx_ring_write", "can_tx_enqueue_frame writes and advances it (imported as can_rx_buf_ptr_1)"),
		// PT6547 display (SPI0)
		new Global("1108", "pt6547_tx_t[44]", null, "isr_spi0_pt6547_pump sends pairs from 0x1108 and wraps when the pointer reaches 0x1160"),
		new Global("1012", "pt6547_tx_t *", "pt6547_tx_ptr", "isr_spi0_pt6547_pump reads and advances it by 2 (imported as signal_gen_spi_buf_ptr)"),
		// EEPROM write staging (see AnnotateFunctions.java: eeprom_write_*)
		new Global("1082", "ushort *", "eeprom_pending_sector", "sector staged for eeprom_write_finish; 0 when none (imported as lok_db_flash_write_ptr)"),
		new Global("1084", "ushort[2]", "eeprom_sector_image", "the staged 4-byte sector: eeprom_write_* copy the sector here, patch it and compare (imported as lok_db_record_word0/1)"),
	};

	/** 16-bit registers the firmware accesses as words; IDA labels only their bytes (TC4Hi/TC4Lo). */
	private static final String[][] WORD_REGISTERS = {
		{ "0044", "TCNT" }, { "0076", "MCCNT" },
		{ "0050", "TC0" }, { "0052", "TC1" }, { "0054", "TC2" }, { "0056", "TC3" },
		{ "0058", "TC4" }, { "005a", "TC5" }, { "005c", "TC6" }, { "005e", "TC7" },
		{ "0090", "ATD0DR0" }, { "0092", "ATD0DR1" }, { "0094", "ATD0DR2" }, { "0096", "ATD0DR3" },
		{ "0098", "ATD0DR4" }, { "009a", "ATD0DR5" }, { "009c", "ATD0DR6" }, { "009e", "ATD0DR7" },
		{ "0150", "CAN0IDAR0_1" }, { "0152", "CAN0IDAR2_3" }, { "0154", "CAN0IDMR0_1" }, { "0156", "CAN0IDMR2_3" },
		{ "0158", "CAN0IDAR4_5" }, { "015a", "CAN0IDAR6_7" }, { "015c", "CAN0IDMR4_5" }, { "015e", "CAN0IDMR6_7" },
		{ "0160", "CAN0RXIDR0_1" }, { "0162", "CAN0RXIDR2_3" }, { "0170", "CAN0TXIDR0_1" }, { "0172", "CAN0TXIDR2_3" },
	};

	@Override
	protected void run() throws Exception {
		DataTypeManager dtm = currentProgram.getDataTypeManager();
		DataTypeParser parser = new DataTypeParser(dtm, null, (DataTypeManagerService) null, AllowedDataTypes.ALL);
		for (Struct st : STRUCTS) {
			StructureDataType struct = new StructureDataType(st.name, 0, dtm);
			for (String field : st.fields.split(";")) {
				String f = field.trim();
				int sp = f.lastIndexOf(' ');
				DataType ft = parser.parse(f.substring(0, sp).trim());
				struct.add(ft, ft.getLength(), f.substring(sp + 1), null);
			}
			struct.setPackingEnabled(false);
			DataType added = dtm.addDataType(struct, DataTypeConflictHandler.REPLACE_HANDLER);
			println(String.format("ApplyTypes: struct %s (%d bytes) [%s]", added.getName(), added.getLength(), st.evidence));
		}
		DataType word = parser.parse("ushort");
		for (String[] r : WORD_REGISTERS) {
			Address a = toAddr(r[0]);
			clearListing(a, a.add(1));
			createData(a, word);
			createLabel(a, r[1], true, SourceType.USER_DEFINED);
		}
		println(String.format("ApplyTypes: %d word registers typed (MC9S12D64 names)", WORD_REGISTERS.length));
		for (Global g : GLOBALS) {
			Address a = toAddr(g.address);
			DataType type = parser.parse(g.type);
			clearListing(a, a.add(type.getLength() - 1));
			createData(a, type);
			String name = g.name;
			if (name != null) {
				Symbol existing = getSymbolAt(a);
				if (existing == null || !existing.getName().equals(name)) {
					createLabel(a, name, true, SourceType.USER_DEFINED);
				}
			}
			Symbol s = getSymbolAt(a);
			println(String.format("ApplyTypes: %s %s %s [%s]", a, type.getName(), s == null ? "" : s.getName(),
				g.evidence));
		}
	}
}
