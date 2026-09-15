/*
 * launcher.c -- the end-user Windows launcher.
 *
 * The Windows counterpart of app/launcher.c + app/launcher.zsh on the Mac:
 * a compiled stub so the desktop shortcut starts a program rather than a
 * script.  A .cmd would flash a console window on every launch and would
 * put the whole command line on screen when anything went wrong.
 *
 * It runs the emulator from its own directory against the user's disc
 * folder:
 *
 *     <this exe's directory>      the emulator, its DLLs and the ROM
 *     %USERPROFILE%\<APP_NAME>    the disc, which is also the HostFS share
 *
 * The disc folder is the machine's filing system, so it is somewhere the
 * user can open; putting it under AppData would hide the one thing they
 * need to reach to get files in and out.
 *
 * There is no card: the share is the whole machine (--no-card in run.py's
 * terms).  The CMOS comes from the share's own CMOS,ff2 and is written
 * back there by RISC OS, so *Configure survives a restart.
 *
 * The network is HostNet or the ROM's own stack, and the disc decides
 * which, by where HostNet's module is:
 *
 *     Modules\HostNet,ffa            on: the boot loads it, and being
 *                                    titled Internet it replaces the ROM's
 *     Modules\Disabled\HostNet,ffa   off: the boot leaves it alone, and the
 *                                    ROM's stack drives the emulated card
 *
 * The window menu's HostNet item moves the file, and the switch takes
 * effect when RISC OS next starts -- which may be a restart inside this
 * session:
 *
 *   - The doorbell is lit only when HostNet is on.  Off is the device's
 *     default, and the window then reads exactly as it did before HostNet
 *     existed.  Switching on from the menu lights it there and then, so a
 *     restart of RISC OS finds it; without that the module would replace
 *     the ROM's Internet module as it loads, decline for want of a host,
 *     and leave none at all.  A file moved by hand waits for the next start.
 *
 *   - The card is always attached, on slirp.  Under HostNet nothing drives
 *     it; switched off, the ROM's stack has it.  Without it, switching off
 *     and restarting RISC OS boots a stack with no interface: "Route:
 *     Network is unreachable", and no network until the emulator restarts.
 *
 * APP_NAME is set at compile time by make-release.py.
 */

#include <windows.h>
#include <shlobj.h>
#include <stdio.h>
#include <stdlib.h>

#ifndef APP_NAME
#define APP_NAME L"RISCOSQEA72"
#endif

#define EMULATOR L"qemu-system-aarch64.exe"

/*
 * QEMU splits an option value at commas, so a literal comma has to be
 * doubled.  That is not a corner case here: a RISC OS filetype suffix IS a
 * comma, so the settings file is called CMOS,ff2 and every launch needs it.
 * Without this QEMU reports "Property 'loader.ff2' not found" and exits
 * before the window ever appears.
 */
static void comma_escape(const wchar_t *in, wchar_t *out, size_t n)
{
    size_t i = 0;

    for (; *in && i + 2 < n; in++) {
        out[i++] = *in;
        if (*in == L',') {
            out[i++] = L',';
        }
    }
    out[i] = 0;
}

