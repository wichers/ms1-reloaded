// Apply verified function prototypes, each with the code that shows its convention, as User
// Defined so CommitSignatures.java leaves them alone and callers decompile against them. Used
// where the decompiler's own parameter analysis goes wrong, typically in hand-written code that
// reads its caller's frame. The function keeps its name; only parameters and return change.
// Headless: -postScript ApplySignatures.java (after RedrawBoundaries.java, before CommitSignatures.java)
//@category MS1

import ghidra.app.cmd.function.ApplyFunctionSignatureCmd;
import ghidra.app.cmd.function.FunctionRenameOption;
import ghidra.app.script.GhidraScript;
import ghidra.app.services.DataTypeManagerService;
import ghidra.app.util.cparser.C.CParserUtils;
import ghidra.program.model.data.FunctionDefinitionDataType;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.SourceType;

public class ApplySignatures extends GhidraScript {

	private record Signature(String entry, String returns, String params, String evidence) {}

	private static final Signature[] SIGNATURES = {
		new Signature("c81d", "short", "short count",
			"SUBD #1; PSHD; BCC ...; PULD; RTS: D is a countdown in and the remainder out " +
				"(its interpreter cases read the loop's own stack slots, which parameter analysis takes for arguments)"),
	};

	@Override
	protected void run() throws Exception {
		for (Signature s : SIGNATURES) {
			Function f = getFunctionAt(toAddr(s.entry));
			if (f == null) {
				printerr("ApplySignatures: no function at " + s.entry);
				continue;
			}
			String text = String.format("%s %s(%s)", s.returns, f.getName(), s.params);
			FunctionDefinitionDataType sig =
				CParserUtils.parseSignature((DataTypeManagerService) null, currentProgram, text, true);
			boolean ok = new ApplyFunctionSignatureCmd(f.getEntryPoint(), sig, SourceType.USER_DEFINED, true,
				FunctionRenameOption.NO_CHANGE).applyTo(currentProgram);
			println(String.format("ApplySignatures: %s %s [%s]", ok ? "applied" : "FAILED", text, s.evidence));
		}
	}
}
