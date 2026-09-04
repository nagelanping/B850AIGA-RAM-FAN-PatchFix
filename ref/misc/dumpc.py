# Dump all functions decompiled to a text file
# @category Analysis
from ghidra.app.decompiler import DecompInterface
from ghidra.util.task import ConsoleTaskMonitor

prog = getCurrentProgram()
ifc = DecompInterface()
ifc.openProgram(prog)

out = []
fm = prog.getFunctionManager()
funcs = fm.getFunctions(True)
for f in funcs:
    res = ifc.decompileFunction(f, 60, ConsoleTaskMonitor())
    if res and res.decompileCompleted():
        out.append("//===== %s @ 0x%x =====\n%s" % (f.getName(), f.getEntryPoint().getOffset(), res.getDecompiledFunction().getC()))

with open("/tmp/bioswork/decomp_%s.c" % prog.getName(), "w") as fp:
    fp.write("\n\n".join(out))
print("dumped %d functions" % len(funcs))