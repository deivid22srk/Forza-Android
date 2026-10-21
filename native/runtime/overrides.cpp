// Kernel exports the XboxXBE-Recomp runtime does not implement yet.
//
// Forza's XBE imports three ordinals the SDK registers no handler for, and the
// dispatcher aborts the guest when it hits one. Everything else is left to the
// SDK runtime, so this file is only the Android-side gap filler.
//
// The registrations below run as static initialisers, before kernel::Boot, and
// RegisterExport overwrites whatever was there. Do not implement behaviour the
// SDK already has here: keep overrides minimal and report them in docs/GAPS.md.

#include <cstring>

#include "kernel/exports.h"
#include "kernel/kernel.h"

namespace kernel {

// 76 IoQueryVolumeInformation(PFILE_OBJECT FileObject, FS_INFORMATION_CLASS Class, ULONG Length,
//                             PVOID FsInformation, PULONG ReturnedLength)
//
// XAPI calls this after turning a file handle into a FILE_OBJECT pointer with
// ObReferenceObjectByHandle. The SDK only implements the handle-based
// NtQueryVolumeInformationFile, so the layouts are mirrored here. The boot
// path asks for class 5 and looks for "FATX" in the answer; unknown classes
// are reported instead of trapping the guest. The file object is ignored the
// same way NtQueryVolumeInformationFile reports one volume for every handle.
KERNEL_EXPORT(76, IoQueryVolumeInformation, 5) {
    uint32_t cls = a[1], len = a[2], out = a[3], returned = a[4];
    if (!out) return kInvalidParameter;

    uint32_t produced = 0;
    switch (cls) {
        case 3:  // FileFsSizeInformation: 16 GiB total, 8 GiB free in 16 KiB units
            if (len < 0x18) return kBufferTooSmall;
            std::memset(HostPtr(out), 0, len);
            W64(out, 0x100000);
            W64(out + 8, 0x80000);
            W32(out + 0x10, 32);
            W32(out + 0x14, 512);
            produced = 0x18;
            break;
        case 7:  // FileFsFullSizeInformation
            if (len < 0x20) return kBufferTooSmall;
            std::memset(HostPtr(out), 0, len);
            W64(out, 0x100000);
            W64(out + 8, 0x80000);
            W64(out + 0x10, 0x80000);
            W32(out + 0x18, 32);
            W32(out + 0x1C, 512);
            produced = 0x20;
            break;
        case 1:  // FileFsVolumeInformation
            if (len < 0x18) return kBufferTooSmall;
            std::memset(HostPtr(out), 0, len);
            W32(out + 8, 0x12345678);  // VolumeSerialNumber
            produced = 0x18;
            break;
        case 5:  // FileFsAttributeInformation: attributes, max name, name length, "FATX"
            if (len < 16) return kBufferTooSmall;
            std::memset(HostPtr(out), 0, len);
            W32(out, 0);
            W32(out + 4, 42);
            W32(out + 8, 4);
            std::memcpy(HostPtr(out + 12), "FATX", 4);
            produced = 16;
            break;
        default:
            LOG_FIRST(8, "IoQueryVolumeInformation: unhandled class %u for file object %08X", cls, a[0]);
            return kInvalidParameter;
    }
    if (returned) W32(returned, produced);
    return kSuccess;
}

// 232 NtUserIoApcDispatcher(PVOID ApcContext, PIO_STATUS_BLOCK IoStatusBlock, ULONG Reserved)
//
// The routine the kernel runs to deliver an overlapped IO completion. The host
// file layer completes requests synchronously, so this never has real work; it
// only needs to exist so an import that is never taken cannot abort the guest.
KERNEL_EXPORT(232, NtUserIoApcDispatcher, 3) {
    LOG_FIRST(4, "NtUserIoApcDispatcher: context %08X iosb %08X reserved %u", a[0], a[1], a[2]);
    return 0;
}

// 343 XcPKGetKeyLen(PUCHAR pbPubKey)
//
// Length of the Xbox public RSA key in bits. Forza only reaches it through the
// offline-disc signature checks, and the SDK already answers those positively
// (XcVerifyPKCS1Signature returns true), so report the console's 2048-bit key.
KERNEL_EXPORT(343, XcPKGetKeyLen, 1) {
    LOG_FIRST(4, "XcPKGetKeyLen: key %08X", a[0]);
    return 0x800;
}

}  // namespace kernel
