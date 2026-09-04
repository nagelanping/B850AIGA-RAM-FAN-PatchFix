// Set entry point + create function there, then re-run
// @category Analysis
import ghidra.program.model.address.Address;
import ghidra.program.model.symbol.SourceType;
import ghidra.app.cmd.disassemble.DisassembleCommand;
import ghidra.program.model.listing.CodeUnit;

public class FixEntry extends GhidraScript {
    @Override
    public void run() throws Exception {
        long entry;
        if (currentProgram.getName().contains("SkSmartFanCtrlPei")) entry = 0x9C02A64L + 0x399L;
        else entry = 0x9BFD57CL + 0x200L;
        Address a = currentProgram.getAddressFactory().getDefaultAddressSpace().getAddress(entry);
        currentProgram.getSymbolTable().addExternalEntryPoint(a);
        DisassembleCommand cmd = new DisassembleCommand(a, null, true);
        cmd.applyTo(currentProgram);
        println("entry set @ " + a);
    }
}