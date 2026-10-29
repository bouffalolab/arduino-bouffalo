"""Pre-open the dedicated FTDI so a measured active operation can be reset promptly."""
from pathlib import Path
import subprocess
import time
from run_ra4_wifis3_tls_gate import wsl_unc_path
from run_bl616cl_generic_at_final_carrier import restore_usb
from uno_r4_usb_identity import resolve_paired_bl616cl_port
from run_ra4_wifis3_scan_final_carrier import require_final_carrier_bridge

ROOT=Path(__file__).resolve().parents[1]

class PreparedBlReset:
    def __init__(self,work):
        port=resolve_paired_bl616cl_port("auto");require_final_carrier_bridge(port)
        self.hold=work/"reset.hold";self.hold.write_text("hold")
        self.trigger=work/"reset.trigger";self.ready=work/"reset.ready"
        self.log=work/"reset.log";self.output=self.log.open("w")
        self.process=subprocess.Popen([
            "powershell.exe","-NoProfile","-ExecutionPolicy","Bypass","-File",
            wsl_unc_path(ROOT/"tools/reset_bl616cl_ftdi_windows.ps1"),
            "-HoldUntilReleasedPath",wsl_unc_path(self.hold),
            "-TriggerPath",wsl_unc_path(self.trigger),"-ReadyPath",wsl_unc_path(self.ready),
        ],stdout=self.output,stderr=subprocess.STDOUT)
        deadline=time.monotonic()+15
        try:
            while not self.ready.exists():
                if self.process.poll() is not None or time.monotonic()>deadline:
                    raise RuntimeError("dedicated FTDI reset did not prearm")
                time.sleep(.05)
        except BaseException:
            self.close()
            raise

    def fire(self):
        started=time.monotonic();self.trigger.write_text("reset")
        while "BL616CL_FTDI_FLASH_BOOT_RESET_PASS" not in self.log.read_text(encoding="utf-8", errors="replace"):
            if self.process.poll() is not None or time.monotonic()-started>2:
                raise RuntimeError("prearmed physical reset did not execute within two seconds")
            time.sleep(.01)
        print(f"BL616CL_PREARMED_RESET_EXECUTED_MS={int((time.monotonic()-started)*1000)}",flush=True)
        restore_usb(physical_reset=False)

    def close(self):
        self.hold.unlink(missing_ok=True)
        try:
            self.process.wait(timeout=8)
            if self.process.returncode:
                raise RuntimeError("prearmed FTDI helper failed")
        finally:
            self.output.close()
            print(self.log.read_text(encoding="utf-8", errors="replace"),flush=True)
