// Decompile functions at given addresses; args: outfile addr...
import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.*;
import ghidra.program.model.listing.*;
import ghidra.program.model.address.*;
import java.io.*;
public class DecompAt extends GhidraScript {
  public void run() throws Exception {
    String[] a = getScriptArgs();
    PrintWriter pw = new PrintWriter(new FileWriter(a[0]));
    DecompInterface d = new DecompInterface();
    d.openProgram(currentProgram);
    for (int i = 1; i < a.length; i++) {
      Address ad = toAddr(Long.parseLong(a[i].replace("0x",""), 16));
      Function f = getFunctionContaining(ad);
      if (f == null) { f = createFunction(ad, null); }
      if (f == null) { pw.println("// no function at " + a[i]); continue; }
      DecompileResults r = d.decompileFunction(f, 120, monitor);
      pw.println("// ===== " + a[i] + " " + f.getName() + " @ " + f.getEntryPoint());
      pw.println(r.decompileCompleted() ? r.getDecompiledFunction().getC() : "// failed " + r.getErrorMessage());
    }
    pw.close();
  }
}
