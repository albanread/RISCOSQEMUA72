/*
 * launcher.c -- the release app's main executable.
 *
 * A bundle whose CFBundleExecutable is a shell script cannot carry the
 * hardened runtime, and notarization asks that of every executable.  So
 * this stub is the Mach-O LaunchServices starts.  All it does is exec
 * /bin/zsh on Contents/Resources/launcher.zsh, which settles the disc, the
 * CMOS and the logs and execs the emulator in turn: the same PID all the
 * way, so the running process is still the app -- scriptable, and with
 * the bundle's identity.  Its own name goes to the script in
 * RISCOS_APP_NAME; every argument is passed on; and if the Option key is
 * down as the app starts, RISCOS_CHOOSE_DISC asks the script to offer the
 * disc-folder choice again.
 */
#include <CoreGraphics/CoreGraphics.h>
#include <limits.h>
#include <mach-o/dyld.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char **argv)
{
    char exe[PATH_MAX], real[PATH_MAX], script[PATH_MAX];
    uint32_t size = sizeof exe;
    char *cut, **args;
    int i;

    if (_NSGetExecutablePath(exe, &size) != 0 || realpath(exe, real) == NULL) {
        perror("launcher: own path");
        return 1;
    }
    cut = strrchr(real, '/');                 /* .../Contents/MacOS/<name> */
    if (cut == NULL) {
        return 1;
    }
    setenv("RISCOS_APP_NAME", cut + 1, 1);
    if (CGEventSourceFlagsState(kCGEventSourceStateCombinedSessionState) & kCGEventFlagMaskAlternate) {
        setenv("RISCOS_CHOOSE_DISC", "1", 1);
    }
    *cut = '\0';
    cut = strrchr(real, '/');                 /* .../Contents/MacOS */
    if (cut == NULL) {
        return 1;
    }
    *cut = '\0';                              /* .../Contents */
    snprintf(script, sizeof script, "%s/Resources/launcher.zsh", real);

    args = calloc((size_t)argc + 2, sizeof *args);
    if (args == NULL) {
        return 1;
    }
    args[0] = "zsh";
    args[1] = script;
    for (i = 1; i < argc; i++) {
        args[i + 1] = argv[i];
    }
    execv("/bin/zsh", args);
    perror("launcher: exec /bin/zsh");
    return 1;
}
