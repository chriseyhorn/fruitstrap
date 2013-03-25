//TODO: don't copy/mount DeveloperDiskImage.dmg if it's already done - Xcode checks this somehow

#import <CoreFoundation/CoreFoundation.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <stdio.h>
#include <signal.h>
#include <getopt.h>
#include <pwd.h>
#include "MobileDevice.h"

#define FDVENDOR_PATH  "/tmp/fruitstrap-remote-debugserver"
#define PREP_CMDS_PATH "/tmp/fruitstrap-gdb-prep-cmds"
#define GDB_SHELL_ARGS      " --arch armv7 -q -x " PREP_CMDS_PATH

#define PRINT(...) printf(__VA_ARGS__)

// approximation of what Xcode does:
#define GDB_PREP_CMDS CFSTR("set mi-show-protections off\n\
    set auto-raise-load-levels 1\n\
    set shlib-path-substitutions /usr \"{ds_path}/Symbols/usr\" /System \"{ds_path}/Symbols/System\" \"{device_container}\" \"{disk_container}\" \"/private{device_container}\" \"{disk_container}\" /Developer \"{ds_path}/Symbols/Developer\"\n\
    set remote max-packet-size 1024\n\
    set sharedlibrary check-uuids on\n\
    set env NSUnbufferedIO YES\n\
    set minimal-signal-handling 1\n\
    set sharedlibrary load-rules \\\".*\\\" \\\".*\\\" container\n\
    set inferior-auto-start-dyld 0\n\
    file \"{disk_app}\"\n\
    set remote executable-directory {device_app}\n\
    set remote noack-mode 1\n\
    set trust-readonly-sections 1\n\
    target remote-mobile " FDVENDOR_PATH "\n\
    mem 0x1000 0x3fffffff cache\n\
    mem 0x40000000 0xffffffff none\n\
    mem 0x00000000 0x0fff none\n\
    run {args}\n\
    set minimal-signal-handling 0\n\
    set inferior-auto-start-cfm off\n\
    set sharedLibrary load-rules dyld \".*libobjc.*\" all dyld \".*CoreFoundation.*\" all dyld \".*Foundation.*\" all dyld \".*libSystem.*\" all dyld \".*AppKit.*\" all dyld \".*PBGDBIntrospectionSupport.*\" all dyld \".*/usr/lib/dyld.*\" all dyld \".*CarbonDataFormatters.*\" all dyld \".*libauto.*\" all dyld \".*CFDataFormatters.*\" all dyld \"/System/Library/Frameworks\\\\\\\\|/System/Library/PrivateFrameworks\\\\\\\\|/usr/lib\" extern dyld \".*\" all exec \".*\" all\n\
    sharedlibrary apply-load-rules all\n\
    set inferior-auto-start-dyld 1\n\
    {run-app}\n\
    quit\n\
    ")

// This guy leaves out local path info for the source code searches, since we won't find any source..
#define GDB_PREP_CMDS_NO_SOURCE CFSTR("set mi-show-protections off\n\
    set auto-raise-load-levels 1\n\
    set shlib-path-substitutions /usr \"{ds_path}/Symbols/usr\" /System \"{ds_path}/Symbols/System\" /Developer \"{ds_path}/Symbols/Developer\"\n\
    set remote max-packet-size 1024\n\
    set sharedlibrary check-uuids on\n\
    set env NSUnbufferedIO YES\n\
    set minimal-signal-handling 1\n\
    set sharedlibrary load-rules \\\".*\\\" \\\".*\\\" container\n\
    set inferior-auto-start-dyld 0\n\
    set remote executable-directory {device_app}\n\
    set remote noack-mode 1\n\
    set trust-readonly-sections 1\n\
    target remote-mobile " FDVENDOR_PATH "\n\
    mem 0x1000 0x3fffffff cache\n\
    mem 0x40000000 0xffffffff none\n\
    mem 0x00000000 0x0fff none\n\
    run {args}\n\
    set minimal-signal-handling 0\n\
    set inferior-auto-start-cfm off\n\
    set sharedLibrary load-rules dyld \".*libobjc.*\" all dyld \".*CoreFoundation.*\" all dyld \".*Foundation.*\" all dyld \".*libSystem.*\" all dyld \".*AppKit.*\" all dyld \".*PBGDBIntrospectionSupport.*\" all dyld \".*/usr/lib/dyld.*\" all dyld \".*CarbonDataFormatters.*\" all dyld \".*libauto.*\" all dyld \".*CFDataFormatters.*\" all dyld \"/System/Library/Frameworks\\\\\\\\|/System/Library/PrivateFrameworks\\\\\\\\|/usr/lib\" extern dyld \".*\" all exec \".*\" all\n\
    sharedlibrary apply-load-rules all\n\
    set inferior-auto-start-dyld 1\n\
    {run-app}\n\
    quit\n\
    ")

