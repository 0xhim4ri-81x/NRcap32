"""
usb_device.py - Auto-detect USB device via termux-api Usb and wrap fd with libusb.

Supports two backends selected automatically at runtime:

  termux   - No-root Termux: all USB access goes through the Android USB host
             stack via termux-api Usb + TERMUX_USB_FD.  The two-process
             bootstrap dance (parent requests permission -> child inherits fd)
             is required.  Works on stock Android with Termux + Termux:API.

  root     - Running as uid 0 (Termux:Root, Nethunter terminal, etc.):
             libusb opens /dev/bus/usb directly - no termux-api binary, no
             Android permission dialog, no child process needed.

Call detect_backend() early in main() to pick the right path.
All other public helpers work the same regardless of backend.
"""

import os
import json
import ctypes
import usb.core
import usb.backend.libusb1 as libusb1

_TERMUX_API = "/data/data/com.termux/files/usr/libexec/termux-api"

# Known ESP32 native USB VID:PIDs
ESP32_KNOWN = {
    (0x303A, 0x1001): "ESP32 native USB CDC",
    (0x303A, 0x0002): "ESP32 native USB CDC",
    (0x10C4, 0xEA60): "CP2102 (ESP32 devboard)",
    (0x1A86, 0x7523): "CH340 (ESP32 devboard)",
    (0x1A86, 0x55D4): "CH9102 (ESP32 devboard)",
    (0x0403, 0x6001): "FTDI FT232 (ESP32 devboard)",
}


# Runtime environment detection

def is_root() -> bool:
    """Returns True if the current process is running as uid 0."""
    return os.getuid() == 0


def has_termux_api() -> bool:
    """Returns True if the termux-api binary is present (Termux environment)."""
    return os.path.isfile(_TERMUX_API)


def detect_backend() -> str:
    """
    Determine which USB access backend to use and return its name.

    Decision order:
      1. uid == 0              -> 'root'   (Nethunter, Termux:Root, sudo)
      2. termux-api present    -> 'termux' (stock Termux, no-root)
      3. neither               -> RuntimeError with install hint

    Returns: 'root' | 'termux'
    """
    if is_root():
        return "root"
    if has_termux_api():
        return "termux"
    raise RuntimeError(
        "No USB backend available.\n"
        "  • Run as root (Nethunter / Termux:Root / sudo), OR\n"
        "  • Install Termux:API add-on and: pkg install termux-api"
    )


# Low-level: call termux-api Usb without subprocess 

def _run_termux_api(*args, timeout: int = 15) -> str:
    """
    Fork + exec termux-api with the given args, collect stdout, return it.

    Equivalent to:
        /data/.../termux-api Usb -a <action> [params…]
    but using only os.fork / os.execve / os.pipe - no subprocess module.

    Args are everything *after* the binary path, e.g.:
        _run_termux_api("Usb", "-a", "list")
    """
    r_fd, w_fd = os.pipe()

    pid = os.fork()
    if pid == 0:
        try:
            os.close(r_fd)
            os.dup2(w_fd, 1)
            os.close(w_fd)
            devnull = os.open("/dev/null", os.O_WRONLY)
            os.dup2(devnull, 2)
            os.close(devnull)
            argv = [_TERMUX_API] + list(args)
            os.execve(_TERMUX_API, argv, os.environ.copy())
        except Exception:
            os._exit(1)
        os._exit(0)  # unreachable, but safe
    else:
        os.close(w_fd)
        chunks = []
        while True:
            chunk = os.read(r_fd, 4096)
            if not chunk:
                break
            chunks.append(chunk)
        os.close(r_fd)
        _, status = os.waitpid(pid, 0)
        exit_code = os.waitstatus_to_exitcode(status)
        output = b"".join(chunks).decode("utf-8", errors="replace").strip()
        if exit_code != 0 and not output:
            raise RuntimeError(
                f"termux-api exited {exit_code} with no output (args={args})"
            )
        return output


# Termux backend: public API

