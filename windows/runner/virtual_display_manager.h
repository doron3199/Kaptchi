#pragma once

#include <windows.h>
#include <string>
#include <vector>

class VirtualDisplayManager {
public:
    // Check if VDD is installed by looking for the driver in display adapters
    static bool IsDriverInstalled();

    // Create a virtual monitor with the given resolution.
    // Writes config to VDD settings XML and triggers driver refresh.
    // Returns the DXGI monitor index of the new virtual display, or -1 on failure.
    static int CreateVirtualMonitor(int width, int height);

    // Remove the virtual monitor created by this app.
    static bool RemoveVirtualMonitor();

    // Get the monitor index of the VDD virtual display (-1 if not found).
    static int GetVirtualMonitorIndex();

    // Move a window to the virtual display and maximize it.
    static bool MoveWindowToMonitor(HWND hwnd, int monitorIndex);

    // Get the screen-coordinate bounds of a monitor by DXGI index.
    static RECT GetMonitorBounds(int monitorIndex);

    // Install the bundled VDD driver from vdd_driver/ next to the executable.
    // Uses SetupDi API to create a device node and bind the driver.
    static bool InstallBundledDriver();

    // Uninstall the VDD driver using pnputil (requires admin/UAC).
    static bool UninstallDriver(const wchar_t* unused = nullptr);

    // Send a mouse click to the virtual display at normalized coordinates (0.0-1.0).
    // clickType: 0 = left click, 1 = right click, 2 = left down, 3 = left up
    static bool SendClick(float normalizedX, float normalizedY, int clickType);

    // Send a mouse scroll to the virtual display at normalized coordinates.
    // deltaY: positive = scroll up, negative = scroll down (in WHEEL_DELTA units, 120 = one notch)
    static bool SendScroll(float normalizedX, float normalizedY, int deltaY);

    // Send a key event to the window on the virtual display.
    // vkCode: virtual key code, isKeyUp: true for key release
    static bool SendKey(int vkCode, bool isKeyUp);

private:
    // Path to the VDD settings XML
    static std::wstring GetSettingsXmlPath();

    // Write a minimal VDD settings XML with one monitor at the given resolution
    static bool WriteSettingsXml(int width, int height);

    // Write a settings XML with zero monitors (disables virtual display)
    static bool WriteEmptySettingsXml();

    // Send a command to the VDD driver through its named pipe
    static bool SendPipeCommand(const std::wstring& command);

    // Notify the VDD driver to reload its configuration
    static bool NotifyDriverReload();

    // Find display adapter matching VDD
    static bool FindVddAdapter(std::wstring& deviceName);
};
