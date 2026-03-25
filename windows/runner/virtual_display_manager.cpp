#include "virtual_display_manager.h"
#include <setupapi.h>
#include <devguid.h>
#include <fstream>
#include <sstream>
#include <thread>
#include <chrono>
#include <iostream>
#include <dxgi.h>

#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "dxgi.lib")

// VDD identifies itself with this string in the device/adapter name
static const wchar_t* VDD_ADAPTER_KEYWORD = L"Virtual Display";
static const wchar_t* VDD_DEVICE_KEYWORD = L"VDD";

// Settings XML location used by VDD
static const wchar_t* VDD_SETTINGS_DIR = L"C:\\VirtualDisplayDriver";
static const wchar_t* VDD_SETTINGS_FILE = L"C:\\VirtualDisplayDriver\\vdd_settings.xml";

// Named pipe used by VDD driver for IPC
static const wchar_t* VDD_PIPE_NAME = L"\\\\.\\pipe\\MTTVirtualDisplayPipe";

// --- Public Methods ---

bool VirtualDisplayManager::IsDriverInstalled() {
    // Method 1: Check if VDD adapter exists via EnumDisplayDevices
    DISPLAY_DEVICEW dd = {};
    dd.cb = sizeof(dd);
    for (DWORD i = 0; EnumDisplayDevicesW(nullptr, i, &dd, 0); i++) {
        std::wstring name(dd.DeviceString);
        if (name.find(L"Virtual Display") != std::wstring::npos ||
            name.find(L"IddSampleDriver") != std::wstring::npos ||
            name.find(L"MTT") != std::wstring::npos ||
            name.find(L"VDD") != std::wstring::npos) {
            return true;
        }
        dd.cb = sizeof(dd);
    }

    // Method 2: Check registry
    HKEY hKey;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\VirtualDisplayDriver",
                      0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        RegCloseKey(hKey);
        return true;
    }

    return false;
}

int VirtualDisplayManager::CreateVirtualMonitor(int width, int height) {
    if (!IsDriverInstalled()) {
        std::cerr << "[VDM] VDD driver not installed" << std::endl;
        return -1;
    }

    // Check if a VDD monitor is already active
    int idx = GetVirtualMonitorIndex();
    if (idx >= 0) {
        std::cout << "[VDM] Virtual monitor already active at index " << idx << std::endl;
        return idx;
    }

    std::cout << "[VDM] Creating virtual monitor " << width << "x" << height << "..." << std::endl;

    // Ensure settings directory exists
    CreateDirectoryW(VDD_SETTINGS_DIR, nullptr);

    // Step 1: Write UTF-8 XML with the desired monitor config
    if (!WriteSettingsXml(width, height)) {
        std::cerr << "[VDM] Failed to write settings XML" << std::endl;
        return -1;
    }

    // Step 2: Try pipe command first
    SendPipeCommand(L"SETDISPLAYCOUNT 1");

    // Poll for 3 seconds
    std::cout << "[VDM] Waiting for virtual monitor to appear..." << std::endl;
    for (int attempt = 0; attempt < 15; attempt++) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        idx = GetVirtualMonitorIndex();
        if (idx >= 0) {
            std::cout << "[VDM] Virtual monitor created via pipe at index " << idx << std::endl;
            return idx;
        }
    }

    // Step 3: Pipe didn't work — restart the driver so it reads the XML on startup.
    // This requires admin (Disable/Enable-PnpDevice), triggers UAC.
    std::cout << "[VDM] Pipe didn't work, restarting driver to apply config..." << std::endl;
    {
        std::wstring psCmd =
            L"-NoProfile -ExecutionPolicy Bypass -Command \""
            L"$dev = Get-PnpDevice -ErrorAction SilentlyContinue | Where-Object { $_.HardwareID -contains 'ROOT\\MttVDD' }; "
            L"if ($dev) { "
            L"  Disable-PnpDevice -InstanceId $dev.InstanceId -Confirm:$false -ErrorAction SilentlyContinue; "
            L"  Start-Sleep -Seconds 1; "
            L"  Enable-PnpDevice -InstanceId $dev.InstanceId -Confirm:$false "
            L"}\"";

        SHELLEXECUTEINFOW sei = {};
        sei.cbSize = sizeof(sei);
        sei.fMask = SEE_MASK_NOCLOSEPROCESS;
        sei.lpVerb = L"runas";
        sei.lpFile = L"powershell.exe";
        sei.lpParameters = psCmd.c_str();
        sei.nShow = SW_HIDE;

        if (ShellExecuteExW(&sei) && sei.hProcess) {
            WaitForSingleObject(sei.hProcess, 30000);
            CloseHandle(sei.hProcess);
        }
    }

    // After driver restart, it should read the XML and create the display.
    // Also try pipe command again now that driver is freshly started.
    std::this_thread::sleep_for(std::chrono::milliseconds(2000));
    SendPipeCommand(L"SETDISPLAYCOUNT 1");

    // Poll for up to 5 seconds
    for (int attempt = 0; attempt < 25; attempt++) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        idx = GetVirtualMonitorIndex();
        if (idx >= 0) {
            std::cout << "[VDM] Virtual monitor created after driver restart at index " << idx << std::endl;
            return idx;
        }
    }

    std::cerr << "[VDM] Failed to create virtual monitor" << std::endl;
    return -1;
}