def list_usb_devices() -> list[str]:
    """
    Return a list of available USB device paths.
    Replaces: subprocess.check_output(["termux-usb", "-l"])
    Termux backend only.
    """
    out = _run_termux_api("Usb", "-a", "list")
    try:
        devices = json.loads(out)
        return devices if isinstance(devices, list) else []
    except json.JSONDecodeError as e:
        raise RuntimeError(f"termux-api Usb list returned bad JSON: {e!r}\n{out!r}")


def request_permission(device_path: str) -> bool:
    """
    Show the Android USB-permission dialog for device_path.
    Returns True if the user granted permission.
    Termux backend only.
    """
    try:
        out = _run_termux_api(
            "Usb", "-a", "permission",
            "--ez", "request", "true",
            "--es", "device", device_path,
        )
        return "yes" in out.lower() or "granted" in out.lower()
    except Exception:
        return False


def open_usb_device(device_path: str, callback_cmd: str,
                    export_as_env: bool = True) -> None:
    """
    Ask termux-api to open device_path and call callback_cmd with the fd.
    Termux backend only.
    """
    params = [
        "Usb", "-a", "open",
        "--ez", "request", "true",
        "--es", "device", device_path,
    ]
    out = _run_termux_api(*params)
    _check_api_output(out)

    fd_str = out.strip()
    env = os.environ.copy()
    if export_as_env:
        env["TERMUX_USB_FD"] = fd_str
        env["TERMUX_USB_DEVICE"] = device_path
        argv = callback_cmd.split()
        os.execvpe(argv[0], argv, env)
    else:
        argv = callback_cmd.split() + [fd_str]
        os.execvpe(argv[0], argv, env)


def _check_api_output(out: str):
    """Raise RuntimeError on known termux-api error strings."""
    lower = out.lower()
    if "no such device" in lower:
        raise RuntimeError("No such device.")
    if "no permission" in lower or "permission denied" in lower:
        raise RuntimeError("Permission denied.")
    if "permission request timeout" in lower:
        raise RuntimeError("Permission request timeout.")
    if "failed to open" in lower or "open device failed" in lower:
        raise RuntimeError("Open device failed.")


def auto_detect_device() -> str:
    """
    Return the first available USB device path (termux backend).
    Raises RuntimeError if none found.
    """
    devices = list_usb_devices()
    if not devices:
        raise RuntimeError("No USB devices found. Is the ESP32 plugged in via OTG?")
    return devices[0]


def launch_with_fd(cmd: str, device_path: str, log_file: str,
                   tail_fn=None) -> tuple:
    """
    Fork-exec termux-api Usb open, log stdout+stderr to log_file (append),
    and tail the log in a daemon thread.  Termux backend only.

    Returns (child_pid, child_done_event, tail_thread).

    The CALLER owns os.waitpid(child_pid, 0) and must call
    child_done.set() afterward so the tail thread can drain final output
    without racing on waitpid itself (which caused ChildProcessError).
    """
    import threading
    import time

    env = os.environ.copy()
    env["TERMUX_CALLBACK"] = cmd
    env["TERMUX_EXPORT_FD"] = "true"
    argv = [_TERMUX_API, "Usb", "-a", "open",
            "--ez", "request", "true", "--es", "device", device_path]

    pid = os.fork()
    if pid == 0:
        try:
            fd = os.open(log_file, os.O_WRONLY | os.O_CREAT | os.O_APPEND, 0o644)
            os.dup2(fd, 1)
            os.dup2(fd, 2)
            os.close(fd)
            os.execve(_TERMUX_API, argv, env)
        except Exception:
            os._exit(1)

    # The tail thread checks this Event instead of calling waitpid itself,
    # avoiding ChildProcessError when the parent reaps the child first.
    child_done = threading.Event()

    def _tail():
        fd = os.open(log_file, os.O_RDONLY | os.O_NONBLOCK)
        buf = b""
        try:
            while True:
                done = child_done.is_set()
                try:
                    chunk = os.read(fd, 4096)
                    buf += chunk
                except BlockingIOError:
                    pass

                while b"\n" in buf:
                    line, buf = buf.split(b"\n", 1)
                    if tail_fn:
                        tail_fn(line.decode("utf-8", errors="replace") + "\n")

                if done:
                    try:
                        buf += os.read(fd, 65536)
                    except BlockingIOError:
                        pass
                    while b"\n" in buf:
                        line, buf = buf.split(b"\n", 1)
                        if tail_fn:
                            tail_fn(line.decode("utf-8", errors="replace") + "\n")
                    if buf and tail_fn:
                        tail_fn(buf.decode("utf-8", errors="replace") + "\n")
                    break

                time.sleep(0.05)
        finally:
            os.close(fd)

    tail_thread = threading.Thread(target=_tail, daemon=True)
    tail_thread.start()

    return pid, child_done, tail_thread