static void fail(const wchar_t *what, const wchar_t *detail)
{
    wchar_t msg[1024];
    _snwprintf(msg, 1024, L"%ls\n\n%ls", what, detail ? detail : L"");
    MessageBoxW(NULL, msg, APP_NAME L" could not start", MB_ICONERROR | MB_OK);
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, PWSTR cmdline, int show)
{
    wchar_t exe[MAX_PATH], app[MAX_PATH], disc[MAX_PATH], emu[MAX_PATH];
    wchar_t rom[MAX_PATH], cmos[MAX_PATH], hostnet[MAX_PATH];
    wchar_t cmos_opt[MAX_PATH * 2], disc_opt[MAX_PATH * 2];
    wchar_t *cmd;
    const wchar_t *net;
    wchar_t profile[MAX_PATH];
    STARTUPINFOW si = { .cb = sizeof si };
    PROCESS_INFORMATION pi;
    DWORD attrs;
    size_t n;

    (void)inst; (void)prev; (void)cmdline; (void)show;

    /* Where we are: the program directory holds everything but the disc. */
    if (!GetModuleFileNameW(NULL, exe, MAX_PATH)) {
        fail(L"Cannot find my own program directory.", NULL);
        return 1;
    }
    wcscpy(app, exe);
    for (n = wcslen(app); n > 0 && app[n - 1] != L'\\'; n--) {
        ;
    }
    app[n] = L'\0';                       /* trailing backslash kept */

    _snwprintf(emu, MAX_PATH, L"%ls%ls", app, EMULATOR);
    _snwprintf(rom, MAX_PATH, L"%ls%ls", app, L"RISCOS.IMG");

    /* The disc: %USERPROFILE%\<APP_NAME>, made if it is not there yet.
     * The environment variable rather than SHGetKnownFolderPath, which
     * would drag in the KNOWNFOLDERID GUIDs and ole32 for no gain. */
    if (!GetEnvironmentVariableW(L"USERPROFILE", profile, MAX_PATH)) {
        fail(L"Cannot find your user folder (USERPROFILE is not set).", NULL);
        return 1;
    }
    _snwprintf(disc, MAX_PATH, L"%ls\\%ls", profile, APP_NAME);

    if (GetFileAttributesW(disc) == INVALID_FILE_ATTRIBUTES) {
        if (SHCreateDirectoryExW(NULL, disc, NULL) != ERROR_SUCCESS) {
            fail(L"Cannot create your disc folder:", disc);
            return 1;
        }
    }
    _snwprintf(cmos, MAX_PATH, L"%ls\\CMOS,ff2", disc);

    if (GetFileAttributesW(cmos) == INVALID_FILE_ATTRIBUTES) {
        fail(L"Your disc folder has no settings file, so the machine has "
             L"nothing to boot.\n\nRe-run the installer to restore it:",
             disc);
        return 1;
    }

    /* The doorbell, lit only with HostNet on: its module in Modules itself.
     * The card is attached regardless, below; see the top of the file. */
    _snwprintf(hostnet, MAX_PATH, L"%ls\\Modules\\HostNet,ffa", disc);
    attrs = GetFileAttributesW(hostnet);
    net = (attrs != INVALID_FILE_ATTRIBUTES
           && !(attrs & FILE_ATTRIBUTE_DIRECTORY))
        ? L"-global hostnet.sockets=on " : L"";

    /*
     * The launch.  No -drive at all: the share is the machine.  Sound needs
     * both halves -- a backend and the vchiq peer told to use it -- or the
     * guest plays to nothing in silence.
     */
    comma_escape(cmos, cmos_opt, MAX_PATH * 2);
    comma_escape(disc, disc_opt, MAX_PATH * 2);

    n = 4096;
    cmd = malloc(n * sizeof *cmd);
    if (!cmd) {
        return 1;
    }
    _snwprintf(cmd, n,
        L"\"%ls\" -M raspi4b -cpu cortex-a72,aarch64=off "
        L"-kernel \"%ls\" "
        L"-device \"loader,file=%ls,addr=0x510000,force-raw=on\" "
        L"-device usb-hub,bus=usb-bus.0,port=1 "
        L"-device usb-kbd,bus=usb-bus.0,port=1.1 "
        L"-device usb-tablet,bus=usb-bus.0,port=1.2 "
        L"-netdev user,id=n0,domainname=lan "
        L"-device usb-net,netdev=n0,rndis=off,bus=usb-bus.0,port=1.3 "
        L"%ls"
        L"-audiodev dsound,id=snd0 -global bcm2835-vchiq.audiodev=snd0 "
        L"-display dx11 -serial null "
        L"-global \"bcm2838-peripherals.vmchannel-root=%ls\"",
        emu, rom, cmos_opt, net, disc_opt);

    /* Working directory is the program directory, so the DLLs beside the
     * emulator are the ones it finds. */
    if (!CreateProcessW(NULL, cmd, NULL, NULL, FALSE,
                        CREATE_NO_WINDOW, NULL, app, &si, &pi)) {
        wchar_t why[512];
        _snwprintf(why, 512, L"%ls\n\nWindows error %lu.", emu,
                   (unsigned long)GetLastError());
        fail(L"Cannot start the emulator:", why);
        free(cmd);
        return 1;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    free(cmd);
    return 0;
}
