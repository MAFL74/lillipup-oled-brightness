# Lillipup OLED brightness bridge

Experimental brightness support for the internal OLED panel on the **Lenovo Chromebook Flex 5i (Lillipup)** running Windows 11.

On the tested machine, Windows' 0–100% brightness value controls panel brightness across the full range. The bridge polls Windows' brightness value and calls a small kernel driver, which configures the panel's eDP AUX backlight controls. During testing, panel readback followed 0%, 10%, 30%, 50%, and 100% as 0, 204, 614, 1024, and 2047; the user visually confirmed clear dimming between 0% and 100%.

## Hardware scope

Tested only on this configuration:

- Lenovo Chromebook Flex 5i, board name Lillipup
- Intel UHD Graphics, PCI ID `8086:9A78` (Tiger Lake)
- Internal OLED panel enumerated as `DISPLAY\SDC4162`
- Windows 11, Intel graphics driver `32.0.101.7092`

The kernel driver directly maps Intel GPU MMIO at physical address `0x90000000`, which was observed as BAR0 on the tested machine. **Do not install or run this driver on a different computer or GPU.** It is not a general Windows display driver, WDDM miniport, or Intel replacement driver. Incorrect MMIO access can hang Windows or damage the display state. Firmware behavior, GPU BAR allocation, Windows version, and Intel driver updates may change the result.

## Security and signing

The included `.sys` is test-signed with the public certificate in `driver/LillipupAuxProbe-test.cer`. It is an experimental, locally trusted kernel driver, not Microsoft-attested or production-signed. To load a test-signed kernel driver, Windows test-signing mode must be enabled and Secure Boot may need to be disabled in firmware. Those settings reduce platform protections. This repository does **not** change boot or Secure Boot settings for you.

Before trusting the certificate, inspect its subject and thumbprint and verify the included driver's signature. Only trust it if you understand the hardware restriction and accept test-signing mode. Do not import the certificate on a device outside the tested configuration.

For rebuilding from source, install Visual Studio with the Windows Driver Kit (WDK) and Windows SDK `10.0.26100.0`, build `driver/LillipupAuxProbe.vcxproj` for `x64`, then sign the resulting `.sys` with your own test certificate and trust that certificate locally. The private key used for the distributed test build is not included.

## Install

1. Confirm that the device matches every hardware item above. Check that `driver/LillipupAuxProbe.sys` has a valid signature from the included certificate.
2. Follow Microsoft's instructions to enable test-signing mode only if you choose to accept the security tradeoff. Restart Windows after changing boot configuration.
3. Import the included `.cer` into the **Local Computer** certificate stores **Trusted Root Certification Authorities** and **Trusted Publishers**. This grants trust to software signed by that test certificate; review its thumbprint before importing.
4. Open an elevated PowerShell in this folder and run:

   ```powershell
   .\driver\Install-Driver.ps1
   ```

5. In a regular PowerShell window, install the user-mode bridge:

   ```powershell
   .\bridge\Install.ps1
   ```

6. Change brightness using Windows Settings or the Chromebook brightness keys. The bridge log is at `%LOCALAPPDATA%\LillipupBrightnessBridge\bridge.log`.

At 0%, the panel may go completely dark. Use the brightness-up key or move the Windows slider to restore the image.

## Remove

Run `bridge/Uninstall.ps1` to stop the bridge. From an elevated PowerShell, run `driver/Uninstall-Driver.ps1` to stop and remove the kernel service. If you enabled test-signing mode for this project, you can disable it with Microsoft's documented procedure and restart. Remove the test certificate from the two Local Computer stores if no longer needed.

## Contents

- `bridge/`: C# source, the tested bridge executable, and per-user scheduled-task install/removal scripts.
- `driver/`: WDM driver source/project, the tested test-signed binary, public test certificate, and service install/removal scripts.

The driver supports both an ACPI experiment and the tested eDP AUX path. The bridge uses the latter and verifies the panel brightness readback before treating a change as successful. Its click-through overlay remains only as a fallback if native AUX control is unavailable.

## License

MIT; see `LICENSE`. The included binaries are provided as-is for this narrowly scoped experiment.