bool VirtualDisplayManager::RemoveVirtualMonitor() {
    std::cout << "[VDM] Removing virtual monitor..." << std::endl;

    // Write empty XML so driver won't recreate monitors on next start
    WriteEmptySettingsXml();

    // Tell driver to remove all virtual displays — just this one command, nothing aggressive
    // Don't touch registry or reload driver as that can break the driver (yellow triangle)
    SendPipeCommand(L"SETDISPLAYCOUNT 0");
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    if (GetVirtualMonitorIndex() < 0) {
        std::cout << "[VDM] Virtual monitor removed successfully" << std::endl;
    } else {
        std::cout << "[VDM] Virtual monitor may still be active" << std::endl;
    }

    return true;
}

int VirtualDisplayManager::GetVirtualMonitorIndex() {
    // Enumerate display devices to find the VDD monitor
    DISPLAY_DEVICEW adapter = {};
    adapter.cb = sizeof(adapter);

    for (DWORD adapterIdx = 0; EnumDisplayDevicesW(nullptr, adapterIdx, &adapter, 0); adapterIdx++) {
        std::wstring adapterName(adapter.DeviceString);
        bool isVdd = (adapterName.find(L"Virtual Display") != std::wstring::npos ||
                      adapterName.find(L"IddSampleDriver") != std::wstring::npos ||
                      adapterName.find(L"MTT") != std::wstring::npos ||
                      adapterName.find(L"VDD") != std::wstring::npos);

        if (isVdd && (adapter.StateFlags & DISPLAY_DEVICE_ACTIVE)) {
            // Found active VDD adapter — now find its monitor index
            // The DXGI output index matches the order from EnumDisplayMonitors
            // We need to match by device name
            std::wstring deviceName(adapter.DeviceName);

            // Get the DEVMODE to find the monitor position
            DEVMODEW dm = {};
            dm.dmSize = sizeof(dm);
            if (EnumDisplaySettingsW(deviceName.c_str(), ENUM_CURRENT_SETTINGS, &dm)) {
                // Find which monitor index has these coordinates
                struct MonitorSearch {
                    POINT pos;
                    int index;
                    int currentIndex;
                };
                MonitorSearch search = {};
                search.pos = { (LONG)dm.dmPosition.x, (LONG)dm.dmPosition.y };
                search.index = -1;
                search.currentIndex = 0;

                EnumDisplayMonitors(nullptr, nullptr,
                    [](HMONITOR, HDC, LPRECT rect, LPARAM lParam) -> BOOL {
                        auto* s = reinterpret_cast<MonitorSearch*>(lParam);
                        if (rect->left == s->pos.x && rect->top == s->pos.y) {
                            s->index = s->currentIndex;
                            return FALSE; // stop enumerating
                        }
                        s->currentIndex++;
                        return TRUE;
                    },
                    reinterpret_cast<LPARAM>(&search));

                if (search.index >= 0) {
                    return search.index;
                }
            }
        }
        adapter.cb = sizeof(adapter);
    }

    return -1;
}

