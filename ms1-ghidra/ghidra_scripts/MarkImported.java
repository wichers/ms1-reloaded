// Mark every symbol the XML import created as Imported, before auto-analysis.
// The XML loader stores IDA's names as User Defined; as Imported they stay distinguishable
// from names confirmed or added in Ghidra, which are User Defined.
// Headless: -preScript MarkImported.java
//@category MS1

import ghidra.app.script.GhidraScript;
import ghidra.program.model.symbol.SourceType;
import ghidra.program.model.symbol.Symbol;

public class MarkImported extends GhidraScript {

	@Override
	protected void run() throws Exception {
		int marked = 0;
		for (Symbol symbol : currentProgram.getSymbolTable().getAllSymbols(true)) {
			if (symbol.getSource() == SourceType.USER_DEFINED) {
				symbol.setSource(SourceType.IMPORTED);
				marked++;
			}
		}
		println(String.format("MarkImported: %d symbols marked Imported", marked));
	}
}
