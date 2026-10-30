"""Exact IPv6 diagnostic echo gate on the host's sole active physical WLAN."""
import base64
import ipaddress
import re
import subprocess


def peer_script(cycles=1):
    if type(cycles) is not int or not 1 <= cycles <= 20:
        raise ValueError('IPv6 cycles must be between 1 and 20')
    source = r'''
using System;
using System.Net;
using System.Net.Sockets;
using System.Diagnostics;
public static class BlIPv6Peer {
 public static void Run(string address, long scope, int cycles) {
  var ip=IPAddress.Parse(address); ip.ScopeId=scope;
  for(int cycle=0;cycle<cycles;++cycle) {
  foreach(int size in new int[]{64,4096,65536}) {
   using(var s=new Socket(AddressFamily.InterNetworkV6,SocketType.Stream,ProtocolType.Tcp)) {
    s.SendTimeout=10000;s.ReceiveTimeout=10000;s.NoDelay=true;
    var watch=Stopwatch.StartNew();
    Console.WriteLine("IPV6_TCP_ECHO_STAGE connect bytes="+size);
    var pending=s.BeginConnect(new IPEndPoint(ip,46160),null,null);
    using(var ready=pending.AsyncWaitHandle) {
     if(!ready.WaitOne(10000)) throw new Exception("IPv6 connect deadline");
     s.EndConnect(pending);
    }
    Console.WriteLine("IPV6_TCP_ECHO_STAGE connected bytes="+size);
    byte[] sent=new byte[size], received=new byte[size];
    for(int i=0;i<size;i++) sent[i]=(byte)((i*29+size)%251);
    for(int start=0;start<size;start+=2048) {
     int end=Math.Min(size,start+2048),offset=start;
     while(offset<end) {
      int n=s.Send(sent,offset,end-offset,SocketFlags.None);
      if(n<=0) throw new Exception("IPv6 short send");offset+=n;
     }
     offset=start;
     while(offset<end) {
      int n=s.Receive(received,offset,end-offset,SocketFlags.None);
      if(n<=0) throw new Exception("IPv6 short receive");offset+=n;
     }
    }
    for(int i=0;i<size;i++) if(sent[i]!=received[i]) throw new Exception("IPv6 payload mismatch");
    s.Shutdown(SocketShutdown.Send);
    if(s.Receive(new byte[1])!=0) throw new Exception("IPv6 unexpected trailing bytes");
    Console.WriteLine("IPV6_TCP_ECHO_PASS bytes_each_direction="+size+" elapsed_ms="+watch.ElapsedMilliseconds);
   }
  }
  }
 }
}
'''
    script = "$ErrorActionPreference='Stop'\nAdd-Type -TypeDefinition @'\n" + source + "\n'@\n"
    script += "$adapters=@(Get-NetAdapter -Physical | Where-Object { $_.NdisPhysicalMedium -eq 9 -and $_.Status -eq 'Up' })\n"
    script += "if($adapters.Count -ne 1){throw 'Expected one active physical WLAN'}\n"
    script += "Write-Output 'IPV6_TCP_PEER_READY'\n"
    script += "$target=[Console]::ReadLine(); if($target -eq 'CANCEL'){exit 0}\n"
    script += "try { [BlIPv6Peer]::Run($target,[long]$adapters[0].ifIndex," + str(cycles) + ") } catch { $e=$_.Exception.GetBaseException(); Write-Output ('IPV6_TCP_ECHO_ERROR type='+$e.GetType().Name+' code='+$e.HResult); Get-NetNeighbor -AddressFamily IPv6 -InterfaceIndex $adapters[0].ifIndex -IPAddress $target -ErrorAction SilentlyContinue | ForEach-Object { Write-Output ('IPV6_TCP_ECHO_STAGE neighbor_state='+$_.State) }; exit 1 }\n"
    return script


