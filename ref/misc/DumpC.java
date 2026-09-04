// Dump all functions decompiled to a text file (Java)
// @category Analysis
import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.util.task.ConsoleTaskMonitor;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.FunctionIterator;
import java.io.FileWriter;

public class DumpC extends GhidraScript {
    @Override
    public void run() throws Exception {
        DecompInterface ifc = new DecompInterface();
        ifc.openProgram(currentProgram);
        StringBuilder out = new StringBuilder();
        FunctionIterator funcs = currentProgram.getFunctionManager().getFunctions(true);
        int n = 0;
        for (Function f : funcs) {
            DecompileResults res = ifc.decompileFunction(f, 60, new ConsoleTaskMonitor());
            if (res != null && res.decompileCompleted()) {
                out.append("//===== ").append(f.getName()).append(" @ 0x")
                   .append(Long.toHexString(f.getEntryPoint().getOffset())).append(" =====\n")
                   .append(res.getDecompiledFunction().getC()).append("\n\n");
                n++;
            }
        }
        String name = currentProgram.getName().replace("/", "_");
        String path = "/tmp/bioswork/decomp_" + name + ".c";
        FileWriter fw = new FileWriter(path);
        fw.write(out.toString());
        fw.close();
        println("dumped " + n + " functions to " + path);
    }
}