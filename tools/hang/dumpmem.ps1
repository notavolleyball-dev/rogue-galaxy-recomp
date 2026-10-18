param(
    [Parameter(Mandatory=$true)][int]$ProcId,
    [Parameter(Mandatory=$true)][string]$OutDir,
    [Parameter(Mandatory=$true)][uint32]$Gp,
    [string]$CtxList = ""
)
# Dumps the game's guest RAM (32 MB) from a running or frozen roguegalaxy.exe
# to OutDir\rdram.bin, and every guest thread context (R5900Context) it can
# find to OutDir\ctx_<address>.bin: a 16-byte lane holding the game's gp
# (r[28], 0x485170 for SCUS_974.90) followed by a plausible sp (r[29]) and ra
# (r[31]); in the file r[i] is at i*16 (low word first) and the pc at 512.
# -CtxList "addr,addr" re-reads those contexts only. From the repo:
#   powershell -File tools\hang\dumpmem.ps1 -ProcId <pid> -OutDir build\run\hang -Gp 0x00485170
# Then tools\hang\aiscript.py decodes an AI script VM from rdram.bin.
$src = @"
using System;
using System.IO;
using System.Collections.Generic;
using System.Runtime.InteropServices;
public static class MemDump {
  [DllImport("kernel32.dll", SetLastError=true)] static extern IntPtr OpenProcess(uint access, bool inherit, int pid);
  [DllImport("kernel32.dll", SetLastError=true)] static extern bool ReadProcessMemory(IntPtr h, IntPtr addr, byte[] buf, IntPtr size, out IntPtr read);
  [DllImport("kernel32.dll", SetLastError=true)] static extern IntPtr VirtualQueryEx(IntPtr h, IntPtr addr, out MBI mbi, IntPtr len);
  [DllImport("kernel32.dll")] static extern bool CloseHandle(IntPtr h);
  [StructLayout(LayoutKind.Sequential)] public struct MBI {
    public IntPtr BaseAddress; public IntPtr AllocationBase; public uint AllocationProtect; public ushort PartitionId;
    public IntPtr RegionSize; public uint State; public uint Protect; public uint Type; }
  static byte[] Read(IntPtr h, ulong addr, int size) {
    byte[] buf = new byte[size]; IntPtr got;
    if (!ReadProcessMemory(h, (IntPtr)(long)addr, buf, (IntPtr)size, out got)) return null;
    if ((long)got != size) { byte[] b2 = new byte[(long)got]; Array.Copy(buf, b2, (long)got); return b2; }
    return buf;
  }
  static bool ZeroTail(byte[] b, int off) { for (int i = 4; i < 16; ++i) if (b[off+i] != 0) return false; return true; }
  public static void Run(int pid, string outDir, uint gp, string ctxList) {
    IntPtr h = OpenProcess(0x0010 | 0x0400, false, pid); // VM_READ | QUERY_INFORMATION
    if (h == IntPtr.Zero) throw new Exception("OpenProcess failed: " + Marshal.GetLastWin32Error());
    Directory.CreateDirectory(outDir);
    var log = new StreamWriter(Path.Combine(outDir, "regions.txt"));
    if (ctxList != "") {
      foreach (string s in ctxList.Split(',')) {
        ulong a = Convert.ToUInt64(s.Trim(), 16);
        byte[] b = Read(h, a, 4096);
        if (b != null) File.WriteAllBytes(Path.Combine(outDir, "ctx_" + a.ToString("X") + ".bin"), b);
        Console.WriteLine("ctx " + a.ToString("X") + (b == null ? " unreadable" : " dumped"));
      }
      log.Close(); CloseHandle(h); return;
    }
    // ScriptVmExec's first two words, at 0x173B00 of the guest RAM.
    byte[] sig = new byte[] { 0xA0, 0xFD, 0xBD, 0x27, 0x40, 0x00, 0xBF, 0xFF };
    ulong addr = 0; int ctxCount = 0; bool rdramDone = false; long scanned = 0;
    while (true) {
      MBI mbi; if (VirtualQueryEx(h, (IntPtr)(long)addr, out mbi, (IntPtr)Marshal.SizeOf(typeof(MBI))) == IntPtr.Zero) break;
      ulong bas = (ulong)(long)mbi.BaseAddress; ulong size = (ulong)(long)mbi.RegionSize;
      bool committed = mbi.State == 0x1000; bool priv = mbi.Type == 0x20000;
      bool readable = (mbi.Protect & 0x100) == 0 && mbi.Protect != 0x01 && mbi.Protect != 0;
      log.WriteLine(bas.ToString("X12") + " " + size.ToString("X10") + " state " + mbi.State.ToString("X") + " type " + mbi.Type.ToString("X") + " prot " + mbi.Protect.ToString("X"));
      if (committed && priv && readable) {
        if (!rdramDone && size >= 0x2000000) {
          // The game's RAM is a heap block: its start is not page aligned.
          byte[] win = Read(h, bas + 0x173B00, 0x20000);
          if (win != null) {
            for (int i = 0; i + 8 <= win.Length; ++i) {
              bool ok = true; for (int k = 0; k < 8; ++k) if (win[i+k] != sig[k]) { ok = false; break; }
              if (!ok) continue;
              ulong rdram = bas + (ulong)i;
              byte[] ram = Read(h, rdram, 0x2000000);
              if (ram != null && ram.Length == 0x2000000) {
                File.WriteAllBytes(Path.Combine(outDir, "rdram.bin"), ram);
                Console.WriteLine("rdram at " + rdram.ToString("X") + " (region " + bas.ToString("X") + " size " + size.ToString("X") + ") dumped");
                rdramDone = true;
              } else Console.WriteLine("rdram at " + rdram.ToString("X") + " read failed");
              break;
            }
          }
        }
        // Contexts: scan in 16 MB chunks with a 512-byte overlap.
        ulong off = 0;
        while (off < size) {
          int chunk = (int)Math.Min(size - off, (ulong)0x1000000);
          byte[] b = Read(h, bas + off, chunk);
          if (b == null) break;
          scanned += b.Length;
          for (int i = 0; i + 64 <= b.Length; i += 16) {
            if (BitConverter.ToUInt32(b, i) != gp || !ZeroTail(b, i)) continue;
            uint sp = BitConverter.ToUInt32(b, i + 16); uint ra = BitConverter.ToUInt32(b, i + 48);
            if (!ZeroTail(b, i + 16) || !ZeroTail(b, i + 48)) continue;
            if (sp == 0 || sp >= 0x02000000 || (sp & 0xF) != 0) continue;
            if (ra < 0x100000 || ra >= 0x600000 || (ra & 3) != 0) continue;
            ulong hit = bas + off + (ulong)i; ulong ctxBase = hit - 448;
            byte[] ctx = Read(h, ctxBase, 4096);
            if (ctx == null) continue;
            string name = "ctx_" + ctxBase.ToString("X") + ".bin";
            File.WriteAllBytes(Path.Combine(outDir, name), ctx);
            Console.WriteLine("ctx at " + ctxBase.ToString("X") + " sp " + sp.ToString("X8") + " ra " + ra.ToString("X8"));
            ++ctxCount;
          }
          if (b.Length < chunk || chunk <= 4096) break;
          off += (ulong)(chunk - 512);
        }
      }
      ulong next = bas + size; if (next <= addr) break; addr = next;
      if (addr >= 0x00007FFFFFFFFFFF) break;
    }
    log.Close(); CloseHandle(h);
    Console.WriteLine("scanned " + (scanned >> 20) + " MB, " + ctxCount + " contexts, rdram " + (rdramDone ? "found" : "NOT found"));
  }
}
"@
Add-Type -TypeDefinition $src -Language CSharp
[MemDump]::Run($ProcId, $OutDir, $Gp, $CtxList)
