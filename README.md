# Rush Injector

Alpha Auto Inject and Alpha update checks use the public service without a download key. Beta requires a private user key and matching device binding; a key ID, stored SHA-256 hash, or device identity alone cannot authorize a Beta download. Downloaded clients are checked again against the checksum received over HTTPS before automatic or Beta loading, and held against modification during that loader call. This detects cached-file changes in the official injector; it cannot prevent someone modifying the injector itself or copying a DLL already obtained.

Official injector releases: https://github.com/Juice-loot-is-pog/Rush-Injector/releases/latest

The **Update** button checks the latest official release. If a newer version is available, it asks to download, checks the SHA-256 checksum and Windows x64 headers, closes Rush, replaces the executable and restarts it. Turn off Auto Inject before updating. Updates require write access to the executable's folder. The update check runs in a separate Windows PowerShell process and reports errors in a dialog. Existing builds without this button need one manual download of 1.3.0.

To publish a future injector update, change the version in both `CMakeLists.txt` and `src/update_injector.ps1`, run `./build.ps1 -Test`, then run `./download-service/publish_injector.ps1`. The publisher uploads the executable, checksum and corresponding source as a draft, then makes the release public after all uploads succeed. Keep injector releases separate from the client DLL repository.

A Windows x64 Minecraft Bedrock DLL injector with a dark crimson interface and Rush artwork.
Based on [ambiennt/MCBE-DLL-Injector](https://github.com/ambiennt/MCBE-DLL-Injector), licensed under GPL-3.0; upstream sources, bundled wxWidgets, and license notices are retained.

## Run

1. Launch Minecraft Bedrock and wait for its main menu.
2. Open `out/build/Release/Rush Injector.exe`.
3. Select an x64 client DLL with **Choose DLL**.
4. Confirm **Game detected**, then click **Inject client**.

The default process is `Minecraft.Windows.exe`. Use **Custom target** if your installation uses a different executable name. The status shows the detected PID. If several processes share that name, Rush prefers a live process it can open, then the newest.

**Auto Inject** downloads the private Rush client through the download service, verifies its checksum and x64 DLL headers, then waits for three process checks (about six seconds) before making one attempt per process/DLL pair. It starts disabled on each launch. A restarted game reuses the verified download. Toggle it off and on to download again and retry. Manual **Choose DLL** and **Inject client** continue to use the selected local DLL.

The service is deployed and configured at `https://rush-download-service.juiceloot-github.workers.dev/client`; a real private DLL download passed native checksum and x64 validation. Your local owner key is configured. Each additional user needs their own Rush key and a corresponding server hash; follow `download-service/README.md`. No GitHub token is embedded in the app; each user's Rush key is saved in Windows Credential Manager. Only the public service address is stored in the config file.

Downloads run in the background, with progress and any download errors shown in the bottom status text. The Auto Inject button changes to **Cancel Download** during transfer. Closing Rush requests cancellation and waits for any pending network operation to finish (subject to the existing network timeout). Button hover motion uses elapsed-time easing; centered text shrinks with the button using a cached high-resolution text layer to reduce glyph jitter. The Themes menu fits its contents and uses the short `Rush Injector` font preview. Platform metadata is right-aligned below Choose DLL on the same row as the selection status.

Before loading the client, Rush writes all six embedded fonts into a `fonts` subfolder beside the selected DLL, using their original filenames. Auto Inject also prepares this folder immediately after downloading the DLL. Matching files are preserved; missing or damaged files are repaired. Font preparation errors stop the load. This matches the client's lookup of `<DLL directory>/fonts/<font filename>`.

The DLL, client fonts folder, and individual font files receive read/execute access for the All Application Packages group so packaged applications can load them. Other dependencies and parent directories must also be accessible to the game. Settings are stored as UTF-8 under your local application data folder, `Rush Injector/config.txt`.

Success means the selected DLL was observed in the target module list. This does not verify compatibility of a client with a particular Minecraft version. A DLL that fails initialization or has missing dependencies produces an error. A loader exceeding ten seconds produces a timeout; its path allocation stays in the target until that process exits, avoiding a use-after-free.

Every failed load adds an `accessDiag=v1` entry and a `processDiag=v1` entry to **Failure log**. `accessDiag` has injector/target elevation and integrity levels, debug-privilege state, session IDs, and separate process permission probes. Probe values are Windows error codes (`0` means granted). Integrity RIDs normally include `8192` (medium), `12288` (high), and `16384` (system). Unreadable token information is recorded with its own error rather than inferred. `processDiag` lists every process with the target name: PID, thread count, parent PID, the error from opening it (`0` means granted), and age in seconds. The diagnostics do not alter process permissions. When Rush runs as administrator it enables its own debug privilege before opening the game; security software can still block access. A missing or disabled debug privilege alone does not prove the cause; different permissions, security software, and a process exiting during collection can affect the results.

## Build and test

Requires Windows 10/11 x64, Visual Studio 2022 C++ Build Tools (Desktop C++, CMake tools, Windows SDK).

```powershell
.\build.ps1 -Test
```

The artwork and six supplied fonts are embedded in the executable. Fonts are extracted to the app's local font cache and registered privately with wxWidgets before any graphics context is created; Windows-wide installation is unnecessary. The **Theme** menu offers Red, Blue, Purple, and Mono presets, plus Comfortaa, Mojangles, Open Sans, Product Sans, Sarabun Light, and SF Pro Display. Choices apply immediately and persist between launches. The main window stays at its intended size, with resizing and maximizing disabled. The executable and window use the supplied cat image as their icon. wxWidgets is built as static libraries; the Microsoft x64 Visual C++ runtime is required.

The integration test creates its own host process and checks that DLL_PROCESS_ATTACH executes, exercises a long Unicode path, repeat loading, missing files, invalid PE files, x86 rejection, and invalid process handling. Minecraft-specific behavior requires a running game and a compatible client DLL.

The font regression test checks native font selection and distinct, nonblank graphics-rendered output for all six fonts in regular and bold modes. Its `--legacy` option reproduces the previous renderer fallback; a font preview sheet is saved with the test output.

## Discord Rich Presence

While Rush Injector is open, it connects to the local Discord desktop app using application ID 1565548127367340172. The card shows Minecraft Bedrock, elapsed time, and Download and Discord buttons. It reconnects if Discord starts later or restarts, and disconnects when Rush closes. No Discord token or beta credentials are used. Discord activity sharing must be enabled.

Upload the Rush artwork as a Rich Presence asset named rush_logo in this application's Discord Developer Portal. The application name controls the card title. Artwork setup is separate from the executable; local RPC cannot upload it.