bool VirtualDisplayManager::MoveWindowToMonitor(HWND hwnd, int monitorIndex) {
    if (!IsWindow(hwnd)) return false;

    RECT bounds = GetMonitorBounds(monitorIndex);
    if (bounds.left == 0 && bounds.top == 0 && bounds.right == 0 && bounds.bottom == 0) {
        return false;
    }

    int width = bounds.right - bounds.left;
    int height = bounds.bottom - bounds.top;

    // Remove maximized state first if present
    ShowWindow(hwnd, SW_RESTORE);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // Move and resize window to fill the virtual display
    SetWindowPos(hwnd, HWND_TOP,
                 bounds.left, bounds.top, width, height,
                 SWP_SHOWWINDOW | SWP_NOZORDER);

    // Maximize on the target monitor
    ShowWindow(hwnd, SW_MAXIMIZE);

    return true;
}

RECT VirtualDisplayManager::GetMonitorBounds(int monitorIndex) {
    RECT result = {0, 0, 0, 0};

    struct MonitorEnum {
        int targetIndex;
        int currentIndex;
        RECT bounds;
    };
    MonitorEnum data = {};
    data.targetIndex = monitorIndex;
    data.currentIndex = 0;
    data.bounds = {0, 0, 0, 0};

    EnumDisplayMonitors(nullptr, nullptr,
        [](HMONITOR hMon, HDC, LPRECT rect, LPARAM lParam) -> BOOL {
            auto* d = reinterpret_cast<MonitorEnum*>(lParam);
            if (d->currentIndex == d->targetIndex) {
                d->bounds = *rect;
                return FALSE;
            }
            d->currentIndex++;
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&data));

    return data.bounds;
}

