// Export function and symbol names from a Ghidra program as JSON.
//
// Java port of xboxrecomp's tools/ghidra_naming/ghidra_scripts/ExportXbeNames.py, which is a
// Jython script and no longer runs on Ghidra 12 (Jython was removed from the default install).
// Writes the same functions.json / symbols.json that tools/ghidra_naming/merge_names.py reads.
// The optional decompile export of the original is not ported.
//
// Script args: arg0 = output directory (default: <cwd>/ghidra_export)
//
// @category XboxRecomp

import java.io.File;
import java.io.FileWriter;
import java.io.IOException;
import java.io.Writer;

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.FunctionIterator;
import ghidra.program.model.symbol.Namespace;
import ghidra.program.model.symbol.Symbol;
import ghidra.program.model.symbol.SymbolIterator;

public class ExportXbeNames extends GhidraScript {

    private static String hex(Address a) {
        return String.format("0x%08X", a.getOffset());
    }

    private static String json(String s) {
        if (s == null) {
            return "\"\"";
        }
        StringBuilder b = new StringBuilder("\"");
        for (char c : s.toCharArray()) {
            switch (c) {
                case '"': b.append("\\\""); break;
                case '\\': b.append("\\\\"); break;
                case '\n': b.append("\\n"); break;
                case '\r': b.append("\\r"); break;
                case '\t': b.append("\\t"); break;
                default:
                    if (c < 0x20) {
                        b.append(String.format("\\u%04x", (int) c));
                    } else {
                        b.append(c);
                    }
            }
        }
        return b.append('"').toString();
    }

    private static String namespaceOf(Namespace ns) {
        if (ns == null || ns.isGlobal()) {
            return "";
        }
        return ns.getName(true);
    }

    private int exportFunctions(File outDir) throws IOException {
        int count = 0;
        try (Writer w = new FileWriter(new File(outDir, "functions.json"))) {
            w.write("[\n");
            FunctionIterator it = currentProgram.getFunctionManager().getFunctions(true);
            while (it.hasNext()) {
                Function f = it.next();
                String cc = f.getCallingConventionName();
                if (count > 0) {
                    w.write(",\n");
                }
                w.write(" {\"address\": " + json(hex(f.getEntryPoint())) +
                        ", \"name\": " + json(f.getName()) +
                        ", \"signature\": " + json(f.getPrototypeString(false, false)) +
                        ", \"calling_convention\": " + json(cc) +
                        ", \"param_count\": " + f.getParameterCount() +
                        ", \"is_thunk\": " + f.isThunk() +
                        ", \"namespace\": " + json(namespaceOf(f.getParentNamespace())) + "}");
                count++;
            }
            w.write("\n]\n");
        }
        println("Exported " + count + " functions -> " + new File(outDir, "functions.json"));
        return count;
    }

    private int exportSymbols(File outDir) throws IOException {
        int count = 0;
        try (Writer w = new FileWriter(new File(outDir, "symbols.json"))) {
            w.write("[\n");
            SymbolIterator it = currentProgram.getSymbolTable().getAllSymbols(true);
            while (it.hasNext()) {
                Symbol s = it.next();
                Address a = s.getAddress();
                if (a == null) {
                    continue;
                }
                if (count > 0) {
                    w.write(",\n");
                }
                w.write(" {\"address\": " + json(hex(a)) +
                        ", \"name\": " + json(s.getName()) +
                        ", \"type\": " + json(s.getSymbolType().toString()) +
                        ", \"namespace\": " + json(namespaceOf(s.getParentNamespace())) +
                        ", \"source\": " + json(s.getSource().toString()) +
                        ", \"primary\": " + s.isPrimary() + "}");
                count++;
            }
            w.write("\n]\n");
        }
        println("Exported " + count + " symbols -> " + new File(outDir, "symbols.json"));
        return count;
    }

    @Override
    protected void run() throws Exception {
        String[] args = getScriptArgs();
        File outDir = args.length > 0 && !args[0].isEmpty()
                ? new File(args[0])
                : new File(System.getProperty("user.dir"), "ghidra_export");
        outDir.mkdirs();

        println("=== ExportXbeNames (Java) ===");
        println("Program: " + currentProgram.getName());
        println("Out dir: " + outDir);
        exportFunctions(outDir);
        exportSymbols(outDir);
        println("=== Export complete ===");
    }
}