def relaunch_with_fd(device_path: str, script_path: str):
    """
    Re-invoke this script so TERMUX_USB_FD gets set by termux-api Usb open.
    This replaces the current process - it does not return.
    Termux backend only.
    """
    argv = [
        _TERMUX_API, "Usb",
        "-a", "open",
        "--ez", "request", "true",
        "--es", "device", device_path,
    ]
    env = os.environ.copy()
    env["TERMUX_CALLBACK"] = f"python {script_path}"
    env["TERMUX_EXPORT_FD"] = "true"
    os.execve(_TERMUX_API, argv, env)



def find_usb_device_direct(vid_pid_filter: list[tuple] | None = None):
    """
    Root path: scan the USB bus via libusb and return the first matching device.

    vid_pid_filter: list of (vid, pid) tuples to try in order.
                    Defaults to all entries in ESP32_KNOWN if None.

    Falls back to the first non-hub device found if no known VID:PID matches.
    Raises RuntimeError if nothing is found at all.
    """
    candidates = list(vid_pid_filter or ESP32_KNOWN.keys())

    for vid, pid in candidates:
        dev = usb.core.find(idVendor=vid, idProduct=pid)
        if dev is not None:
            return dev

    all_devs = list(usb.core.find(find_all=True))
    non_hubs = [d for d in all_devs if d.bDeviceClass != 9]
    if non_hubs:
        return non_hubs[0]

    raise RuntimeError(
        "No USB device found on the bus. Is the ESP32 plugged in via OTG?"
    )


def wrap_direct(vid_pid_filter: list[tuple] | None = None):
    """
    Root backend equivalent of wrap_fd().

    Opens the ESP32 directly via libusb without termux-api or an Android
    file descriptor.  Returns a usb.core.Device ready for claim_device()
    and get_cdc_endpoints() - same contract as wrap_fd().

    Requires root (uid 0) so libusb can open /dev/bus/usb directly.
    On Nethunter, ensure libusb-1.0 is installed:
        apt install libusb-1.0-0
    """
    device = find_usb_device_direct(vid_pid_filter)
    return device



def wrap_fd(fd: int):
    """
    Wrap Android's USB file descriptor into a PyUSB Device object.
    Must call LIBUSB_OPTION_NO_DEVICE_DISCOVERY before libusb_init.
    Returns a usb.core.Device ready for use.
    Termux backend only.
    """
    backend = libusb1.get_backend()
    lib     = backend.lib
    ctx     = backend.ctx

    lib.libusb_set_option.argtypes = [libusb1.c_void_p, ctypes.c_int]
    lib.libusb_set_option.restype  = ctypes.c_int
    lib.libusb_set_option(None, 5)   # LIBUSB_OPTION_NO_DEVICE_DISCOVERY

    lib.libusb_wrap_sys_device.argtypes = [
        libusb1.c_void_p,
        ctypes.c_int,
        ctypes.POINTER(libusb1._libusb_device_handle),
    ]
    lib.libusb_get_device.argtypes = [libusb1._libusb_device_handle]
    lib.libusb_get_device.restype  = ctypes.c_void_p

    handle = libusb1._libusb_device_handle()
    ret = lib.libusb_wrap_sys_device(ctx, int(fd), ctypes.byref(handle))
    if ret != 0:
        raise RuntimeError(f"libusb_wrap_sys_device failed: {ret}")

    devid = lib.libusb_get_device(handle)

    class _Dummy:
        def __init__(self, devid, handle):
            self.devid  = devid
            self.handle = handle

    dummy  = _Dummy(devid, handle)
    device = usb.core.Device(dummy, backend)
    device._ctx.handle = dummy
    return device