typedef struct am_device * AMDeviceRef;
int AMDeviceSecureTransferPath(int zero, AMDeviceRef device, CFURLRef url, CFDictionaryRef options, void *callback, int cbarg);
int AMDeviceSecureInstallApplication(int zero, AMDeviceRef device, CFURLRef url, CFDictionaryRef options, void *callback, int cbarg);
int AMDeviceMountImage(AMDeviceRef device, CFStringRef image, CFDictionaryRef options, void *callback, int cbarg);
int AMDeviceLookupApplications(AMDeviceRef device, int zero, CFDictionaryRef* result);

bool found_device = false, debug = false, verbose = false;
// Whether to install the app
bool g_install = true;
// Bundle identifier to use, overrides app_path and copy_disc_app_identifier()
char *g_bundle_identifier = NULL;
// Whether to run without debugging
bool g_run = false;
// Whether to kill the app instead of running or debugging
bool g_kill = false;
// We don't have the source for the program we're running
bool g_nosource = false;

char *app_path = NULL;
char *device_id = NULL;
char *args = NULL;
int timeout = 0;
CFStringRef last_path = NULL;
service_conn_t gdbfd;

char *cstring_from_cfstringref(CFStringRef stringRef, bool *needsRelease) {
    char *cstring = (char *)CFStringGetCStringPtr(stringRef, kCFStringEncodingUTF8);
    if (cstring) {
        if (needsRelease) {
            *needsRelease = false;
        }
        return cstring;
    }
    
    CFIndex stringLength = CFStringGetLength(stringRef);
    CFIndex bufferLength = CFStringGetMaximumSizeForEncoding(stringLength, kCFStringEncodingUTF8) + 1;
    char *buffer = malloc(bufferLength);
    if (buffer) {
        CFIndex used = 0;
        CFIndex converted = CFStringGetBytes(stringRef, CFRangeMake(0, stringLength), kCFStringEncodingUTF8, '?', false, buffer, bufferLength, &used);
        buffer[used] = '\0'; // Null-terminate the string
        if (needsRelease) {
            *needsRelease = true;
        }
        return buffer;
    }
    
    if (needsRelease) {
        *needsRelease = false;
    }
    return NULL; // Couldn't get the string
}

Boolean path_exists(CFTypeRef path) {
    if (CFGetTypeID(path) == CFStringGetTypeID()) {
        CFURLRef url = CFURLCreateWithFileSystemPath(NULL, path, kCFURLPOSIXPathStyle, true);
        Boolean result = CFURLResourceIsReachable(url, NULL);
        CFRelease(url);
        return result;
    } else if (CFGetTypeID(path) == CFURLGetTypeID()) {
        return CFURLResourceIsReachable(path, NULL);
    } else {
        return false;
    }
}

CFStringRef copy_xcode_dev_path() {
    FILE *fpipe = NULL;
    char *command = "xcode-select -print-path";
    
    if (!(fpipe = (FILE *)popen(command, "r")))
    {
        perror("Error encountered while opening pipe");
        exit(EXIT_FAILURE);
    }
    
    char buffer[256] = { '\0' };
    
    fgets(buffer, sizeof(buffer), fpipe);
    pclose(fpipe);
    
    strtok(buffer, "\n");
    return CFStringCreateWithCString(NULL, buffer, kCFStringEncodingUTF8);
}

const char *get_home() {
    const char* home = getenv("HOME");
    if (!home) {
        struct passwd *pwd = getpwuid(getuid());
        home = pwd->pw_dir;
    }
    return home;
}

