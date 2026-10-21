// Kernel exports the XboxXBE-Recomp runtime does not implement, or implements
// in a way the game's XAPI code cannot survive.
//
// Forza's XBE imports 159 ordinals; three had no SDK handler, and one handler
// returned values that made XAPI dereference a bogus pointer. Everything else
// is left to the SDK runtime, so this file is only the Android-side gap filler.
//
// The registrations below run as static initialisers, before kernel::Boot, and
// RegisterExport overwrites whatever was there. Do not implement behaviour the
// SDK already has here: keep overrides minimal and report them in docs/GAPS.md.

#include <cstring>

#include "kernel/exports.h"
#include "kernel/kernel.h"

namespace kernel {

namespace {

// A stable, non-zero stand-in for the retail disc's media ID. XAPI returns it
// as the volume identity of D:, and the game compares it between runs (disc
// change detection), so it must never vary. Forza's title ID fills the role.
constexpr uint32_t kMediaId = 0x4D53006E;

// FILE_OBJECT body the game can safely dereference. XAPI's volume/media ID
// helper (XAPILIB code at 0x267489) reads FILE_OBJECT->FsContext right after
// ObReferenceObjectByHandle, and the FATX/GDFX branches read from the context
// itself. The SDK's default body would hold the dispatcher stamp (a small
// serial) at the FsContext offset, which faults the guest.
uint32_t BuildFileBody() {
    uint32_t body = mem::AllocSystem(0x48);  // sizeof(Xbox FILE_OBJECT)
    std::memset(HostPtr(body), 0, 0x48);
    W16(body, 5);         // Type: IO_TYPE_FILE (NT io manager, as on Xbox)
    W16(body + 2, 0x36);  // ReadAccess|WriteAccess|SharedRead|SharedWrite
    W32(body + 4, 0);     // DeviceObject
    // FsContext: a fake GDFX volume descriptor. The GDFX branch reads the
    // media ID at [0]; the FATX branch tests bit 0 of byte 0 (0x6E is even,
    // so it takes the [0x1C] serial read instead) - both stay in the block.
    uint32_t ctx = mem::AllocSystem(0x40);
    std::memset(HostPtr(ctx), 0, 0x40);
    W32(ctx, kMediaId);
    W32(ctx + 0x1C, kMediaId);
    W32(body + 8, ctx);  // FsContext
    W32(body + 0xC, 0);  // FsContext2
    return body;
}

}  // namespace

// 246 ObReferenceObjectByHandle(HANDLE Handle, PVOID ObjectType, PVOID* Object)
//
// Same as the SDK for dispatcher objects: a 32-byte body stamped for
// FromPointer. Files get a real FILE_OBJECT body instead, because XAPI reads
// FsContext from it and the stamp would sit exactly on that field.
KERNEL_EXPORT(246, ObReferenceObjectByHandle, 3) {
    uint32_t h = a[0];
    ObjectRef o = h == 0xFFFFFFFE ? CurrentRef() : FromHandle(h);
    if (!o) return kInvalidHandle;
    if (!o->guest) {
        if (o->type == ObjType::File) {
            o->guest = BuildFileBody();
        } else {
            uint32_t body = mem::AllocSystem(32);
            Bind(o, body);
        }
    }
    W32(a[2], o->guest);
    return kSuccess;
}

// 76 IoQueryVolumeInformation(PFILE_OBJECT FileObject, FS_INFORMATION_CLASS Class, ULONG Length,
//                             PVOID FsInformation, PULONG ReturnedLength)
//
// XAPI calls this after turning a file handle into a FILE_OBJECT pointer with
// ObReferenceObjectByHandle. The SDK only implements the handle-based
// NtQueryVolumeInformationFile, so the layouts are mirrored here.
//
// Class 5 must answer "GDFX", the Xbox game disc filesystem: on the console
// D: is an XDFS volume, and XAPI's media ID helper reads the ID from
// FsContext[0] only on that branch. "FATX" would send it to the hard disk
// branch instead. Unknown classes are reported instead of trapping the guest.
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
        case 5:  // FileFsAttributeInformation: attributes, max name, name length, "GDFX"
            if (len < 16) return kBufferTooSmall;
            std::memset(HostPtr(out), 0, len);
            W32(out, 0);
            W32(out + 4, 42);
            W32(out + 8, 4);
            std::memcpy(HostPtr(out + 12), "GDFX", 4);
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