def describe_device(device) -> str:
    vid, pid = device.idVendor, device.idProduct
    label = ESP32_KNOWN.get((vid, pid), "Unknown device")
    return f"{vid:04X}:{pid:04X}  {label}"


def get_cdc_endpoints(device):
    """
    Find bulk IN and OUT endpoints on the CDC Data interface.
    Returns (ep_in_addr, ep_out_addr, interface_number).
    Tries all interfaces, picks the first CDC Data (class 0x0A) one.
    Falls back to any bulk pair if none found.
    """
    import usb.util
    cfg = device.get_active_configuration()

    for intf in cfg:
        if intf.bInterfaceClass == 0x0A:
            eps = list(intf)
            ep_in  = next((e for e in eps
                           if usb.util.endpoint_direction(e.bEndpointAddress) == usb.util.ENDPOINT_IN
                           and usb.util.endpoint_type(e.bmAttributes) == usb.util.ENDPOINT_TYPE_BULK), None)
            ep_out = next((e for e in eps
                           if usb.util.endpoint_direction(e.bEndpointAddress) == usb.util.ENDPOINT_OUT
                           and usb.util.endpoint_type(e.bmAttributes) == usb.util.ENDPOINT_TYPE_BULK), None)
            if ep_in and ep_out:
                return ep_in.bEndpointAddress, ep_out.bEndpointAddress, intf.bInterfaceNumber

    # Fallback: first bulk pair found anywhere
    for intf in cfg:
        eps = list(intf)
        ep_in  = next((e for e in eps
                       if usb.util.endpoint_direction(e.bEndpointAddress) == usb.util.ENDPOINT_IN
                       and usb.util.endpoint_type(e.bmAttributes) == usb.util.ENDPOINT_TYPE_BULK), None)
        ep_out = next((e for e in eps
                       if usb.util.endpoint_direction(e.bEndpointAddress) == usb.util.ENDPOINT_OUT
                       and usb.util.endpoint_type(e.bmAttributes) == usb.util.ENDPOINT_TYPE_BULK), None)
        if ep_in and ep_out:
            return ep_in.bEndpointAddress, ep_out.bEndpointAddress, intf.bInterfaceNumber

    raise RuntimeError("No bulk endpoint pair found on device")


def claim_device(device, interface_num: int):
    """
    Detach kernel driver if needed and claim the interface.
    Works for both backends - root mode may need to detach cdc_acm.
    """
    import usb.util
    try:
        if device.is_kernel_driver_active(interface_num):
            device.detach_kernel_driver(interface_num)
    except Exception:
        pass
    device.set_configuration()
    try:
        usb.util.claim_interface(device, interface_num)
    except Exception:
        pass


def reset_endpoint_toggles(device, ep_in_addr: int, ep_out_addr: int):
    """
    Clear the DATA0/DATA1 toggle on both bulk endpoints.

    Each invocation is a fresh OS process wrapping the *same* physical USB
    device via TERMUX_USB_FD (termux) or a direct libusb open (root).
    The toggle bit lives in the peripheral's endpoint state, not in process
    memory, so a clean exit does NOT reset it.  Call this once right after
    claim_device(), before any read/write.
    """
    for ep in (ep_in_addr, ep_out_addr):
        try:
            device.clear_halt(ep)
        except usb.core.USBError:
            pass