bool VirtualDisplayManager::InstallBundledDriver() {
    std::cout << "[VDM] Installing bundled VDD driver..." << std::endl;

    // Find the bundled driver files next to the executable
    wchar_t exePath[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    std::wstring exeDir(exePath);
    exeDir = exeDir.substr(0, exeDir.find_last_of(L"\\/") + 1);
    std::wstring infPath = exeDir + L"vdd_driver\\MttVDD.inf";

    if (GetFileAttributesW(infPath.c_str()) == INVALID_FILE_ATTRIBUTES) {
        std::cerr << "[VDM] Bundled driver not found at: ";
        std::wcerr << infPath << std::endl;
        return false;
    }
    std::wcout << L"[VDM] Using bundled .inf: " << infPath << std::endl;

    // All driver installation requires admin privileges (SetupDi, pnputil, etc.)
    // So we run an elevated PowerShell script that does everything:
    // 1. pnputil /add-driver to add to driver store
    // 2. Create device node via .NET SetupAPI P/Invoke if needed
    // 3. Enable device if it already exists

    // Write a temporary PS1 script that handles all cases
    wchar_t tempDir[MAX_PATH] = {};
    GetTempPathW(MAX_PATH, tempDir);
    std::wstring scriptPath = std::wstring(tempDir) + L"kaptchi_install_vdd.ps1";

    {
        std::ofstream script(scriptPath);
        if (!script.is_open()) {
            std::cerr << "[VDM] Failed to create install script" << std::endl;
            return false;
        }

        // Convert infPath to narrow (ASCII) string for the script
        int len = WideCharToMultiByte(CP_ACP, 0, infPath.c_str(), -1, nullptr, 0, nullptr, nullptr);
        std::string infPathNarrow(len - 1, '\0');
        WideCharToMultiByte(CP_ACP, 0, infPath.c_str(), -1, &infPathNarrow[0], len, nullptr, nullptr);

        script << "$ErrorActionPreference = 'Stop'\n";
        script << "$infPath = '" << infPathNarrow << "'\n";
        script << "\n";
        script << "# Step 1: Add driver to store\n";
        script << "Write-Host 'Adding driver to store...'\n";
        script << "pnputil /add-driver $infPath /install\n";
        script << "\n";
        script << "# Step 2: Check if device already exists\n";
        script << "$dev = Get-PnpDevice -ErrorAction SilentlyContinue | Where-Object { $_.HardwareID -contains 'ROOT\\MttVDD' }\n";
        script << "if ($dev) {\n";
        script << "    Write-Host 'Device exists, enabling...'\n";
        script << "    Enable-PnpDevice -InstanceId $dev.InstanceId -Confirm:$false -ErrorAction SilentlyContinue\n";
        script << "    # Restart the device to pick up updated driver\n";
        script << "    Disable-PnpDevice -InstanceId $dev.InstanceId -Confirm:$false -ErrorAction SilentlyContinue\n";
        script << "    Start-Sleep -Seconds 1\n";
        script << "    Enable-PnpDevice -InstanceId $dev.InstanceId -Confirm:$false\n";
        script << "} else {\n";
        script << "    Write-Host 'Creating device node...'\n";
        script << "    # Use SetupAPI via P/Invoke to create root-enumerated device\n";
        script << "    Add-Type @'\n";
        script << "using System;\n";
        script << "using System.Runtime.InteropServices;\n";
        script << "public class DeviceInstaller {\n";
        script << "    [DllImport(\"setupapi.dll\", CharSet=CharSet.Unicode, SetLastError=true)]\n";
        script << "    static extern IntPtr SetupDiCreateDeviceInfoList(ref Guid ClassGuid, IntPtr hwndParent);\n";
        script << "    [DllImport(\"setupapi.dll\", CharSet=CharSet.Unicode, SetLastError=true)]\n";
        script << "    static extern bool SetupDiCreateDeviceInfoW(IntPtr DeviceInfoSet, string DeviceName, ref Guid ClassGuid, string DeviceDescription, IntPtr hwndParent, uint CreationFlags, ref SP_DEVINFO_DATA DeviceInfoData);\n";
        script << "    [DllImport(\"setupapi.dll\", CharSet=CharSet.Unicode, SetLastError=true)]\n";
        script << "    static extern bool SetupDiSetDeviceRegistryPropertyW(IntPtr DeviceInfoSet, ref SP_DEVINFO_DATA DeviceInfoData, uint Property, byte[] PropertyBuffer, uint PropertyBufferSize);\n";
        script << "    [DllImport(\"setupapi.dll\", CharSet=CharSet.Unicode, SetLastError=true)]\n";
        script << "    static extern bool SetupDiCallClassInstaller(uint InstallFunction, IntPtr DeviceInfoSet, ref SP_DEVINFO_DATA DeviceInfoData);\n";
        script << "    [DllImport(\"setupapi.dll\", SetLastError=true)]\n";
        script << "    static extern bool SetupDiDestroyDeviceInfoList(IntPtr DeviceInfoSet);\n";
        script << "    [DllImport(\"newdev.dll\", CharSet=CharSet.Unicode, SetLastError=true)]\n";
        script << "    static extern bool UpdateDriverForPlugAndPlayDevicesW(IntPtr hwndParent, string HardwareId, string FullInfPath, uint InstallFlags, out bool RebootRequired);\n";
        script << "    [StructLayout(LayoutKind.Sequential)]\n";
        script << "    public struct SP_DEVINFO_DATA {\n";
        script << "        public uint cbSize;\n";
        script << "        public Guid ClassGuid;\n";
        script << "        public uint DevInst;\n";
        script << "        public IntPtr Reserved;\n";
        script << "    }\n";
        script << "    const uint DICD_GENERATE_ID = 1;\n";
        script << "    const uint DIF_REGISTERDEVICE = 0x19;\n";
        script << "    const uint SPDRP_HARDWAREID = 1;\n";
        script << "    const uint INSTALLFLAG_FORCE = 1;\n";
        script << "    static readonly Guid GUID_DISPLAY = new Guid(\"{4d36e968-e325-11ce-bfc1-08002be10318}\");\n";
        script << "    public static bool Install(string infPath) {\n";
        script << "        Guid classGuid = GUID_DISPLAY;\n";
        script << "        IntPtr devs = SetupDiCreateDeviceInfoList(ref classGuid, IntPtr.Zero);\n";
        script << "        if (devs == (IntPtr)(-1)) return false;\n";
        script << "        SP_DEVINFO_DATA devInfo = new SP_DEVINFO_DATA();\n";
        script << "        devInfo.cbSize = (uint)Marshal.SizeOf(devInfo);\n";
        script << "        if (!SetupDiCreateDeviceInfoW(devs, \"MttVDD\", ref classGuid, null, IntPtr.Zero, DICD_GENERATE_ID, ref devInfo)) {\n";
        script << "            SetupDiDestroyDeviceInfoList(devs); return false;\n";
        script << "        }\n";
        script << "        string hwid = \"ROOT\\\\MttVDD\\0\";\n";
        script << "        byte[] hwidBytes = System.Text.Encoding.Unicode.GetBytes(hwid + \"\\0\");\n";
        script << "        if (!SetupDiSetDeviceRegistryPropertyW(devs, ref devInfo, SPDRP_HARDWAREID, hwidBytes, (uint)hwidBytes.Length)) {\n";
        script << "            SetupDiDestroyDeviceInfoList(devs); return false;\n";
        script << "        }\n";
        script << "        if (!SetupDiCallClassInstaller(DIF_REGISTERDEVICE, devs, ref devInfo)) {\n";
        script << "            SetupDiDestroyDeviceInfoList(devs); return false;\n";
        script << "        }\n";
        script << "        SetupDiDestroyDeviceInfoList(devs);\n";
        script << "        bool reboot;\n";
        script << "        UpdateDriverForPlugAndPlayDevicesW(IntPtr.Zero, \"ROOT\\\\MttVDD\", infPath, INSTALLFLAG_FORCE, out reboot);\n";
        script << "        return true;\n";
        script << "    }\n";
        script << "}\n";
        script << "'@\n";
        script << "    $result = [DeviceInstaller]::Install($infPath)\n";
        script << "    Write-Host \"Device creation result: $result\"\n";
        script << "}\n";
        script << "Write-Host 'Done'\n";

        script.close();
    }

    std::cout << "[VDM] Running elevated install script..." << std::endl;

    // Run the script elevated (triggers UAC)
    std::wstring psArgs = L"-NoProfile -ExecutionPolicy Bypass -File \"" + scriptPath + L"\"";

    SHELLEXECUTEINFOW sei = {};
    sei.cbSize = sizeof(sei);
    sei.fMask = SEE_MASK_NOCLOSEPROCESS;
    sei.lpVerb = L"runas";
    sei.lpFile = L"powershell.exe";
    sei.lpParameters = psArgs.c_str();
    sei.nShow = SW_HIDE;

    if (!ShellExecuteExW(&sei)) {
        DWORD err = GetLastError();
        std::cerr << "[VDM] Failed to launch elevated installer: " << err << std::endl;
        DeleteFileW(scriptPath.c_str());
        return false;
    }

    if (sei.hProcess) {
        WaitForSingleObject(sei.hProcess, 120000); // 2 min timeout
        DWORD exitCode = 0;
        GetExitCodeProcess(sei.hProcess, &exitCode);
        CloseHandle(sei.hProcess);
        std::cout << "[VDM] Install script exit code: " << exitCode << std::endl;
    }

    DeleteFileW(scriptPath.c_str());

    // Wait for the driver to initialize
    std::this_thread::sleep_for(std::chrono::milliseconds(3000));

    bool installed = IsDriverInstalled();
    std::cout << "[VDM] Driver installed: " << (installed ? "YES" : "NO") << std::endl;
    return installed;
}

bool VirtualDisplayManager::UninstallDriver(const wchar_t* /* unused */) {
    // Use pnputil to delete the driver (no need for devcon)
    // First, find the OEM inf name for the VDD driver
    std::wstring psFindOem =
        L"-NoProfile -ExecutionPolicy Bypass -Command \""
        L"$d = pnputil /enum-drivers | Select-String -Pattern 'MttVDD' -Context 5; "
        L"if ($d) { ($d.Context.PreContext | Select-String 'oem\\d+\\.inf').Matches.Value }\"";

    std::wstring oemOutputFile;
    {
        wchar_t tempDir[MAX_PATH] = {};
        GetTempPathW(MAX_PATH, tempDir);
        oemOutputFile = std::wstring(tempDir) + L"kaptchi_vdd_oem.txt";
    }

    std::wstring psFindOemRedirect =
        L"-NoProfile -ExecutionPolicy Bypass -Command \""
        L"$lines = pnputil /enum-drivers; "
        L"for ($i = 0; $i -lt $lines.Count; $i++) { "
        L"  if ($lines[$i] -match 'MttVDD') { "
        L"    for ($j = [Math]::Max(0,$i-10); $j -le $i; $j++) { "
        L"      if ($lines[$j] -match '(oem\\d+\\.inf)') { "
        L"        $matches[1] | Out-File -FilePath '" + oemOutputFile + L"' -Encoding ASCII -NoNewline; break "
        L"      } } break } }\"";

    SHELLEXECUTEINFOW sei = {};
    sei.cbSize = sizeof(sei);
    sei.fMask = SEE_MASK_NOCLOSEPROCESS;
    sei.lpFile = L"powershell.exe";
    sei.lpParameters = psFindOemRedirect.c_str();
    sei.nShow = SW_HIDE;

    if (ShellExecuteExW(&sei) && sei.hProcess) {
        WaitForSingleObject(sei.hProcess, 30000);
        CloseHandle(sei.hProcess);
    }

    std::string oemInf;
    std::ifstream oemFile(oemOutputFile);
    if (oemFile.is_open()) {
        std::getline(oemFile, oemInf);
        oemFile.close();
        while (!oemInf.empty() && (oemInf.back() == '\r' || oemInf.back() == '\n' || oemInf.back() == ' '))
            oemInf.pop_back();
    }
    DeleteFileW(oemOutputFile.c_str());

    if (oemInf.empty()) {
        std::cerr << "[VDM] Could not find VDD OEM driver package" << std::endl;
        return false;
    }

    std::wstring oemInfW(oemInf.begin(), oemInf.end());
    std::wcout << L"[VDM] Found VDD driver: " << oemInfW << std::endl;

    // Delete the driver with pnputil (requires admin)
    std::wstring pnpArgs = L"/delete-driver " + oemInfW + L" /uninstall /force";

    memset(&sei, 0, sizeof(sei));
    sei.cbSize = sizeof(sei);
    sei.fMask = SEE_MASK_NOCLOSEPROCESS;
    sei.lpVerb = L"runas";
    sei.lpFile = L"pnputil.exe";
    sei.lpParameters = pnpArgs.c_str();
    sei.nShow = SW_HIDE;

    if (!ShellExecuteExW(&sei)) {
        std::cerr << "[VDM] Failed to launch pnputil for uninstall: " << GetLastError() << std::endl;
        return false;
    }
    if (sei.hProcess) {
        WaitForSingleObject(sei.hProcess, 60000);
        DWORD exitCode = 0;
        GetExitCodeProcess(sei.hProcess, &exitCode);
        CloseHandle(sei.hProcess);
        std::cout << "[VDM] pnputil uninstall exit code: " << exitCode << std::endl;
    }

    std::cout << "[VDM] VDD driver uninstalled" << std::endl;
    return true;
}

bool VirtualDisplayManager::SendClick(float normalizedX, float normalizedY, int clickType) {
    int vdIdx = GetVirtualMonitorIndex();
    if (vdIdx < 0) return false;

    RECT bounds = GetMonitorBounds(vdIdx);
    int monW = bounds.right - bounds.left;
    int monH = bounds.bottom - bounds.top;
    if (monW <= 0 || monH <= 0) return false;

    // Save current cursor position so we can restore it after
    POINT savedCursor;
    GetCursorPos(&savedCursor);

    // Convert normalized (0-1) to screen coordinates
    int screenX = bounds.left + static_cast<int>(normalizedX * monW);
    int screenY = bounds.top + static_cast<int>(normalizedY * monH);

    // Convert to absolute coordinates for SendInput (0-65535 range across all monitors)
    int virtualW = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    int virtualH = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    int virtualLeft = GetSystemMetrics(SM_XVIRTUALSCREEN);
    int virtualTop = GetSystemMetrics(SM_YVIRTUALSCREEN);

    DWORD absX = static_cast<DWORD>(((screenX - virtualLeft) * 65535) / virtualW);
    DWORD absY = static_cast<DWORD>(((screenY - virtualTop) * 65535) / virtualH);

    INPUT inputs[2] = {};
    int inputCount = 0;

    // Move mouse to position
    inputs[0].type = INPUT_MOUSE;
    inputs[0].mi.dx = absX;
    inputs[0].mi.dy = absY;
    inputs[0].mi.dwFlags = MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK | MOUSEEVENTF_MOVE;
    inputCount = 1;

    switch (clickType) {
        case 0: // Left click (down + up)
            inputs[0].mi.dwFlags |= MOUSEEVENTF_LEFTDOWN;
            inputs[1].type = INPUT_MOUSE;
            inputs[1].mi.dx = absX;
            inputs[1].mi.dy = absY;
            inputs[1].mi.dwFlags = MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK | MOUSEEVENTF_LEFTUP;
            inputCount = 2;
            break;
        case 1: // Right click (down + up)
            inputs[0].mi.dwFlags |= MOUSEEVENTF_RIGHTDOWN;
            inputs[1].type = INPUT_MOUSE;
            inputs[1].mi.dx = absX;
            inputs[1].mi.dy = absY;
            inputs[1].mi.dwFlags = MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK | MOUSEEVENTF_RIGHTUP;
            inputCount = 2;
            break;
        case 2: // Left down only
            inputs[0].mi.dwFlags |= MOUSEEVENTF_LEFTDOWN;
            break;
        case 3: // Left up only
            inputs[0].mi.dwFlags |= MOUSEEVENTF_LEFTUP;
            break;
    }

    UINT sent = SendInput(inputCount, inputs, sizeof(INPUT));

    // Restore cursor to original position
    SetCursorPos(savedCursor.x, savedCursor.y);

    return sent == static_cast<UINT>(inputCount);
}

bool VirtualDisplayManager::SendScroll(float normalizedX, float normalizedY, int deltaY) {
    int vdIdx = GetVirtualMonitorIndex();
    if (vdIdx < 0) return false;

    RECT bounds = GetMonitorBounds(vdIdx);
    int monW = bounds.right - bounds.left;
    int monH = bounds.bottom - bounds.top;
    if (monW <= 0 || monH <= 0) return false;

    // Save current cursor position
    POINT savedCursor;
    GetCursorPos(&savedCursor);

    int screenX = bounds.left + static_cast<int>(normalizedX * monW);
    int screenY = bounds.top + static_cast<int>(normalizedY * monH);

    int virtualW = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    int virtualH = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    int virtualLeft = GetSystemMetrics(SM_XVIRTUALSCREEN);
    int virtualTop = GetSystemMetrics(SM_YVIRTUALSCREEN);

    DWORD absX = static_cast<DWORD>(((screenX - virtualLeft) * 65535) / virtualW);
    DWORD absY = static_cast<DWORD>(((screenY - virtualTop) * 65535) / virtualH);

    // Move mouse to position, then scroll
    INPUT inputs[2] = {};

    inputs[0].type = INPUT_MOUSE;
    inputs[0].mi.dx = absX;
    inputs[0].mi.dy = absY;
    inputs[0].mi.dwFlags = MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK | MOUSEEVENTF_MOVE;

    inputs[1].type = INPUT_MOUSE;
    inputs[1].mi.dwFlags = MOUSEEVENTF_WHEEL;
    inputs[1].mi.mouseData = static_cast<DWORD>(deltaY);

    UINT sent = SendInput(2, inputs, sizeof(INPUT));

    // Restore cursor to original position
    SetCursorPos(savedCursor.x, savedCursor.y);

    return sent == 2;
}

bool VirtualDisplayManager::SendKey(int vkCode, bool isKeyUp) {
    INPUT input = {};
    input.type = INPUT_KEYBOARD;
    input.ki.wVk = static_cast<WORD>(vkCode);
    if (isKeyUp) {
        input.ki.dwFlags = KEYEVENTF_KEYUP;
    }
    return SendInput(1, &input, sizeof(INPUT)) == 1;
}

// --- Private Methods ---

std::wstring VirtualDisplayManager::GetSettingsXmlPath() {
    return VDD_SETTINGS_FILE;
}

bool VirtualDisplayManager::WriteSettingsXml(int width, int height) {
    // VDD expects UTF-8 encoded XML with its specific schema
    std::ofstream file(VDD_SETTINGS_FILE);
    if (!file.is_open()) {
        std::cerr << "[VDM] Cannot open settings XML for writing" << std::endl;
        return false;
    }

    file << "<?xml version='1.0' encoding='utf-8'?>\n";
    file << "<vdd_settings>\n";
    file << "    <monitors>\n";
    file << "        <count>1</count>\n";
    file << "    </monitors>\n";
    file << "    <gpu>\n";
    file << "        <friendlyname>default</friendlyname>\n";
    file << "    </gpu>\n";
    file << "    <global>\n";
    file << "        <g_refresh_rate>60</g_refresh_rate>\n";
    file << "    </global>\n";
    file << "    <resolutions>\n";
    file << "        <resolution>\n";
    file << "            <width>" << width << "</width>\n";
    file << "            <height>" << height << "</height>\n";
    file << "            <refresh_rate>60</refresh_rate>\n";
    file << "        </resolution>\n";
    file << "    </resolutions>\n";
    file << "    <options>\n";
    file << "        <HardwareCursor>true</HardwareCursor>\n";
    file << "    </options>\n";
    file << "</vdd_settings>\n";

    file.close();
    std::cout << "[VDM] Wrote settings XML: " << width << "x" << height << std::endl;
    return true;
}

bool VirtualDisplayManager::WriteEmptySettingsXml() {
    std::ofstream file(VDD_SETTINGS_FILE);
    if (!file.is_open()) return false;

    file << "<?xml version='1.0' encoding='utf-8'?>\n";
    file << "<vdd_settings>\n";
    file << "    <monitors>\n";
    file << "        <count>0</count>\n";
    file << "    </monitors>\n";
    file << "    <gpu>\n";
    file << "        <friendlyname>default</friendlyname>\n";
    file << "    </gpu>\n";
    file << "    <resolutions>\n";
    file << "    </resolutions>\n";
    file << "</vdd_settings>\n";

    file.close();
    std::cout << "[VDM] Wrote empty settings XML" << std::endl;
    return true;
}

bool VirtualDisplayManager::SendPipeCommand(const std::wstring& command) {
    HANDLE hPipe = CreateFileW(
        VDD_PIPE_NAME,
        GENERIC_READ | GENERIC_WRITE,
        0, nullptr,
        OPEN_EXISTING,
        0, nullptr);

    if (hPipe == INVALID_HANDLE_VALUE) {
        std::cerr << "[VDM] Cannot connect to VDD pipe (error " << GetLastError() << ")" << std::endl;
        return false;
    }

    DWORD mode = PIPE_READMODE_MESSAGE;
    SetNamedPipeHandleState(hPipe, &mode, nullptr, nullptr);

    DWORD bytesWritten = 0;
    DWORD dataSize = static_cast<DWORD>(command.size() * sizeof(wchar_t));
    BOOL ok = WriteFile(hPipe, command.c_str(), dataSize, &bytesWritten, nullptr);

    if (!ok) {
        std::cerr << "[VDM] Failed to write to VDD pipe (error " << GetLastError() << ")" << std::endl;
        CloseHandle(hPipe);
        return false;
    }

    // Read response
    wchar_t buffer[512] = {};
    DWORD bytesRead = 0;
    ReadFile(hPipe, buffer, sizeof(buffer) - sizeof(wchar_t), &bytesRead, nullptr);
    if (bytesRead > 0) {
        std::wcout << L"[VDM] Pipe response: " << buffer << std::endl;
    }

    CloseHandle(hPipe);
    std::cout << "[VDM] Sent pipe command: ";
    std::wcout << command << std::endl;
    return true;
}

bool VirtualDisplayManager::NotifyDriverReload() {
    if (SendPipeCommand(L"RELOAD_DRIVER")) {
        return true;
    }
    // Fallback
    ChangeDisplaySettingsExW(nullptr, nullptr, nullptr, 0, nullptr);
    return false;
}

bool VirtualDisplayManager::FindVddAdapter(std::wstring& deviceName) {
    DISPLAY_DEVICEW dd = {};
    dd.cb = sizeof(dd);
    for (DWORD i = 0; EnumDisplayDevicesW(nullptr, i, &dd, 0); i++) {
        std::wstring name(dd.DeviceString);
        if (name.find(L"Virtual Display") != std::wstring::npos ||
            name.find(L"IddSampleDriver") != std::wstring::npos ||
            name.find(L"MTT") != std::wstring::npos ||
            name.find(L"VDD") != std::wstring::npos) {
            deviceName = dd.DeviceName;
            return true;
        }
        dd.cb = sizeof(dd);
    }
    return false;
}