CFStringRef copy_xcode_path_for(CFStringRef search) {
    CFStringRef xcodeDevPath = copy_xcode_dev_path();
    CFStringRef path;
    bool found = false;
    const char* home = get_home();
    
    
    // Try using xcode-select --print-path
    if (!found) {
        path = CFStringCreateWithFormat(NULL, NULL, CFSTR("%@/%@"), xcodeDevPath, search);
        found = path_exists(path);
    }
    // If not look in the default xcode location (xcode-select is sometimes wrong)
    if (!found) {
        path = CFStringCreateWithFormat(NULL, NULL, CFSTR("/Applications/Xcode.app/Contents/Developer/%@"), search);
        found = path_exists(path);
    }
    // If not look in the users home directory, Xcode can store device support stuff there
    if (!found) {
        path = CFStringCreateWithFormat(NULL, NULL, CFSTR("%s/Library/Developer/Xcode/%@"), home, search);
        found = path_exists(path);
    }
    
    CFRelease(xcodeDevPath);
    
    if (found) {
        return path;
    } else {
        CFRelease(path);
        return NULL;
    }
}

CFStringRef copy_device_support_path(AMDeviceRef device) {
    // TODO: Change priority of the search here to check via xcode-select first.
    CFStringRef version = AMDeviceCopyValue(device, 0, CFSTR("ProductVersion"));
    CFStringRef build = AMDeviceCopyValue(device, 0, CFSTR("BuildVersion"));
    const char* home = getenv("HOME");
    CFStringRef path;
    bool found = false;

    path = CFStringCreateWithFormat(NULL, NULL, CFSTR("%s/Library/Developer/Xcode/iOS DeviceSupport/%@ (%@)"), home, version, build);
    found = path_exists(path);

    if (!found)
    {
        path = CFStringCreateWithFormat(NULL, NULL, CFSTR("/Developer/Platforms/iPhoneOS.platform/DeviceSupport/%@ (%@)"), version, build);
        found = path_exists(path);
    }
    if (!found)
    {
        path = CFStringCreateWithFormat(NULL, NULL, CFSTR("%s/Library/Developer/Xcode/iOS DeviceSupport/%@"), home, version);
        found = path_exists(path);
    }
    if (!found)
    {
        path = CFStringCreateWithFormat(NULL, NULL, CFSTR("/Developer/Platforms/iPhoneOS.platform/DeviceSupport/%@"), version);
        found = path_exists(path);
    }
    if (!found)
    {
        path = CFStringCreateWithFormat(NULL, NULL, CFSTR("/Applications/Xcode.app/Contents/Developer/Platforms/iPhoneOS.platform/DeviceSupport/%@"), version);
        found = path_exists(path);
    }

    CFRelease(version);
    CFRelease(build);

    if (!found)
    {
        CFRelease(path);
        printf("[ !! ] Unable to locate DeviceSupport directory.\n");
        exit(1);
    }

    return path;
}

CFStringRef copy_long_shot_disk_image_path() {
    FILE *fpipe = NULL;
    char *command = "find `xcode-select --print-path` -name DeveloperDiskImage.dmg | tail -n 1";
    
    if (!(fpipe = (FILE *)popen(command, "r")))
    {
        perror("Error encountered while opening pipe");
        exit(EXIT_FAILURE);
    }
    
    char buffer[256] = { '\0' };
    
    fgets(buffer, sizeof(buffer), fpipe);
    pclose(fpipe);
    
    strtok(buffer, "\n");
    return CFStringCreateWithCString(NULL, buffer, kCFStringEncodingUTF8);
}

CFStringRef copy_developer_disk_image_path(AMDeviceRef device) {
    CFStringRef version = AMDeviceCopyValue(device, 0, CFSTR("ProductVersion"));
    CFStringRef build = AMDeviceCopyValue(device, 0, CFSTR("BuildVersion"));
    CFStringRef path = NULL;
    
    if (path == NULL) {
        path = copy_xcode_path_for(CFStringCreateWithFormat(NULL, NULL, CFSTR("Platforms/iPhoneOS.platform/DeviceSupport/%@ (%@)/DeveloperDiskImage.dmg"), version, build));
    }
    if (path == NULL) {
        path = copy_xcode_path_for(CFStringCreateWithFormat(NULL, NULL, CFSTR("Platforms/iPhoneOS.platform/DeviceSupport/%@/DeveloperDiskImage.dmg"), version));
    }
    if (path == NULL) {
        path = copy_xcode_path_for(CFSTR("Platforms/iPhoneOS.platform/DeviceSupport/Latest/DeveloperDiskImage.dmg"));
    }
    
    CFRelease(version);
    CFRelease(build);
    
    if (path == NULL) {
        // Sometimes Latest seems to be missing in Xcode, in that case use find and hope for the best
        path = copy_long_shot_disk_image_path();
        if (CFStringGetLength(path) < 5) {
            path = NULL;
        }
    }
    
    if (path == NULL) {
        PRINT("[ !! ] Unable to locate DeveloperDiskImage.dmg.\n[ !! ] This probably means you don't have Xcode installed, you will need to launch the app manually and logging output will not be shown!\n");
        exit(1);
    }
    
    return path;
}