class PreparedIPv6Peer:
    def __init__(self, cycles=1, failure_log=None):
        import select
        encoded = base64.b64encode(peer_script(cycles).encode('utf-16le')).decode('ascii')
        self.cycles = cycles
        startup_error = b''
        # WSL's PowerShell interop occasionally fails before the child emits
        # any marker (UtilAcceptVsock/accept4 110). Retry only that startup
        # transport error; application data is never retried here.
        for attempt in range(3):
            self.process = subprocess.Popen(['powershell.exe', '-NoProfile', '-NonInteractive', '-EncodedCommand', encoded],
                                            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            try:
                if not select.select([self.process.stdout], [], [], 20)[0]:
                    raise RuntimeError('Windows IPv6 peer preparation timeout')
                if self.process.stdout.readline().strip() != b'IPV6_TCP_PEER_READY':
                    raise RuntimeError('Windows IPv6 peer preparation failed')
                print('IPV6_TCP_PEER_READY', flush=True)
                return
            except Exception:
                output = self.close()
                startup_error = output[1] if isinstance(output, tuple) and len(output) > 1 else b''
                transient = b'UtilAcceptVsock' in startup_error or b'accept4 failed 110' in startup_error
                if transient and attempt < 2:
                    print('IPV6_TCP_PEER_PREPARATION_RETRY=' + str(attempt + 1), flush=True)
                    import time
                    time.sleep(0.5)
                    continue
                # Preparation has not accepted an address or started traffic.
                # Preserve compiler/adapter startup errors separately from a
                # TCP failure instead of losing stderr when the process exits.
                if failure_log is not None:
                    failure_log.write_bytes(startup_error)
                    codes = sorted(set(re.findall(rb'CS[0-9]{4}', startup_error)))
                    print('IPV6_TCP_PEER_PREPARATION_ERROR status=' + str(self.process.returncode) +
                          ' compiler_codes=' + ','.join(c.decode('ascii') for c in codes), flush=True)
                raise

    def close(self):
        if self.process.poll() is None:
            try:
                return self.process.communicate(input=b'CANCEL\n', timeout=5)
            except subprocess.TimeoutExpired:
                self.process.terminate()
                return self.process.communicate(timeout=5)
        return self.process.communicate(timeout=5)

    def exchange(self, address):
        parsed = ipaddress.IPv6Address(address)
        if not parsed.is_link_local or '%' in str(parsed):
            raise ValueError('expected an unscoped IPv6 link-local address')
        try:
            stdout, stderr = self.process.communicate(input=(str(parsed) + '\n').encode('ascii'), timeout=120)
        except subprocess.TimeoutExpired:
            self.process.terminate()
            self.process.communicate(timeout=5)
            raise RuntimeError('Windows IPv6 peer process deadline') from None
        validate_result(self.process.returncode, stdout, stderr, getattr(self, 'cycles', 1))


def validate_result(returncode, stdout, stderr, cycles=1):
    if type(cycles) is not int or not 1 <= cycles <= 20:
        raise ValueError('IPv6 cycles must be between 1 and 20')
    # PowerShell exceptions may contain the address or invocation. Emit only
    # fixed result lines; keep addresses and generated invocation in memory.
    output = stdout.decode('utf-8', errors='replace')
    for line in output.splitlines():
        if line.startswith(('IPV6_TCP_ECHO_STAGE ', 'IPV6_TCP_ECHO_ERROR ')):
            print(line.strip(), flush=True)
    markers = [line.strip() for line in output.splitlines() if line.startswith('IPV6_TCP_ECHO_PASS ')]
    for marker in markers:
        print(marker, flush=True)
    parsed = [re.fullmatch(r'IPV6_TCP_ECHO_PASS bytes_each_direction=([0-9]+) elapsed_ms=([0-9]+)', marker)
              for marker in markers]
    exact = all(parsed) and [int(match[1]) for match in parsed] == [64, 4096, 65536] * cycles
    if returncode or not exact:
        errors = sorted(set(re.findall(r'CS[0-9]{4}', stderr.decode('utf-8', errors='replace'))))
        print('IPV6_TCP_PEER_PROCESS_STATUS=' + str(returncode) + ' compiler_codes=' + ','.join(errors), flush=True)
        raise RuntimeError('Windows IPv6 TCP echo gate failed; result markers=' + str(len(markers)))
