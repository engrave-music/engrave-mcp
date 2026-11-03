"""Run a benchmark with timestamped host telemetry, without changing its timers.

Requires psutil in the measurement environment. No settings are changed. Commands
and process names are retained; process command lines and environment secrets are
never collected. Use -- before the benchmark command.
"""
import argparse
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import platform
import subprocess
import threading
import time

import psutil


def utc():
    return datetime.now(timezone.utc).isoformat()


def probe(command):
    try:
        p = subprocess.run(command, capture_output=True, text=True, encoding='utf-8', errors='replace', timeout=30)
        return dict(exit=p.returncode, stdout=p.stdout.strip(), stderr=p.stderr.strip())
    except Exception as e:
        return dict(unavailable=str(e))


def context():
    result = dict(utc=utc(), platform=platform.platform(), cpu_count=psutil.cpu_count(),
                  physical_cpu_count=psutil.cpu_count(logical=False),
                  memory_bytes=psutil.virtual_memory().total, psutil=psutil.__version__)
    battery = psutil.sensors_battery()
    result['battery'] = battery._asdict() if battery else None
    if os.name == 'nt':
        result['power_plan'] = probe(['powercfg', '/getactivescheme'])
        result['windows'] = probe(['powershell', '-NoProfile', '-Command', "@{defender=(Get-MpComputerStatus | Select-Object AntivirusEnabled,RealTimeProtectionEnabled); thermal=@(Get-CimInstance -Namespace root/wmi -ClassName MSAcpi_ThermalZoneTemperature -ErrorAction Continue | Select-Object CurrentTemperature); disks=@(Get-Partition | Where-Object DriveLetter | Select-Object DriveLetter,DiskNumber); cpu=(Get-CimInstance Win32_Processor | Select-Object Name,MaxClockSpeed)} | ConvertTo-Json -Depth 5"])
    else:
        result['loadavg'] = os.getloadavg()
        result['thermal'] = {str(p): p.read_text().strip() for p in Path('/sys/class/thermal').glob('thermal_zone*/temp')}
    result['volumes'] = []
    for disk in psutil.disk_partitions():
        try:
            result['volumes'].append(dict(device=disk.device, mount=disk.mountpoint,
                fs=disk.fstype, free=psutil.disk_usage(disk.mountpoint).free))
        except OSError:
            pass
    return result


def windows_processes():
    # psutil's fallback for protected processes queries the entire Windows
    # process table once per denied PID. Avoid that expensive observer effect.
    import ctypes
    from ctypes import wintypes as w
    k = ctypes.WinDLL('kernel32', use_last_error=True)
    k.OpenProcess.argtypes = [w.DWORD, w.BOOL, w.DWORD]
    k.OpenProcess.restype = w.HANDLE
    k.CloseHandle.argtypes = [w.HANDLE]
    k.GetProcessTimes.argtypes = [w.HANDLE] + [ctypes.POINTER(w.FILETIME)] * 4
    k.QueryFullProcessImageNameW.argtypes = [w.HANDLE, w.DWORD, w.LPWSTR, ctypes.POINTER(w.DWORD)]
    class IO(ctypes.Structure):
        _fields_ = [(n, ctypes.c_ulonglong) for n in ('read_count', 'write_count',
                    'other_count', 'read_bytes', 'write_bytes', 'other_bytes')]
    k.GetProcessIoCounters.argtypes = [w.HANDLE, ctypes.POINTER(IO)]
    rows = []
    for pid in psutil.pids():
        handle = k.OpenProcess(0x1000, False, pid)
        if not handle:
            continue
        try:
            times = [w.FILETIME() for _ in range(4)]
            if not k.GetProcessTimes(handle, *[ctypes.byref(t) for t in times]):
                continue
            name = ctypes.create_unicode_buffer(32768); size = w.DWORD(len(name))
            if not k.QueryFullProcessImageNameW(handle, 0, name, ctypes.byref(size)):
                continue
            io = IO(); have_io = k.GetProcessIoCounters(handle, ctypes.byref(io))
            rows.append(dict(pid=pid, name=Path(name.value).name,
                cpu_seconds=sum((t.dwHighDateTime << 32) + t.dwLowDateTime for t in times[2:])/1e7,
                io={n:getattr(io,n) for n,_ in io._fields_} if have_io else None))
        finally:
            k.CloseHandle(handle)
    return rows


