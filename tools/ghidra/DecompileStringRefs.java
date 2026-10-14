// Decompile the functions behind strings or addresses of interest.
//
// Meant for locating subsystems (file loaders, etc.) from their string
// literals and then following the calls out of them.
//
// Script args: arg0 = output file, arg1.. = targets, each one of:
//   text        an ASCII string: decompile every function referencing it
//   @0x1234     an address: decompile every function referencing it
//   #0x1234     decompile the function containing that address
//
// @category XboxRecomp

import java.io.FileWriter;
import java.io.Writer;
import java.nio.charset.StandardCharsets;
import java.util.LinkedHashSet;
import java.util.Set;

import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.mem.Memory;
import ghidra.program.model.symbol.Reference;

public class DecompileStringRefs extends GhidraScript {

    private DecompInterface decomp;

    @Override
    protected void run() throws Exception {
        String[] args = getScriptArgs();
        if (args.length < 2) {
            printerr("usage: DecompileStringRefs <out_file> <target> [target...]");
            return;
        }
        Memory mem = currentProgram.getMemory();
        decomp = new DecompInterface();
        decomp.openProgram(currentProgram);

        try (Writer w = new FileWriter(args[0])) {
            for (int i = 1; i < args.length; i++) {
                String t = args[i];
                Set<Function> funcs = new LinkedHashSet<>();
                w.write("// ===== " + t + " =====\n");

                if (t.startsWith("#")) {
                    Function f = getFunctionContaining(toAddr(Long.decode(t.substring(1))));
                    if (f != null) {
                        funcs.add(f);
                    }
                } else if (t.startsWith("@")) {
                    collectRefs(w, toAddr(Long.decode(t.substring(1))), funcs);
                } else {
                    byte[] needle = t.getBytes(StandardCharsets.US_ASCII);
                    Address at = mem.findBytes(mem.getMinAddress(), needle, null, true, monitor);
                    while (at != null) {
                        w.write("// string at " + at + "\n");
                        collectRefs(w, at, funcs);
                        at = mem.findBytes(at.add(1), needle, null, true, monitor);
                    }
                }
                writeDecomp(w, funcs);
            }
        }
        decomp.dispose();
        println("Wrote " + args[0]);
    }

    private void collectRefs(Writer w, Address at, Set<Function> funcs) throws Exception {
        for (Reference r : getReferencesTo(at)) {
            Address from = r.getFromAddress();
            Function f = getFunctionContaining(from);
            w.write("//   ref from " + from + " in " + (f == null ? "<no function>" : f.getName()) + "\n");
            if (f != null) {
                funcs.add(f);
            }
        }
    }

    private void writeDecomp(Writer w, Set<Function> funcs) throws Exception {
        for (Function f : funcs) {
            DecompileResults res = decomp.decompileFunction(f, 60, monitor);
            w.write("\n// ----- " + f.getName() + " @ " + f.getEntryPoint() + " -----\n");
            if (res != null && res.decompileCompleted()) {
                w.write(res.getDecompiledFunction().getC());
            } else {
                w.write("// decompile failed: " + (res == null ? "no result" : res.getErrorMessage()) + "\n");
            }
        }
        w.write("\n");
    }
}
