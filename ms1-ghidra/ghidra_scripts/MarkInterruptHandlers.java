// Give every interrupt handler the __interrupt calling convention: entered with CCR, B, A, X, Y
// and the return PC stacked, left by RTI. Handlers are found from the vector table: vector
// 0xFF80 + 2*n holds the trampoline address, and the trampoline is a thunk of (or jumps to) the
// handler. The reset vector is skipped, and so is any handler also reached by a call, which is
// reported. A vector whose trampoline jumps to non-code is unused: the trampoline itself gets the
// convention and its jump becomes a call (CALL_RETURN), so it decompiles as a call to that
// address instead of decoding data.
// Headless: -postScript MarkInterruptHandlers.java (before ApplySignatures.java)
//@category MS1

import java.util.Set;
import java.util.TreeSet;

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.FlowOverride;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.symbol.Reference;

public class MarkInterruptHandlers extends GhidraScript {

	private static final long VECTORS = 0xFF80;
	private static final long RESET_VECTOR = 0xFFFE;
	private static final String CONVENTION = "__interrupt";

	@Override
	protected void run() throws Exception {
		Set<String> marked = new TreeSet<>();
		int unused = 0;
		for (long v = VECTORS; v < RESET_VECTOR; v += 2) {
			Address trampoline = toAddr(getShort(toAddr(v)) & 0xffff);
			if (jumpsToNonCode(trampoline)) {
				getInstructionAt(trampoline).setFlowOverride(FlowOverride.CALL_RETURN);
				Function t = getFunctionAt(trampoline);
				if (t == null) {
					t = createFunction(trampoline, null);
				}
				t.setCallingConvention(CONVENTION);
				unused++;
				continue;
			}
			Function handler = handlerBehind(trampoline);
			if (handler == null) {
				printerr(String.format("MarkInterruptHandlers: vector %04x -> %s has no handler function", v, trampoline));
				continue;
			}
			if (marked.contains(handler.getName())) {
				continue;
			}
			if (isCalled(handler)) {
				println(String.format("MarkInterruptHandlers: %s is also called; convention left as is", handler.getName()));
				continue;
			}
			handler.setCallingConvention(CONVENTION);
			marked.add(handler.getName());
		}
		println(String.format("MarkInterruptHandlers: %d unused vectors; handlers %s", unused, marked));
	}

	private boolean jumpsToNonCode(Address trampoline) {
		Instruction insn = getInstructionAt(trampoline);
		if (insn == null || insn.getDefaultFlows().length != 1) {
			return false;
		}
		return getInstructionAt(insn.getDefaultFlows()[0]) == null;
	}

	private Function handlerBehind(Address trampoline) {
		Function t = getFunctionAt(trampoline);
		if (t != null && t.isThunk()) {
			return t.getThunkedFunction(false);
		}
		if (t != null) {
			return t;
		}
		var insn = getInstructionAt(trampoline);
		Address[] flows = insn == null ? new Address[0] : insn.getFlows();
		return flows.length == 1 ? getFunctionAt(flows[0]) : null;
	}

	private boolean isCalled(Function f) {
		for (Reference r : currentProgram.getReferenceManager().getReferencesTo(f.getEntryPoint())) {
			if (r.getReferenceType().isCall() && getInstructionAt(r.getFromAddress()) != null &&
				getInstructionAt(r.getFromAddress()).getFlowOverride() == FlowOverride.NONE) {
				return true;
			}
		}
		return false;
	}
}