def sample():
    started = time.monotonic()
    processes = []
    for p in (() if os.name == 'nt' else psutil.process_iter(['pid', 'name', 'cpu_times', 'io_counters'])):
        try:
            info = p.info
            cpu = info.pop('cpu_times')
            io = info.pop('io_counters')
            info['cpu_seconds'] = cpu.user + cpu.system if cpu else None
            info['io'] = io._asdict() if io else None
            processes.append(info)
        except (psutil.NoSuchProcess, psutil.AccessDenied):
            pass
    if os.name == 'nt':
        processes = windows_processes()
    freq = psutil.cpu_freq()
    disk = psutil.disk_io_counters(perdisk=True)
    return dict(utc=utc(), monotonic=time.monotonic(), cpu_percent=psutil.cpu_percent(),
                cpu_frequency=freq._asdict() if freq else None,
                available_memory=psutil.virtual_memory().available,
                disk={k:v._asdict() for k,v in disk.items()}, processes=processes,
                collection_seconds=time.monotonic()-started)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--output', type=Path, required=True)
    p.add_argument('--interval', type=float, default=2)
    p.add_argument('command', nargs=argparse.REMAINDER)
    a = p.parse_args()
    command = a.command[1:] if a.command[:1] == ['--'] else a.command
    if not command or a.interval <= 0:
        p.error('positive interval and benchmark command required')
    if Path(command[0]).is_file():
        command[0] = str(Path(command[0]).resolve())
    a.output.mkdir(parents=True, exist_ok=True)
    report = dict(command=command, before=context(), timer_boundary='unchanged child benchmark',
                  telemetry_interval_seconds=a.interval)
    report['source_head'] = probe(['git', 'rev-parse', 'HEAD'])
    report['source_status'] = probe(['git', 'status', '--short'])
    report['engine_head'] = probe(['git', '-C', 'vendor/engrave', 'rev-parse', 'HEAD'])
    report['hashes'] = {str(Path(x).resolve()):hashlib.sha256(Path(x).read_bytes()).hexdigest()
                        for x in command if Path(x).is_file()}
    stop = threading.Event()
    gaps = []
    awake = None
    if os.name == 'nt':
        import ctypes
        awake = ctypes.WinDLL('kernel32').SetThreadExecutionState
        awake.argtypes = [ctypes.c_uint32]
        awake.restype = ctypes.c_uint32
        report['idle_sleep_inhibited'] = bool(awake(0x80000001))
    with (a.output/'telemetry.jsonl').open('w', encoding='utf-8') as stream:
        stream.write(json.dumps(sample())+'\n'); stream.flush()
        def monitor():
            previous = time.monotonic()
            while not stop.wait(a.interval):
                row = sample()
                gap = row['monotonic'] - previous
                if gap > max(10, 5*a.interval):
                    gaps.append(dict(utc=row['utc'], seconds=gap))
                previous = row['monotonic']
                stream.write(json.dumps(row)+'\n'); stream.flush()
        thread = threading.Thread(target=monitor)
        thread.start()
        report['start_utc'] = utc()
        try:
            with (a.output/'benchmark.log').open('w', encoding='utf-8') as log:
                result = subprocess.run(command, stdout=log, stderr=subprocess.STDOUT)
            report['exit'] = result.returncode
        except OSError as e:
            report['launch_error'] = str(e)
            raise
        finally:
            report['end_utc'] = utc()
            stop.set(); thread.join()
            if awake:
                awake(0x80000000)
            stream.write(json.dumps(sample())+'\n')
            report['telemetry_gaps'] = gaps
            report['after'] = context()
            (a.output/'environment.json').write_text(json.dumps(report, indent=2), encoding='utf-8')
    print(json.dumps(dict(command=command, exit=result.returncode)), flush=True)
    raise SystemExit(result.returncode or (2 if gaps else 0))


if __name__ == '__main__':
    main()
