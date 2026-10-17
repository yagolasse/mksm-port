// List functions referencing the given addresses; args: outfile addr...
import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.*;
import ghidra.program.model.address.*;
import java.io.*;
import java.util.*;
public class RefsTo extends GhidraScript {
  public void run() throws Exception {
    String[] a = getScriptArgs();
    PrintWriter pw = new PrintWriter(new FileWriter(a[0]));
    for (int i = 1; i < a.length; i++) {
      Address t = toAddr(Long.parseLong(a[i].replace("0x",""), 16));
      TreeMap<String,Integer> m = new TreeMap<>();
      for (Reference r : getReferencesTo(t)) {
        Function f = getFunctionContaining(r.getFromAddress());
        String k = (f == null ? "none" : f.getEntryPoint().toString()) + " " + r.getReferenceType();
        m.merge(k, 1, Integer::sum);
      }
      pw.println("== " + a[i]);
      for (Map.Entry<String,Integer> e : m.entrySet()) pw.println(e.getKey() + " x" + e.getValue());
    }
    pw.close();
  }
}
