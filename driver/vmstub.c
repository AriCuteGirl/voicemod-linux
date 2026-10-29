/*
 * vmstub.sys - stand-in for Voicemod's virtual audio kernel driver under Wine.
 *
 * Voicemod's VADIPC client opens \\.\VMDriver (or \\.\voicemodvad), registers an
 * event with IOCTL 0x1d2007, polls xrun counters and sends 0x1d200b/0f/13 on stop.
 * The real audio path goes through a PipeWire null sink, so this driver only has to
 * accept those requests: every IOCTL succeeds and returns a zero-filled output buffer.
 */
#include <ddk/ntddk.h>

static const WCHAR *names[][2] = {
    { L"\\Device\\VMDriver",    L"\\DosDevices\\VMDriver" },
    { L"\\Device\\voicemodvad", L"\\DosDevices\\voicemodvad" },
};
#define NAME_COUNT (sizeof(names) / sizeof(names[0]))

static PDEVICE_OBJECT devices[NAME_COUNT];

static NTSTATUS complete(PIRP irp, NTSTATUS status, ULONG_PTR info)
{
    irp->IoStatus.Status = status;
    irp->IoStatus.Information = info;
    IoCompleteRequest(irp, IO_NO_INCREMENT);
    return status;
}

static NTSTATUS WINAPI dispatch_open_close(PDEVICE_OBJECT dev, PIRP irp)
{
    (void)dev;
    return complete(irp, STATUS_SUCCESS, 0);
}

static NTSTATUS WINAPI dispatch_ioctl(PDEVICE_OBJECT dev, PIRP irp)
{
    PIO_STACK_LOCATION stack = IoGetCurrentIrpStackLocation(irp);
    ULONG code = stack->Parameters.DeviceIoControl.IoControlCode;
    ULONG out_len = stack->Parameters.DeviceIoControl.OutputBufferLength;
    void *out = NULL;
    (void)dev;

    switch (code & 3)
    {
    case METHOD_BUFFERED:   out = irp->AssociatedIrp.SystemBuffer; break;
    case METHOD_NEITHER:    out = irp->UserBuffer; break;
    default:                out = irp->MdlAddress ? MmGetSystemAddressForMdlSafe(irp->MdlAddress, NormalPagePriority) : NULL; break;
    }
    if (out && out_len) RtlZeroMemory(out, out_len);
    else out_len = 0;

    DbgPrint("vmstub: ioctl %#lx in=%lu out=%lu\n", code,
             stack->Parameters.DeviceIoControl.InputBufferLength, out_len);
    return complete(irp, STATUS_SUCCESS, out_len);
}

static VOID WINAPI driver_unload(PDRIVER_OBJECT driver)
{
    UNICODE_STRING link;
    unsigned int i;
    (void)driver;

    for (i = 0; i < NAME_COUNT; i++)
    {
        if (!devices[i]) continue;
        RtlInitUnicodeString(&link, names[i][1]);
        IoDeleteSymbolicLink(&link);
        IoDeleteDevice(devices[i]);
    }
}

NTSTATUS WINAPI DriverEntry(PDRIVER_OBJECT driver, PUNICODE_STRING path)
{
    UNICODE_STRING name, link;
    NTSTATUS status;
    unsigned int i;
    (void)path;

    driver->MajorFunction[IRP_MJ_CREATE] = dispatch_open_close;
    driver->MajorFunction[IRP_MJ_CLEANUP] = dispatch_open_close;
    driver->MajorFunction[IRP_MJ_CLOSE] = dispatch_open_close;
    driver->MajorFunction[IRP_MJ_DEVICE_CONTROL] = dispatch_ioctl;
    driver->DriverUnload = driver_unload;

    for (i = 0; i < NAME_COUNT; i++)
    {
        RtlInitUnicodeString(&name, names[i][0]);
        RtlInitUnicodeString(&link, names[i][1]);
        status = IoCreateDevice(driver, 0, &name, FILE_DEVICE_UNKNOWN, 0, FALSE, &devices[i]);
        if (status) return status;
        status = IoCreateSymbolicLink(&link, &name);
        if (status) return status;
    }
    DbgPrint("vmstub: loaded\n");
    return STATUS_SUCCESS;
}
