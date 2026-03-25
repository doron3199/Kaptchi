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

    // Find the existing active VDD monitor (created during installation via VDD Control)
    int idx = GetVirtualMonitorIndex();
    if (idx >= 0) {
        std::cout << "[VDM] Virtual monitor active at index " << idx << std::endl;
        return idx;
    }

    std::cerr << "[VDM] VDD driver installed but no active virtual monitor found" << std::endl;
    return -1;
}

bool VirtualDisplayManager::RemoveVirtualMonitor() {
    // Write empty settings to remove the virtual monitor
    if (!WriteEmptySettingsXml()) {
        return false;
    }

    NotifyDriverReload();

    // Wait briefly for removal
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
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

bool VirtualDisplayManager::LaunchInstaller(const wchar_t* vddControlPath) {
    SHELLEXECUTEINFOW sei = {};
    sei.cbSize = sizeof(sei);
    sei.fMask = SEE_MASK_NOCLOSEPROCESS;
    sei.lpVerb = L"runas"; // Request admin elevation
    sei.lpFile = vddControlPath;
    sei.nShow = SW_SHOWNORMAL;

    if (!ShellExecuteExW(&sei)) {
        std::cerr << "[VDM] Failed to launch VDD.Control: " << GetLastError() << std::endl;
        return false;
    }

    // Wait for the installer to finish (up to 5 minutes)
    if (sei.hProcess) {
        WaitForSingleObject(sei.hProcess, 300000);
        CloseHandle(sei.hProcess);
    }

    return true;
}

bool VirtualDisplayManager::UninstallDriver(const wchar_t* devconPath) {
    SHELLEXECUTEINFOW sei = {};
    sei.cbSize = sizeof(sei);
    sei.fMask = SEE_MASK_NOCLOSEPROCESS;
    sei.lpVerb = L"runas"; // Request admin elevation
    sei.lpFile = devconPath;
    sei.lpParameters = L"remove root\\MttVDD";
    sei.nShow = SW_HIDE;

    if (!ShellExecuteExW(&sei)) {
        std::cerr << "[VDM] Failed to launch devcon for uninstall: " << GetLastError() << std::endl;
        return false;
    }

    if (sei.hProcess) {
        WaitForSingleObject(sei.hProcess, 60000);
        DWORD exitCode = 0;
        GetExitCodeProcess(sei.hProcess, &exitCode);
        CloseHandle(sei.hProcess);
        if (exitCode != 0) {
            std::cerr << "[VDM] devcon remove failed with exit code: " << exitCode << std::endl;
            return false;
        }
    }

    std::cout << "[VDM] VDD driver uninstalled successfully" << std::endl;
    return true;
}

bool VirtualDisplayManager::SendClick(float normalizedX, float normalizedY, int clickType) {
    int vdIdx = GetVirtualMonitorIndex();
    if (vdIdx < 0) return false;

    RECT bounds = GetMonitorBounds(vdIdx);
    int monW = bounds.right - bounds.left;
    int monH = bounds.bottom - bounds.top;
    if (monW <= 0 || monH <= 0) return false;

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
    return sent == static_cast<UINT>(inputCount);
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
    std::wofstream file(VDD_SETTINGS_FILE);
    if (!file.is_open()) {
        std::cerr << "[VDM] Cannot open settings XML for writing" << std::endl;
        return false;
    }

    // Write VDD settings XML with one monitor
    file << L"<?xml version=\"1.0\" encoding=\"utf-8\"?>" << std::endl;
    file << L"<VddSettings>" << std::endl;
    file << L"  <Monitors>" << std::endl;
    file << L"    <Monitor>" << std::endl;
    file << L"      <Width>" << width << L"</Width>" << std::endl;
    file << L"      <Height>" << height << L"</Height>" << std::endl;
    file << L"      <RefreshRates>" << std::endl;
    file << L"        <Rate>60</Rate>" << std::endl;
    file << L"      </RefreshRates>" << std::endl;
    file << L"    </Monitor>" << std::endl;
    file << L"  </Monitors>" << std::endl;
    file << L"</VddSettings>" << std::endl;

    file.close();
    return true;
}

bool VirtualDisplayManager::WriteEmptySettingsXml() {
    std::wofstream file(VDD_SETTINGS_FILE);
    if (!file.is_open()) return false;

    file << L"<?xml version=\"1.0\" encoding=\"utf-8\"?>" << std::endl;
    file << L"<VddSettings>" << std::endl;
    file << L"  <Monitors>" << std::endl;
    file << L"  </Monitors>" << std::endl;
    file << L"</VddSettings>" << std::endl;

    file.close();
    return true;
}

bool VirtualDisplayManager::NotifyDriverReload() {
    // The VDD driver watches for changes to its settings.
    // We can also try to trigger a display refresh via ChangeDisplaySettingsEx.
    // First, try the registry notification approach used by VDD.Control:
    HKEY hKey;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\VirtualDisplayDriver",
                      0, KEY_SET_VALUE, &hKey) == ERROR_SUCCESS) {
        // Write a "reload" trigger value
        DWORD val = 1;
        RegSetValueExW(hKey, L"ReloadSettings", 0, REG_DWORD,
                       reinterpret_cast<const BYTE*>(&val), sizeof(val));
        RegCloseKey(hKey);
    }

    // Also trigger a display settings change to force re-enumeration
    ChangeDisplaySettingsExW(nullptr, nullptr, nullptr, 0, nullptr);

    return true;
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