void mount_callback(CFDictionaryRef dict, int arg) {
    CFStringRef status = CFDictionaryGetValue(dict, CFSTR("Status"));

    if (CFEqual(status, CFSTR("LookingUpImage"))) {
        printf("[  0%%] Looking up developer disk image\n");
    } else if (CFEqual(status, CFSTR("CopyingImage"))) {
        printf("[ 30%%] Copying DeveloperDiskImage.dmg to device\n");
    } else if (CFEqual(status, CFSTR("MountingImage"))) {
        printf("[ 90%%] Mounting developer disk image\n");
    }
}

void mount_developer_image(AMDeviceRef device) {
    CFStringRef ds_path = copy_device_support_path(device);
    CFStringRef image_path = copy_developer_disk_image_path(device);
    CFStringRef sig_path = CFStringCreateWithFormat(NULL, NULL, CFSTR("%@.signature"), image_path);
    CFRelease(ds_path);

    if (verbose) {
        printf("Device support path: ");
        fflush(stdout);
        CFShow(ds_path);
        printf("Developer disk image: ");
        fflush(stdout);
        CFShow(image_path);
    }

    FILE* sig = fopen(CFStringGetCStringPtr(sig_path, kCFStringEncodingMacRoman), "rb");
    void *sig_buf = malloc(128);
    assert(fread(sig_buf, 1, 128, sig) == 128);
    fclose(sig);
    CFDataRef sig_data = CFDataCreateWithBytesNoCopy(NULL, sig_buf, 128, NULL);
    CFRelease(sig_path);

    CFTypeRef keys[] = { CFSTR("ImageSignature"), CFSTR("ImageType") };
    CFTypeRef values[] = { sig_data, CFSTR("Developer") };
    CFDictionaryRef options = CFDictionaryCreate(NULL, (const void **)&keys, (const void **)&values, 2, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFRelease(sig_data);

    int result = AMDeviceMountImage(device, image_path, options, &mount_callback, 0);
    if (result == 0) {
        printf("[ 95%%] Developer disk image mounted successfully\n");
    } else if (result == 0xe8000076 /* already mounted */) {
        printf("[ 95%%] Developer disk image already mounted\n");
    } else {
        printf("[ !! ] Unable to mount developer disk image. (%x)\n", result);
        exit(1);
    }

    CFRelease(image_path);
    CFRelease(options);
}

void transfer_callback(CFDictionaryRef dict, int arg) {
    int percent;
    CFStringRef status = CFDictionaryGetValue(dict, CFSTR("Status"));
    CFNumberGetValue(CFDictionaryGetValue(dict, CFSTR("PercentComplete")), kCFNumberSInt32Type, &percent);

    if (CFEqual(status, CFSTR("CopyingFile"))) {
        CFStringRef path = CFDictionaryGetValue(dict, CFSTR("Path"));

        if ((last_path == NULL || !CFEqual(path, last_path)) && !CFStringHasSuffix(path, CFSTR(".ipa"))) {
            printf("[%3d%%] Copying %s to device\n", percent / 2, CFStringGetCStringPtr(path, kCFStringEncodingMacRoman));
        }

        if (last_path != NULL) {
            CFRelease(last_path);
        }
        last_path = CFStringCreateCopy(NULL, path);
    }
}

void install_callback(CFDictionaryRef dict, int arg) {
    int percent;
    CFStringRef status = CFDictionaryGetValue(dict, CFSTR("Status"));
    CFNumberGetValue(CFDictionaryGetValue(dict, CFSTR("PercentComplete")), kCFNumberSInt32Type, &percent);

    printf("[%3d%%] %s\n", (percent / 2) + 50, CFStringGetCStringPtr(status, kCFStringEncodingMacRoman));
}

void fdvendor_callback(CFSocketRef s, CFSocketCallBackType callbackType, CFDataRef address, const void *data, void *info) {
    CFSocketNativeHandle socket = (CFSocketNativeHandle)(*((CFSocketNativeHandle *)data));

    struct msghdr message;
    struct iovec iov[1];
    struct cmsghdr *control_message = NULL;
    char ctrl_buf[CMSG_SPACE(sizeof(int))];
    char dummy_data[1];

    memset(&message, 0, sizeof(struct msghdr));
    memset(ctrl_buf, 0, CMSG_SPACE(sizeof(int)));

    dummy_data[0] = ' ';
    iov[0].iov_base = dummy_data;
    iov[0].iov_len = sizeof(dummy_data);

    message.msg_name = NULL;
    message.msg_namelen = 0;
    message.msg_iov = iov;
    message.msg_iovlen = 1;
    message.msg_controllen = CMSG_SPACE(sizeof(int));
    message.msg_control = ctrl_buf;

    control_message = CMSG_FIRSTHDR(&message);
    control_message->cmsg_level = SOL_SOCKET;
    control_message->cmsg_type = SCM_RIGHTS;
    control_message->cmsg_len = CMSG_LEN(sizeof(int));

    *((int *) CMSG_DATA(control_message)) = gdbfd;

    sendmsg(socket, &message, 0);
    CFSocketInvalidate(s);
    CFRelease(s);
}

CFURLRef copy_device_app_url(AMDeviceRef device, CFStringRef identifier) {
    CFDictionaryRef result;
    assert(AMDeviceLookupApplications(device, 0, &result) == 0);

    CFDictionaryRef app_dict = CFDictionaryGetValue(result, identifier);
    assert(app_dict != NULL);

    CFStringRef app_path = CFDictionaryGetValue(app_dict, CFSTR("Path"));
    assert(app_path != NULL);

    CFURLRef url = CFURLCreateWithFileSystemPath(NULL, app_path, kCFURLPOSIXPathStyle, true);
    CFRelease(result);
    return url;
}

CFStringRef copy_disk_app_identifier(CFURLRef disk_app_url) {
    CFURLRef plist_url = CFURLCreateCopyAppendingPathComponent(NULL, disk_app_url, CFSTR("Info.plist"), false);
    CFReadStreamRef plist_stream = CFReadStreamCreateWithFile(NULL, plist_url);
    CFReadStreamOpen(plist_stream);
    CFPropertyListRef plist = CFPropertyListCreateWithStream(NULL, plist_stream, 0, kCFPropertyListImmutable, NULL, NULL);
    CFStringRef bundle_identifier = CFRetain(CFDictionaryGetValue(plist, CFSTR("CFBundleIdentifier")));
    CFReadStreamClose(plist_stream);

    CFRelease(plist_url);
    CFRelease(plist_stream);
    CFRelease(plist);

    return bundle_identifier;
}

void write_gdb_prep_cmds(AMDeviceRef device) {
    // Get URL for the bundle to install
    CFStringRef path = CFStringCreateWithCString(NULL, app_path, kCFStringEncodingASCII);
    CFURLRef relative_url = CFURLCreateWithFileSystemPath(NULL, path, kCFURLPOSIXPathStyle, false);
    CFURLRef disk_app_url = CFURLCopyAbsoluteURL(relative_url);
    CFRelease(relative_url);

    CFMutableStringRef cmds = NULL;
    if (g_nosource) {
        cmds = CFStringCreateMutableCopy(NULL, 0, GDB_PREP_CMDS_NO_SOURCE);
    } else {
        cmds = CFStringCreateMutableCopy(NULL, 0, GDB_PREP_CMDS);
    }
    CFRange range = { 0, CFStringGetLength(cmds) };

    CFStringRef ds_path = copy_device_support_path(device);
    CFStringFindAndReplace(cmds, CFSTR("{ds_path}"), ds_path, range, 0);
    range.length = CFStringGetLength(cmds);

    if (args) {
        CFStringRef cf_args = CFStringCreateWithCString(NULL, args, kCFStringEncodingASCII);
        CFStringFindAndReplace(cmds, CFSTR("{args}"), cf_args, range, 0);
        CFRelease(cf_args);
    } else {
        CFStringFindAndReplace(cmds, CFSTR(" {args}"), CFSTR(""), range, 0);
    }
    range.length = CFStringGetLength(cmds);

    CFStringRef bundle_identifier = NULL;
    if (g_bundle_identifier) {
        bundle_identifier = CFStringCreateWithCString(NULL, g_bundle_identifier, kCFStringEncodingUTF8);
    } else {
        bundle_identifier = copy_disk_app_identifier(disk_app_url);
    }
    
    CFURLRef device_app_url = copy_device_app_url(device, bundle_identifier);
    CFStringRef device_app_path = CFURLCopyFileSystemPath(device_app_url, kCFURLPOSIXPathStyle);
    CFStringFindAndReplace(cmds, CFSTR("{device_app}"), device_app_path, range, 0);
    range.length = CFStringGetLength(cmds);

    CFStringRef disk_app_path = CFURLCopyFileSystemPath(disk_app_url, kCFURLPOSIXPathStyle);
    CFStringFindAndReplace(cmds, CFSTR("{disk_app}"), disk_app_path, range, 0);
    range.length = CFStringGetLength(cmds);

    CFURLRef device_container_url = CFURLCreateCopyDeletingLastPathComponent(NULL, device_app_url);
    CFStringRef device_container_path = CFURLCopyFileSystemPath(device_container_url, kCFURLPOSIXPathStyle);
    CFMutableStringRef dcp_noprivate = CFStringCreateMutableCopy(NULL, 0, device_container_path);
    range.length = CFStringGetLength(dcp_noprivate);
    CFStringFindAndReplace(dcp_noprivate, CFSTR("/private/var/"), CFSTR("/var/"), range, 0);
    range.length = CFStringGetLength(cmds);
    CFStringFindAndReplace(cmds, CFSTR("{device_container}"), dcp_noprivate, range, 0);
    range.length = CFStringGetLength(cmds);

    CFURLRef disk_container_url = CFURLCreateCopyDeletingLastPathComponent(NULL, disk_app_url);
    CFStringRef disk_container_path = CFURLCopyFileSystemPath(disk_container_url, kCFURLPOSIXPathStyle);
    CFStringFindAndReplace(cmds, CFSTR("{disk_container}"), disk_container_path, range, 0);
    range.length = CFStringGetLength(cmds);
    
    if (g_run) {
        CFStringFindAndReplace(cmds, CFSTR("{run-app}"), CFSTR("detach"), range, 0);
    } else if (g_kill) {
        CFStringFindAndReplace(cmds, CFSTR("{run-app}"), CFSTR("kill"), range, 0);
    } else {
        CFStringFindAndReplace(cmds, CFSTR("{run-app}"), CFSTR("continue"), range, 0);
    }
    
    CFDataRef cmds_data = CFStringCreateExternalRepresentation(NULL, cmds, kCFStringEncodingASCII, 0);
    FILE *out = fopen(PREP_CMDS_PATH, "w");
    fwrite(CFDataGetBytePtr(cmds_data), CFDataGetLength(cmds_data), 1, out);
    fclose(out);

    CFRelease(cmds);
    if (ds_path != NULL) CFRelease(ds_path);
    CFRelease(bundle_identifier);
    CFRelease(device_app_url);
    CFRelease(device_app_path);
    CFRelease(disk_app_path);
    CFRelease(device_container_url);
    CFRelease(device_container_path);
    CFRelease(dcp_noprivate);
    CFRelease(disk_container_url);
    CFRelease(disk_container_path);
    CFRelease(cmds_data);
    CFRelease(disk_app_url);
}

void start_remote_debug_server(AMDeviceRef device) {
    assert(AMDeviceStartService(device, CFSTR("com.apple.debugserver"), &gdbfd, NULL) == 0);

    CFSocketRef fdvendor = CFSocketCreate(NULL, AF_UNIX, 0, 0, kCFSocketAcceptCallBack, &fdvendor_callback, NULL);

    int yes = 1;
    setsockopt(CFSocketGetNative(fdvendor), SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    struct sockaddr_un address;
    memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    strcpy(address.sun_path, FDVENDOR_PATH);
    address.sun_len = SUN_LEN(&address);
    CFDataRef address_data = CFDataCreate(NULL, (const UInt8 *)&address, sizeof(address));

    unlink(FDVENDOR_PATH);

    CFSocketSetAddress(fdvendor, address_data);
    CFRelease(address_data);
    CFRunLoopAddSource(CFRunLoopGetMain(), CFSocketCreateRunLoopSource(NULL, fdvendor, 0), kCFRunLoopCommonModes);
}

void gdb_ready_handler(int signum)
{
	_exit(0);
}

void install_bundle(AMDeviceRef device) {
    // Connect to the device to copy files. Looks like we need to connect to the device anew for
    // each service we use.
    AMDeviceConnect(device);
    assert(AMDeviceIsPaired(device));
    assert(AMDeviceValidatePairing(device) == 0);
    assert(AMDeviceStartSession(device) == 0);

    // Get URL for the bundle to install
    CFStringRef path = CFStringCreateWithCString(NULL, app_path, kCFStringEncodingASCII);
    CFURLRef relative_url = CFURLCreateWithFileSystemPath(NULL, path, kCFURLPOSIXPathStyle, false);
    CFURLRef url = CFURLCopyAbsoluteURL(relative_url);
    CFRelease(relative_url);

    // Start the Apple File Connection service
    int afcFd;
    assert(AMDeviceStartService(device, CFSTR("com.apple.afc"), &afcFd, NULL) == 0);
    assert(AMDeviceStopSession(device) == 0);
    assert(AMDeviceDisconnect(device) == 0);
    // transfer_callback copys the app bundle to the device (?)
    assert(AMDeviceTransferApplication(afcFd, path, NULL, transfer_callback, NULL) == 0);

    close(afcFd);

    // Now we'll install the device on Springboard
    CFStringRef keys[] = { CFSTR("PackageType") };
    CFStringRef values[] = { CFSTR("Developer") };
    CFDictionaryRef options = CFDictionaryCreate(NULL, (const void **)&keys, (const void **)&values, 1, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);

    AMDeviceConnect(device);
    assert(AMDeviceIsPaired(device));
    assert(AMDeviceValidatePairing(device) == 0);
    assert(AMDeviceStartSession(device) == 0);

    int installFd;
    assert(AMDeviceStartService(device, CFSTR("com.apple.mobile.installation_proxy"), &installFd, NULL) == 0);

    assert(AMDeviceStopSession(device) == 0);
    assert(AMDeviceDisconnect(device) == 0);

    mach_error_t result = AMDeviceInstallApplication(installFd, path, options, install_callback, NULL);
    if (result != 0)
    {
        printf("AMDeviceInstallApplication failed: %d\n", result);
        exit(1);
    }

    close(installFd);

    CFRelease(path);
    CFRelease(options);
}

void handle_device(AMDeviceRef device) {
    if (found_device) return; // handle one device only

    CFStringRef found_device_id = AMDeviceCopyDeviceIdentifier(device);

    // If user specified a device, make sure the one we found is the right one.
    if (device_id != NULL) {
        if(strcmp(device_id, CFStringGetCStringPtr(found_device_id, CFStringGetSystemEncoding())) == 0) {
            found_device = true;
        } else {
            return;
        }
    } else {
        found_device = true;
    }

    CFRetain(device); // don't know if this is necessary?

    if (g_install) {
        printf("[  0%%] Found device (%s), beginning install\n", CFStringGetCStringPtr(found_device_id, CFStringGetSystemEncoding()));
        install_bundle(device);
        printf("[100%%] Installed package %s\n", app_path);
    }
    
    if (!debug && !g_run && !g_kill) exit(0); // no debug phase

    AMDeviceConnect(device);
    assert(AMDeviceIsPaired(device));
    assert(AMDeviceValidatePairing(device) == 0);
    assert(AMDeviceStartSession(device) == 0);

    printf("------ Debug phase ------\n");

    mount_developer_image(device);      // put debugserver on the device
    start_remote_debug_server(device);  // start debugserver
    write_gdb_prep_cmds(device);   // dump the necessary gdb commands into a file
    
    printf("[100%%] Connecting to remote debug server\n");
    printf("-------------------------\n");

    signal(SIGHUP, gdb_ready_handler);

    pid_t parent = getpid();
    int pid = fork();
    if (pid == 0) {
        CFStringRef path = copy_xcode_path_for(CFSTR("Platforms/iPhoneOS.platform/Developer/usr/libexec/gdb/gdb-arm-apple-darwin"));
        if (path == NULL) {
            printf("[ !! ] Unable to locate GDB.\n");
            exit(1);
        } else {
            CFStringRef gdb_cmd = CFStringCreateWithFormat(NULL, NULL, CFSTR("%@ %@"), path, CFSTR(GDB_SHELL_ARGS));
            
            // Convert CFStringRef to char* for system call
            const char *char_gdb_cmd = CFStringGetCStringPtr(gdb_cmd, kCFStringEncodingMacRoman);
            
            system(char_gdb_cmd);      // launch gdb
        }

        kill(parent, SIGHUP);  // "No. I am your father."
        _exit(0);
    }
}

void device_callback(struct am_device_notification_callback_info *info, void *arg) {
    switch (info->msg) {
        case ADNCI_MSG_CONNECTED:
            handle_device(info->dev);
        default:
            break;
    }
}

void timeout_callback(CFRunLoopTimerRef timer, void *info) {
    if (!found_device) {
        printf("Timed out waiting for device.\n");
        exit(1);
    }
}

void usage(const char* app) {
    printf("usage: %s [-d/--debug] [-i/--id device_id] -b/--bundle bundle.app [-a/--args arguments] [-t/--timeout timeout(seconds)]\n", app);
}

int main(int argc, char *argv[]) {
    static struct option longopts[] = {
        { "debug", no_argument, NULL, 'd' },
        { "run", no_argument, NULL, 'r' },
        { "id", required_argument, NULL, 'i' },
        { "bundle", required_argument, NULL, 'b' },
        { "bundle-identifier", required_argument, NULL, 'B' },
        { "args", required_argument, NULL, 'a' },
        { "verbose", no_argument, NULL, 'v' },
        { "timeout", required_argument, NULL, 't' },
        { "no-install", no_argument, NULL, 'N' },
        { "no-source", no_argument, NULL, 'S' },
        { "kill", no_argument, NULL, 'k' },
        { NULL, 0, NULL, 0 },
    };
    char ch;

    while ((ch = getopt_long(argc, argv, "drkvi:b:a:t:B:", longopts, NULL)) != -1)
    {
        switch (ch) {
        case 'd':
            debug = 1;
            break;
        case 'r':
            debug = 0;
            g_run = true;
            break;
        case 'k':
            debug = 0;
            g_run = false;
            g_kill = true;
            break;
        case 'i':
            device_id = optarg;
            break;
        case 'b':
            app_path = optarg;
            break;
        case 'a':
            args = optarg;
            break;
        case 'v':
            verbose = 1;
            break;
        case 't':
            timeout = atoi(optarg);
            break;
        case 'N':
            g_install = false;
            break;
        case 'B':
            g_bundle_identifier = optarg;
            break;
        case 'S':
            g_nosource = true;
            break;
        default:
            usage(argv[0]);
            return 1;
        }
    }

    if ((g_install && !app_path) && !g_bundle_identifier) {
        usage(argv[0]);
        exit(0);
    }

    if (g_install) {
        printf("------ Install phase ------\n");

        assert(access(app_path, F_OK) == 0);
    }
    
    AMDSetLogLevel(5); // otherwise syslog gets flooded with crap
    if (timeout > 0)
    {
        CFRunLoopTimerRef timer = CFRunLoopTimerCreate(NULL, CFAbsoluteTimeGetCurrent() + timeout, 0, 0, 0, timeout_callback, NULL);
        CFRunLoopAddTimer(CFRunLoopGetCurrent(), timer, kCFRunLoopCommonModes);
        printf("[....] Waiting up to %d seconds for iOS device to be connected\n", timeout);
    }
    else
    {
        printf("[....] Waiting for iOS device to be connected\n");
    }

    struct am_device_notification *notify;
    AMDeviceNotificationSubscribe(&device_callback, 0, 0, NULL, &notify);
    CFRunLoopRun();
}
