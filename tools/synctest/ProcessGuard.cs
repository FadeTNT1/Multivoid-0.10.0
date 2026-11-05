using System;
using System.Collections.Generic;
using System.ComponentModel;
using System.Diagnostics;
using System.Linq;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading;

// A private kill-on-close job owns only processes created here, including their crash handlers.
public sealed class SyncTestProcesses : IDisposable
{
    [StructLayout(LayoutKind.Sequential)] struct IO { public ulong a,b,c,d,e,f; }
    [StructLayout(LayoutKind.Sequential)] struct Basic {
        public long a,b; public uint flags; public UIntPtr min,max; public uint active;
        public UIntPtr affinity; public uint priority,scheduling;
    }
    [StructLayout(LayoutKind.Sequential)] struct Limits {
        public Basic basic; public IO io; public UIntPtr process,job,peakProcess,peakJob;
    }
    [StructLayout(LayoutKind.Sequential, CharSet=CharSet.Unicode)] struct Startup {
        public int size; public string reserved,desktop,title; public int x,y,w,h,cx,cy,fill;
        public uint flags; public short show,bytes; public IntPtr data,input,output,error;
    }
    [StructLayout(LayoutKind.Sequential)] struct Info { public IntPtr process,thread; public int pid,tid; }
    delegate bool WindowCallback(IntPtr window, IntPtr arg);
    [StructLayout(LayoutKind.Sequential)] struct Rect { public int left,top,right,bottom; }
    [DllImport("user32.dll")] static extern bool GetClipCursor(out Rect r);
    [DllImport("user32.dll")] static extern int GetSystemMetrics(int n);
    [DllImport("user32.dll")] static extern IntPtr SetThreadDpiAwarenessContext(IntPtr c);
    [DllImport("user32.dll")] static extern IntPtr GetForegroundWindow();
    [DllImport("kernel32.dll",CharSet=CharSet.Unicode,SetLastError=true)] static extern IntPtr CreateJobObject(IntPtr a,string n);
    [DllImport("kernel32.dll",SetLastError=true)] static extern bool SetInformationJobObject(IntPtr j,int c,ref Limits l,uint n);
    [DllImport("kernel32.dll",SetLastError=true)] static extern bool AssignProcessToJobObject(IntPtr j,IntPtr p);
    [DllImport("kernel32.dll",CharSet=CharSet.Unicode,SetLastError=true)] static extern bool CreateProcess(string a,StringBuilder c,IntPtr p,IntPtr t,bool inherit,uint f,IntPtr env,string cwd,ref Startup s,out Info i);
    [DllImport("kernel32.dll",SetLastError=true)] static extern bool SetProcessAffinityMask(IntPtr p,UIntPtr m);
    [DllImport("kernel32.dll",SetLastError=true)] static extern bool GetProcessAffinityMask(IntPtr p,out UIntPtr m,out UIntPtr s);
    [DllImport("kernel32.dll")] static extern uint ResumeThread(IntPtr t);
    [DllImport("kernel32.dll")] static extern bool TerminateProcess(IntPtr p,uint c);
    [DllImport("kernel32.dll")] static extern bool TerminateJobObject(IntPtr j,uint c);
    [DllImport("kernel32.dll")] static extern bool CloseHandle(IntPtr h);
    [DllImport("user32.dll")] static extern bool EnumWindows(WindowCallback c,IntPtr a);
    [DllImport("user32.dll")] static extern uint GetWindowThreadProcessId(IntPtr w,out int p);
    [DllImport("user32.dll")] static extern bool ShowWindowAsync(IntPtr w,int s);
    [DllImport("user32.dll")] static extern bool SetWindowPos(IntPtr w,IntPtr after,int x,int y,int cx,int cy,uint f);
    readonly IntPtr job;
    readonly List<Process> peers = new List<Process>();
    readonly Timer timer;
    public string Failure { get; private set; }
    public static string CursorStatus() {
        var previous=SetThreadDpiAwarenessContext(new IntPtr(-4));
        try {
            Rect r;
            int x=GetSystemMetrics(76),y=GetSystemMetrics(77),w=GetSystemMetrics(78),h=GetSystemMetrics(79);
            if(!GetClipCursor(out r)) return "FAIL GetClipCursor";
            bool ok=r.left==x && r.top==y && r.right==x+w && r.bottom==y+h;
            return (ok?"PASS":"FAIL")+" clip="+r.left+","+r.top+","+r.right+","+r.bottom+" desktop="+x+","+y+","+(x+w)+","+(y+h);
        } finally { if(previous!=IntPtr.Zero) SetThreadDpiAwarenessContext(previous); }
    }
    public SyncTestProcesses() {
        job=CreateJobObject(IntPtr.Zero,null);
        if(job==IntPtr.Zero) throw new Win32Exception();
        var limits=new Limits(); limits.basic.flags=0x2000; // JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE
        if(!SetInformationJobObject(job,9,ref limits,(uint)Marshal.SizeOf(typeof(Limits)))) {
            CloseHandle(job); throw new Win32Exception();
        }
        timer=new Timer(Check,null,100,100);
    }
    public int Start(string exe,string arguments,IDictionary<string,string> environment) {
        string cursor=CursorStatus();
        if(!cursor.StartsWith("PASS")) throw new InvalidOperationException("Desktop mouse is not free before launch: "+cursor);
        var block=string.Join("\0",environment.OrderBy(p=>p.Key,StringComparer.OrdinalIgnoreCase).Select(p=>p.Key+"="+p.Value))+"\0\0";
        IntPtr env=Marshal.StringToHGlobalUni(block);
        Info info;
        var startup=new Startup(); startup.size=Marshal.SizeOf(typeof(Startup)); startup.flags=1; startup.show=7;
        try {
            // Suspended: affinity, low priority, and ownership are in place before game code executes.
            if(!CreateProcess(exe,new StringBuilder("\""+exe+"\" "+arguments),IntPtr.Zero,IntPtr.Zero,false,
                0x00000004|0x00000400|0x08000000|0x00004000,env,System.IO.Path.GetDirectoryName(exe),ref startup,out info))
                throw new Win32Exception();
        } finally { Marshal.FreeHGlobal(env); }
        try {
            UIntPtr allowed,system;
            if(!GetProcessAffinityMask(Process.GetCurrentProcess().Handle,out allowed,out system)) throw new Win32Exception();
            ulong mask=0,available=allowed.ToUInt64(); int count=0;
            for(int i=0;i<64 && count<4;i++) if((available&(1UL<<i))!=0) { mask|=1UL<<i; count++; }
            if(mask==0 || !SetProcessAffinityMask(info.process,new UIntPtr(mask))) throw new Win32Exception();
            if(!AssignProcessToJobObject(job,info.process)) throw new Win32Exception();
            lock(peers) peers.Add(Process.GetProcessById(info.pid));
            if(ResumeThread(info.thread)==uint.MaxValue) throw new Win32Exception();
            return info.pid;
        } catch { TerminateProcess(info.process,1); throw; }
        finally { CloseHandle(info.thread); CloseHandle(info.process); }
    }
    void Check(object unused) {
        lock(peers) {
            try {
                if(peers.Count>0 && !CursorStatus().StartsWith("PASS")) {
                    Failure="Desktop cursor became clipped: "+CursorStatus();
                    TerminateJobObject(job,1); return;
                }
                int foregroundPid; var foreground=GetForegroundWindow();
                GetWindowThreadProcessId(foreground,out foregroundPid);
                if(peers.Any(p=>p.Id==foregroundPid)) {
                    ShowWindowAsync(foreground,7);
                    Failure="Rig window became foreground: PID "+foregroundPid;
                    TerminateJobObject(job,1); return;
                }
                foreach(var peer in peers) {
                    if(peer.HasExited) continue;
                    peer.Refresh();
                    if(peer.WorkingSet64>6L*1024*1024*1024) {
                        Failure="Working set exceeded 6 GiB: PID "+peer.Id;
                        TerminateJobObject(job,1); return;
                    }
                }
                EnumWindows((window,arg)=> {
                    int pid; GetWindowThreadProcessId(window,out pid);
                    if(peers.Any(p=>p.Id==pid)) {
                        SetWindowPos(window,IntPtr.Zero,-32000,-32000,640,360,0x0010|0x0004);
                        ShowWindowAsync(window,7); // SW_SHOWMINNOACTIVE
                    }
                    return true;
                },IntPtr.Zero);
            } catch(Exception e) { Failure=e.Message; TerminateJobObject(job,1); }
        }
    }
    public void Dispose() {
        timer.Dispose();
        lock(peers) {
            TerminateJobObject(job,0);
            foreach(var peer in peers) { try { peer.WaitForExit(5000); } catch {} peer.Dispose(); }
            peers.Clear(); CloseHandle(job);
        }
    }
}
