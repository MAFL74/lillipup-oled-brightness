#include <ntifs.h>
#include <ntstrsafe.h>
#include <wdmsec.h>
#include <initguid.h>
#include <wdmguid.h>
#include <dispmprt.h>
#include <ntddvdeo.h>
#include <acpiioct.h>

#define DEVICE_NAME     L"\\Device\\LillipupAuxProbe"
#define SYMBOLIC_NAME   L"\\DosDevices\\LillipupAuxProbe"
#define FILE_DEVICE_LILLIPUP_AUX 0x8000
#define IOCTL_AUX_STATUS CTL_CODE(FILE_DEVICE_LILLIPUP_AUX, 0x800, METHOD_BUFFERED, FILE_READ_ACCESS)
#define IOCTL_AUX_GET_DP_ADDRESS CTL_CODE(FILE_DEVICE_LILLIPUP_AUX, 0x801, METHOD_BUFFERED, FILE_READ_ACCESS)
#define IOCTL_AUX_READ_DPCD CTL_CODE(FILE_DEVICE_LILLIPUP_AUX, 0x802, METHOD_BUFFERED, FILE_READ_ACCESS)
// Device ACL below is administrator/SYSTEM only. Use read-access encoding
// so Windows' device-control gate accepts the same handle used for probing.
#define IOCTL_AUX_WRITE_BACKLIGHT_DPCD CTL_CODE(FILE_DEVICE_LILLIPUP_AUX, 0x803, METHOD_BUFFERED, FILE_READ_ACCESS)
#define IOCTL_AUX_DUMP_OPREGION CTL_CODE(FILE_DEVICE_LILLIPUP_AUX, 0x804, METHOD_BUFFERED, FILE_READ_ACCESS)
#define IOCTL_AUX_ACPI_QUERY_XBQC CTL_CODE(FILE_DEVICE_LILLIPUP_AUX, 0x805, METHOD_BUFFERED, FILE_READ_ACCESS)
#define IOCTL_AUX_ACPI_SET_XBCM CTL_CODE(FILE_DEVICE_LILLIPUP_AUX, 0x806, METHOD_BUFFERED, FILE_READ_ACCESS)
#define IOCTL_AUX_READ_GPU_MMIO CTL_CODE(FILE_DEVICE_LILLIPUP_AUX, 0x807, METHOD_BUFFERED, FILE_READ_ACCESS)
#define IOCTL_AUX_READ_DPCD_MMIO CTL_CODE(FILE_DEVICE_LILLIPUP_AUX, 0x808, METHOD_BUFFERED, FILE_READ_ACCESS)
#define IOCTL_AUX_SET_NATIVE_BRIGHTNESS CTL_CODE(FILE_DEVICE_LILLIPUP_AUX, 0x809, METHOD_BUFFERED, FILE_READ_ACCESS)
#define MAX_PROBE_BYTES 16

#define LILLIPUP_AUX_POOL_TAG 'pAUL'

static PDEVICE_OBJECT g_DeviceObject = NULL;
static FAST_MUTEX g_AuxMmioMutex;
static BOOLEAN g_SymbolicLinkCreated = FALSE;
static BOOLEAN g_DpInterfaceReferenced = FALSE;
static PFILE_OBJECT g_VideoFileObject = NULL;
static PDEVICE_OBJECT g_VideoDeviceObject = NULL;
static PDEVICE_OBJECT g_TopVideoDeviceObject = NULL;
static DXGK_DP_INTERFACE g_DpInterface = {0};
static NTSTATUS g_QueryStatus = STATUS_DEVICE_NOT_READY;
static NTSTATUS g_InterfaceEnumerationStatus = STATUS_NOT_FOUND;
static NTSTATUS g_LastOpenStatus = STATUS_NOT_FOUND;
static NTSTATUS g_LastQueryInterfaceStatus = STATUS_NOT_FOUND;
static ULONG g_InterfaceCount = 0;
static NTSTATUS g_LastDpcapsStatus = STATUS_NOT_FOUND;
static ULONG g_InterfaceFunctionMask = 0;
static DXGKARG_QUERYDPCAPS g_DpCaps = {0};

static const GUID g_DeviceClassGuid = { 0x536ddb43, 0x4a97, 0x4d3c, {0x9e,0x43,0x6d,0x1c,0x4f,0x33,0x82,0xb1} };

#pragma pack(push, 1)
typedef struct _AUX_STATUS_OUT {
    NTSTATUS QueryStatus;
    NTSTATUS EnumerationStatus;
    NTSTATUS LastOpenStatus;
    NTSTATUS LastQueryInterfaceStatus;
    USHORT InterfaceVersion;
    USHORT InterfaceSize;
    UINT NumRootPorts;
    UINT InterfaceCount;
    BYTE DPVersionMajor;
    BYTE DPVersionMinor;
    USHORT Reserved;
    NTSTATUS LastDpcapsStatus;
    UINT InterfaceFunctionMask;
} AUX_STATUS_OUT, *PAUX_STATUS_OUT;

typedef struct _AUX_TARGET_IN {
    D3DDDI_VIDEO_PRESENT_TARGET_ID TargetId;
} AUX_TARGET_IN, *PAUX_TARGET_IN;

typedef struct _AUX_TARGET_OUT {
    NTSTATUS Status;
    UINT NativeError;
    UINT RootPortIndex;
    BYTE NumLinks;
    BYTE RelativeAddress[MAX_DP_ADDRESS_SIZE];
} AUX_TARGET_OUT, *PAUX_TARGET_OUT;

typedef struct _AUX_READ_IN {
    UINT RootPortIndex;
    UINT DpcdAddress;
    BYTE Length;
    BYTE Reserved[3];
} AUX_READ_IN, *PAUX_READ_IN;

typedef struct _AUX_READ_OUT {
    NTSTATUS Status;
    UINT NativeError;
    BYTE BytesDone;
    BYTE Data[MAX_PROBE_BYTES];
} AUX_READ_OUT, *PAUX_READ_OUT;

typedef struct _AUX_WRITE_IN {
    UINT RootPortIndex;
    UINT DpcdAddress;
    BYTE Length;
    BYTE Reserved[3];
    BYTE Data[MAX_PROBE_BYTES];
} AUX_WRITE_IN, *PAUX_WRITE_IN;

typedef struct _AUX_WRITE_OUT {
    NTSTATUS Status;
    UINT NativeError;
    BYTE BytesDone;
    BYTE Reserved[3];
} AUX_WRITE_OUT, *PAUX_WRITE_OUT;

typedef struct _AUX_OPREGION_OUT {
    NTSTATUS Status;
    ULONG AslsAddress;
    ULONG OpRegionSize;
    ULONG BytesCopied;
    ULONG VbtOffset;
    ULONG PciBytes;
    USHORT VendorId;
    USHORT DeviceId;
    BYTE Data[1];
} AUX_OPREGION_OUT, *PAUX_OPREGION_OUT;

typedef struct _AUX_ACPI_QUERY_OUT {
    NTSTATUS TransportStatus;
    ULONG AcpiStackFound;
    ULONG ArgumentCount;
    ULONG ArgumentType;
    ULONG ArgumentLength;
    ULONGLONG BrightnessValue;
} AUX_ACPI_QUERY_OUT, *PAUX_ACPI_QUERY_OUT;

typedef struct _AUX_ACPI_SET_OUT {
    NTSTATUS TransportStatus;
    ULONG RequestedBrightness;
    NTSTATUS ReadbackStatus;
    ULONG ReadbackCount;
    ULONG ReadbackType;
    ULONG ReadbackLength;
    ULONGLONG ReadbackBrightness;
} AUX_ACPI_SET_OUT, *PAUX_ACPI_SET_OUT;

typedef struct _AUX_MMIO_OUT {
    NTSTATUS Status;
    ULONG RegisterValues[6];
} AUX_MMIO_OUT, *PAUX_MMIO_OUT;

typedef struct _AUX_MMIO_DPCD_IN {
    ULONG Address;
    ULONG Length;
} AUX_MMIO_DPCD_IN, *PAUX_MMIO_DPCD_IN;

typedef struct _AUX_MMIO_DPCD_OUT {
    NTSTATUS Status;
    ULONG HardwareStatus;
    ULONG DataLength;
    UCHAR Reply;
    UCHAR Data[16];
} AUX_MMIO_DPCD_OUT, *PAUX_MMIO_DPCD_OUT;

typedef struct _AUX_NATIVE_BRIGHTNESS_OUT {
    NTSTATUS Status;
    ULONG RequestedPercent;
    ULONG MaximumLevel;
    ULONG AppliedLevel;
    NTSTATUS ReadbackStatus;
    ULONG ReadbackBrightness;
    UCHAR BacklightEnable;
    UCHAR BacklightMode;
    UCHAR BrightnessMsb;
    UCHAR BrightnessLsb;
} AUX_NATIVE_BRIGHTNESS_OUT, *PAUX_NATIVE_BRIGHTNESS_OUT;
#pragma pack(pop)

static PDEVICE_OBJECT FindGpuAcpiFilterDevice(VOID)
{
    PDEVICE_OBJECT base, current, deviceObject;
    PFILE_OBJECT fileObject;
    PDEVICE_OBJECT found = NULL;
    PWSTR interfaceList = NULL, interfaceName;
    UNICODE_STRING acpiDriverName;
    UNICODE_STRING interfaceString;
    NTSTATUS status;

    RtlInitUnicodeString(&acpiDriverName, L"\\Driver\\ACPI");
    status = IoGetDeviceInterfaces(&GUID_DEVINTERFACE_DISPLAY_ADAPTER, NULL, 0, &interfaceList);
    if (NT_SUCCESS(status) && interfaceList != NULL) {
        for (interfaceName = interfaceList; *interfaceName != UNICODE_NULL; ) {
            PWSTR end = interfaceName;
            while (*end != UNICODE_NULL) ++end;
            RtlInitUnicodeString(&interfaceString, interfaceName);
            status = IoGetDeviceObjectPointer(&interfaceString, FILE_READ_ATTRIBUTES, &fileObject, &deviceObject);
            interfaceName = end + 1;
            if (!NT_SUCCESS(status)) continue;

            base = IoGetDeviceAttachmentBaseRef(deviceObject);
            for (current = base; current != NULL; current = current->AttachedDevice) {
                if (current->DriverObject != NULL &&
                    RtlEqualUnicodeString(&current->DriverObject->DriverName, &acpiDriverName, TRUE)) {
                    ObReferenceObject(current);
                    found = current;
                    break;
                }
            }
            if (base != NULL) ObDereferenceObject(base);
            ObDereferenceObject(fileObject);
            if (found != NULL) break;
        }
        ExFreePool(interfaceList);
    }

    // If the adapter interface does not expose an ACPI filter, fall back to
    // the monitor stack. This can still answer panel-scoped methods such as _BQC.
    if (found == NULL && g_VideoDeviceObject != NULL) {
        base = IoGetDeviceAttachmentBaseRef(g_VideoDeviceObject);
        for (current = base; current != NULL; current = current->AttachedDevice) {
            if (current->DriverObject != NULL &&
                RtlEqualUnicodeString(&current->DriverObject->DriverName, &acpiDriverName, TRUE)) {
                ObReferenceObject(current);
                found = current;
                break;
            }
        }
        if (base != NULL) ObDereferenceObject(base);
    }
    return found;
}

static NTSTATUS QueryFirmwareBrightness(ULONG PathIndex, ULONGLONG *Value, ULONG *Count, ULONG *Type, ULONG *Length)
{
    static const CHAR *methodPaths[] = {
        "GFX0.XBQC",
        "LCD0._BQC",
        "GFX0.LCD0._BQC",
        "_SB.PCI0.GFX0.XBQC",
        "_BQC",
        "\\_SB.PCI0.GFX0.XBQC",
        "\\_SB.PCI0.GFX0.LCD0._BQC",
        "\\_SB.PCI0.GFX0.LCD0._BCL"
    };
    PDEVICE_OBJECT acpiDevice;
    ACPI_EVAL_INPUT_BUFFER_V1_EX input;
    UCHAR outputStorage[128];
    PACPI_EVAL_OUTPUT_BUFFER output = (PACPI_EVAL_OUTPUT_BUFFER)outputStorage;
    IO_STATUS_BLOCK ioStatus;
    KEVENT event;
    PIRP irp;
    NTSTATUS status;

    *Value = 0;
    *Count = 0;
    *Type = 0;
    *Length = 0;
    if (PathIndex >= RTL_NUMBER_OF(methodPaths)) return STATUS_INVALID_PARAMETER;
    acpiDevice = FindGpuAcpiFilterDevice();
    if (acpiDevice == NULL) return STATUS_NOT_FOUND;

    RtlZeroMemory(&input, sizeof(input));
    input.Signature = ACPI_EVAL_INPUT_BUFFER_SIGNATURE_EX;
    status = RtlStringCchCopyA(input.MethodName, RTL_NUMBER_OF(input.MethodName), methodPaths[PathIndex]);
    if (!NT_SUCCESS(status)) {
        ObDereferenceObject(acpiDevice);
        return status;
    }

    RtlZeroMemory(outputStorage, sizeof(outputStorage));
    KeInitializeEvent(&event, NotificationEvent, FALSE);
    irp = IoBuildDeviceIoControlRequest(
        IOCTL_ACPI_EVAL_METHOD_EX,
        acpiDevice,
        &input,
        sizeof(input),
        outputStorage,
        sizeof(outputStorage),
        FALSE,
        &event,
        &ioStatus);
    if (irp == NULL) {
        ObDereferenceObject(acpiDevice);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    status = IoCallDriver(acpiDevice, irp);
    if (status == STATUS_PENDING) {
        KeWaitForSingleObject(&event, Executive, KernelMode, FALSE, NULL);
        status = ioStatus.Status;
    }
    if (NT_SUCCESS(status)) {
        *Count = output->Count;
        if (output->Count >= 1) {
            PACPI_METHOD_ARGUMENT argument = &output->Argument[0];
            *Type = argument->Type;
            *Length = argument->DataLength;
            if (argument->Type == ACPI_METHOD_ARGUMENT_INTEGER && argument->DataLength == sizeof(ULONGLONG))
                RtlCopyMemory(Value, argument->Data, sizeof(ULONGLONG));
            else
                status = STATUS_DATA_ERROR;
        } else {
            status = STATUS_DATA_ERROR;
        }
    }

    ObDereferenceObject(acpiDevice);
    return status;
}

static NTSTATUS SetFirmwareBrightness(ULONG Brightness)
{
    PDEVICE_OBJECT acpiDevice;
    ACPI_EVAL_INPUT_BUFFER_SIMPLE_INTEGER_V1_EX input;
    UCHAR outputStorage[128];
    IO_STATUS_BLOCK ioStatus;
    KEVENT event;
    PIRP irp;
    NTSTATUS status;

    if (Brightness > 100) return STATUS_INVALID_PARAMETER;
    acpiDevice = FindGpuAcpiFilterDevice();
    if (acpiDevice == NULL) return STATUS_NOT_FOUND;

    RtlZeroMemory(&input, sizeof(input));
    input.Signature = ACPI_EVAL_INPUT_BUFFER_SIMPLE_INTEGER_SIGNATURE_V1_EX;
    status = RtlStringCchCopyA(input.MethodName, RTL_NUMBER_OF(input.MethodName), "\\_SB.PCI0.GFX0.XBCM");
    if (!NT_SUCCESS(status)) {
        ObDereferenceObject(acpiDevice);
        return status;
    }
    input.IntegerArgument = Brightness;

    RtlZeroMemory(outputStorage, sizeof(outputStorage));
    KeInitializeEvent(&event, NotificationEvent, FALSE);
    irp = IoBuildDeviceIoControlRequest(
        IOCTL_ACPI_EVAL_METHOD_EX,
        acpiDevice,
        &input,
        sizeof(input),
        outputStorage,
        sizeof(outputStorage),
        FALSE,
        &event,
        &ioStatus);
    if (irp == NULL) {
        ObDereferenceObject(acpiDevice);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    status = IoCallDriver(acpiDevice, irp);
    if (status == STATUS_PENDING) {
        KeWaitForSingleObject(&event, Executive, KernelMode, FALSE, NULL);
        status = ioStatus.Status;
    }
    ObDereferenceObject(acpiDevice);
    return status;
}

static ULONG PackAuxBytes(const UCHAR *Bytes, ULONG Count)
{
    ULONG value = 0;
    ULONG index;
    if (Count > 4) Count = 4;
    for (index = 0; index < Count; ++index)
        value |= ((ULONG)Bytes[index]) << ((3 - index) * 8);
    return value;
}

static VOID UnpackAuxBytes(ULONG Value, UCHAR *Bytes, ULONG Count)
{
    ULONG index;
    if (Count > 4) Count = 4;
    for (index = 0; index < Count; ++index)
        Bytes[index] = (UCHAR)(Value >> ((3 - index) * 8));
}

static NTSTATUS ReadDpcdViaAuxMmio(ULONG Address, ULONG Length, PAUX_MMIO_DPCD_OUT Out)
{
    PHYSICAL_ADDRESS physicalAddress;
    PUCHAR mapped;
    volatile ULONG *control;
    volatile ULONG *data;
    UCHAR request[4];
    UCHAR response[20];
    ULONG sendControl, hardwareStatus, receiveLength, offset, index;
    LARGE_INTEGER interval;
    NTSTATUS status = STATUS_SUCCESS;

    RtlZeroMemory(Out, sizeof(*Out));
    if (Address > 0xFFFFF || Length == 0 || Length > sizeof(Out->Data))
        return STATUS_INVALID_PARAMETER;

    // Port A is the internal eDP AUX channel on this Tiger Lake Lillipup.
    physicalAddress.QuadPart = 0x90000000;
    mapped = MmMapIoSpace(physicalAddress, 0x100000, MmNonCached);
    if (mapped == NULL) return STATUS_INSUFFICIENT_RESOURCES;
    control = (volatile ULONG *)(mapped + 0x64010);
    data = (volatile ULONG *)(mapped + 0x64014);

    hardwareStatus = READ_REGISTER_ULONG((PULONG)control);
    if ((hardwareStatus & 0x80000000) != 0) {
        status = STATUS_DEVICE_BUSY;
        goto Exit;
    }

    request[0] = (UCHAR)(0x90 | ((Address >> 16) & 0x0F)); // native DPCD read
    request[1] = (UCHAR)(Address >> 8);
    request[2] = (UCHAR)Address;
    request[3] = (UCHAR)(Length - 1);
    WRITE_REGISTER_ULONG((PULONG)&data[0], PackAuxBytes(request, sizeof(request)));

    // Tiger Lake AUX transaction setup: 600 us maximum timeout, 18 us FW
    // sync pulse, and 32 us sync pulse. Clear stale W1C flags while starting.
    sendControl = 0x80000000 | 0x40000000 | 0x20000000 | 0x10000000 |
        0x0C000000 | 0x02000000 | (sizeof(request) << 20) | 0x23F;
    WRITE_REGISTER_ULONG((PULONG)control, sendControl);

    interval.QuadPart = -500; // 50 microseconds
    for (index = 0; index < 200; ++index) {
        hardwareStatus = READ_REGISTER_ULONG((PULONG)control);
        if ((hardwareStatus & 0x80000000) == 0) break;
        KeDelayExecutionThread(KernelMode, FALSE, &interval);
    }
    Out->HardwareStatus = hardwareStatus;
    if ((hardwareStatus & 0x80000000) != 0) {
        status = STATUS_IO_TIMEOUT;
        goto ClearStatus;
    }
    if ((hardwareStatus & 0x10000000) != 0) {
        status = STATUS_IO_TIMEOUT;
        goto ClearStatus;
    }
    if ((hardwareStatus & 0x02000000) != 0) {
        status = STATUS_IO_DEVICE_ERROR;
        goto ClearStatus;
    }
    if ((hardwareStatus & 0x40000000) == 0) {
        status = STATUS_UNSUCCESSFUL;
        goto ClearStatus;
    }

    receiveLength = (hardwareStatus >> 20) & 0x1F;
    if (receiveLength > sizeof(response) || receiveLength < Length + 1) {
        status = STATUS_DATA_ERROR;
        goto ClearStatus;
    }
    for (offset = 0; offset < receiveLength; offset += 4)
        UnpackAuxBytes(READ_REGISTER_ULONG((PULONG)&data[offset / 4]), response + offset,
            min(4u, receiveLength - offset));
    Out->Reply = response[0] >> 4;
    Out->DataLength = receiveLength - 1;
    if (Out->Reply != 0) {
        status = STATUS_DEVICE_PROTOCOL_ERROR;
        goto ClearStatus;
    }
    RtlCopyMemory(Out->Data, response + 1, min((ULONG)Length, Out->DataLength));

ClearStatus:
    WRITE_REGISTER_ULONG((PULONG)control, hardwareStatus | 0x70000000 | 0x02000000);
Exit:
    MmUnmapIoSpace(mapped, 0x100000);
    Out->Status = status;
    return status;
}

static NTSTATUS WriteDpcdViaAuxMmio(ULONG Address, const UCHAR *Bytes, ULONG Length, ULONG *HardwareStatus)
{
    PHYSICAL_ADDRESS physicalAddress;
    PUCHAR mapped;
    volatile ULONG *control;
    volatile ULONG *data;
    UCHAR request[20];
    UCHAR response[20];
    ULONG sendLength, receiveLength, offset, index, hardwareStatus, sendControl;
    LARGE_INTEGER interval;
    NTSTATUS status = STATUS_SUCCESS;

    if (Address > 0xFFFFF || Length == 0 || Length > 16) return STATUS_INVALID_PARAMETER;
    sendLength = 4 + Length;
    request[0] = (UCHAR)(0x80 | ((Address >> 16) & 0x0F)); // native DPCD write
    request[1] = (UCHAR)(Address >> 8);
    request[2] = (UCHAR)Address;
    request[3] = (UCHAR)(Length - 1);
    RtlCopyMemory(request + 4, Bytes, Length);

    physicalAddress.QuadPart = 0x90000000;
    mapped = MmMapIoSpace(physicalAddress, 0x100000, MmNonCached);
    if (mapped == NULL) return STATUS_INSUFFICIENT_RESOURCES;
    control = (volatile ULONG *)(mapped + 0x64010);
    data = (volatile ULONG *)(mapped + 0x64014);

    interval.QuadPart = -500;
    for (index = 0; index < 200; ++index) {
        hardwareStatus = READ_REGISTER_ULONG((PULONG)control);
        if ((hardwareStatus & 0x80000000) == 0) break;
        KeDelayExecutionThread(KernelMode, FALSE, &interval);
    }
    if ((hardwareStatus & 0x80000000) != 0) {
        status = STATUS_DEVICE_BUSY;
        goto Exit;
    }

    for (index = 0; index < RTL_NUMBER_OF(request) / 4; ++index) {
        offset = index * 4;
        if (offset >= sendLength) break;
        WRITE_REGISTER_ULONG((PULONG)&data[index], PackAuxBytes(request + offset, min(4u, sendLength - offset)));
    }

    hardwareStatus = READ_REGISTER_ULONG((PULONG)control);
    if ((hardwareStatus & 0x80000000) != 0) {
        status = STATUS_DEVICE_BUSY;
        goto Exit;
    }
    sendControl = 0x80000000 | 0x40000000 | 0x20000000 | 0x10000000 |
        0x0C000000 | 0x02000000 | (sendLength << 20) | 0x23F;
    WRITE_REGISTER_ULONG((PULONG)control, sendControl);

    interval.QuadPart = -500;
    for (index = 0; index < 200; ++index) {
        hardwareStatus = READ_REGISTER_ULONG((PULONG)control);
        if ((hardwareStatus & 0x80000000) == 0) break;
        KeDelayExecutionThread(KernelMode, FALSE, &interval);
    }
    if (HardwareStatus != NULL) *HardwareStatus = hardwareStatus;
    if ((hardwareStatus & 0x80000000) != 0 || (hardwareStatus & 0x10000000) != 0) {
        status = STATUS_IO_TIMEOUT;
        goto ClearStatus;
    }
    if ((hardwareStatus & 0x02000000) != 0) {
        status = STATUS_IO_DEVICE_ERROR;
        goto ClearStatus;
    }
    if ((hardwareStatus & 0x40000000) == 0) {
        status = STATUS_UNSUCCESSFUL;
        goto ClearStatus;
    }
    receiveLength = (hardwareStatus >> 20) & 0x1F;
    if (receiveLength == 0 || receiveLength > sizeof(response)) {
        status = STATUS_DATA_ERROR;
        goto ClearStatus;
    }
    for (offset = 0; offset < receiveLength; offset += 4)
        UnpackAuxBytes(READ_REGISTER_ULONG((PULONG)&data[offset / 4]), response + offset,
            min(4u, receiveLength - offset));
    if ((response[0] >> 4) != 0) status = STATUS_DEVICE_PROTOCOL_ERROR;
    else if (receiveLength > 1 && response[1] != Length) status = STATUS_DATA_ERROR;

ClearStatus:
    WRITE_REGISTER_ULONG((PULONG)control, hardwareStatus | 0x70000000 | 0x02000000);
Exit:
    MmUnmapIoSpace(mapped, 0x100000);
    return status;
}

static NTSTATUS SetNativeBrightnessViaAuxMmio(ULONG Percent, PAUX_NATIVE_BRIGHTNESS_OUT Out)
{
    AUX_MMIO_DPCD_OUT readOut;
    ULONG maximum, bitCount, level, hardwareStatus = 0;
    UCHAR value, brightness[2];
    NTSTATUS status;

    RtlZeroMemory(Out, sizeof(*Out));
    Out->RequestedPercent = Percent;
    if (Percent > 100) return STATUS_INVALID_PARAMETER;

    // Require sink capabilities for AUX enable and AUX-set brightness.
    status = ReadDpcdViaAuxMmio(0x701, 2, &readOut);
    if (!NT_SUCCESS(status)) goto Done;
    if ((readOut.Data[0] & 0x04) == 0 || (readOut.Data[1] & 0x02) == 0) {
        status = STATUS_NOT_SUPPORTED;
        goto Done;
    }

    status = ReadDpcdViaAuxMmio(0x724, 1, &readOut);
    if (!NT_SUCCESS(status)) goto Done;
    bitCount = readOut.Data[0] & 0x1F;
    if (bitCount == 0 || bitCount > 16) {
        status = STATUS_DATA_ERROR;
        goto Done;
    }
    maximum = (1u << bitCount) - 1;
    level = (ULONG)(((ULONGLONG)Percent * maximum + 50) / 100);
    Out->MaximumLevel = maximum;
    Out->AppliedLevel = level;

    // Match the panel's Linux VESA setup: 16-bit PWM resolution, AUX-set
    // frequency 2, DPCD backlight mode, then scaled brightness and enable.
    value = (UCHAR)bitCount;
    status = WriteDpcdViaAuxMmio(0x724, &value, 1, &hardwareStatus);
    if (!NT_SUCCESS(status)) goto Done;
    value = 2;
    status = WriteDpcdViaAuxMmio(0x728, &value, 1, &hardwareStatus);
    if (!NT_SUCCESS(status)) goto Done;
    value = 0x0A; // DPCD brightness mode plus AUX frequency control
    status = WriteDpcdViaAuxMmio(0x721, &value, 1, &hardwareStatus);
    if (!NT_SUCCESS(status)) goto Done;
    brightness[0] = (UCHAR)(level >> 8);
    brightness[1] = (UCHAR)level;
    status = WriteDpcdViaAuxMmio(0x722, brightness, sizeof(brightness), &hardwareStatus);
    if (!NT_SUCCESS(status)) goto Done;
    status = ReadDpcdViaAuxMmio(0x720, 1, &readOut);
    if (!NT_SUCCESS(status)) goto Done;
    value = (UCHAR)(readOut.Data[0] | 0x01);
    status = WriteDpcdViaAuxMmio(0x720, &value, 1, &hardwareStatus);
    if (!NT_SUCCESS(status)) goto Done;

    Out->BacklightEnable = value;
    Out->BacklightMode = 0x0A;
    Out->BrightnessMsb = brightness[0];
    Out->BrightnessLsb = brightness[1];
    status = ReadDpcdViaAuxMmio(0x722, 2, &readOut);
    Out->ReadbackStatus = status;
    if (NT_SUCCESS(status))
        Out->ReadbackBrightness = ((ULONG)readOut.Data[0] << 8) | readOut.Data[1];

Done:
    Out->Status = status;
    return status;
}

static NTSTATUS QueryComplete(PDEVICE_OBJECT DeviceObject, PIRP Irp, PVOID Context)
{
    UNREFERENCED_PARAMETER(DeviceObject);
    UNREFERENCED_PARAMETER(Irp);
    KeSetEvent((PKEVENT)Context, IO_NO_INCREMENT, FALSE);
    return STATUS_MORE_PROCESSING_REQUIRED;
}

static NTSTATUS QueryDpInterface(PDEVICE_OBJECT DeviceObject, PDXGK_DP_INTERFACE Interface)
{
    PIRP irp;
    PIO_STACK_LOCATION stack;
    KEVENT event;
    NTSTATUS status;

    RtlZeroMemory(Interface, sizeof(*Interface));
    Interface->Size = sizeof(*Interface);
    Interface->Version = DXGK_DP_INTERFACE_VERSION_1;
    KeInitializeEvent(&event, NotificationEvent, FALSE);
    irp = IoAllocateIrp(DeviceObject->StackSize, FALSE);
    if (irp == NULL) return STATUS_INSUFFICIENT_RESOURCES;

    IoSetCompletionRoutine(irp, QueryComplete, &event, TRUE, TRUE, TRUE);
    stack = IoGetNextIrpStackLocation(irp);
    stack->MajorFunction = IRP_MJ_PNP;
    stack->MinorFunction = IRP_MN_QUERY_INTERFACE;
    stack->Parameters.QueryInterface.InterfaceType = &GUID_DXGK_DP_INTERFACE;
    stack->Parameters.QueryInterface.Size = sizeof(*Interface);
    stack->Parameters.QueryInterface.Version = DXGK_DP_INTERFACE_VERSION_1;
    stack->Parameters.QueryInterface.Interface = (PINTERFACE)Interface;
    stack->Parameters.QueryInterface.InterfaceSpecificData = NULL;

    // IoAllocateIrp initializes IoStatus to success. Use a failure sentinel so
    // a stack that does not handle/complete this PnP request cannot look like
    // it successfully returned an empty interface.
    irp->IoStatus.Status = STATUS_NOT_SUPPORTED;
    irp->IoStatus.Information = 0;
    status = IoCallDriver(DeviceObject, irp);
    if (status == STATUS_PENDING) {
        KeWaitForSingleObject(&event, Executive, KernelMode, FALSE, NULL);
    }
    status = irp->IoStatus.Status;
    IoFreeIrp(irp);
    return status;
}

static NTSTATUS QueryPciBusInterface(PDEVICE_OBJECT DeviceObject, PPCI_BUS_INTERFACE_STANDARD Interface)
{
    PIRP irp;
    PIO_STACK_LOCATION stack;
    KEVENT event;
    NTSTATUS status;
    RtlZeroMemory(Interface, sizeof(*Interface));
    Interface->Size = PCI_BUS_INTERFACE_STANDARD_VERSION_1_LENGTH;
    Interface->Version = 1;
    KeInitializeEvent(&event, NotificationEvent, FALSE);
    irp = IoAllocateIrp(DeviceObject->StackSize, FALSE);
    if (irp == NULL) return STATUS_INSUFFICIENT_RESOURCES;
    IoSetCompletionRoutine(irp, QueryComplete, &event, TRUE, TRUE, TRUE);
    stack = IoGetNextIrpStackLocation(irp);
    stack->MajorFunction = IRP_MJ_PNP;
    stack->MinorFunction = IRP_MN_QUERY_INTERFACE;
    stack->Parameters.QueryInterface.InterfaceType = &GUID_BUS_INTERFACE_STANDARD;
    stack->Parameters.QueryInterface.Size = PCI_BUS_INTERFACE_STANDARD_VERSION_1_LENGTH;
    stack->Parameters.QueryInterface.Version = 1;
    stack->Parameters.QueryInterface.Interface = (PINTERFACE)Interface;
    stack->Parameters.QueryInterface.InterfaceSpecificData = NULL;
    irp->IoStatus.Status = STATUS_NOT_SUPPORTED;
    irp->IoStatus.Information = 0;
    status = IoCallDriver(DeviceObject, irp);
    if (status == STATUS_PENDING) KeWaitForSingleObject(&event, Executive, KernelMode, FALSE, NULL);
    status = irp->IoStatus.Status;
    IoFreeIrp(irp);
    return status;
}

static NTSTATUS ReadIntelGpuAsls(PULONG Asls, PULONG BytesRead, PUSHORT VendorId, PUSHORT DeviceId)
{
    PWSTR interfaceList = NULL, current;
    NTSTATUS status = STATUS_NOT_FOUND;
    *Asls = 0;
    *BytesRead = 0;
    *VendorId = 0;
    *DeviceId = 0;

    // The active adapter's PnP PDO name is reported by Windows as
    // DEVPKEY_Device_PDOName. Query its base object directly first so the
    // request reaches PCI.sys instead of the WDDM adapter function driver.
    {
        UNICODE_STRING pdoName;
        PFILE_OBJECT fileObject = NULL;
        PDEVICE_OBJECT deviceObject = NULL, pdo = NULL;
        PCI_BUS_INTERFACE_STANDARD bus = {0};
        PCI_COMMON_CONFIG config;
        ULONG got;
        RtlInitUnicodeString(&pdoName, L"\\Device\\NTPNP_PCI0001");
        status = IoGetDeviceObjectPointer(&pdoName, FILE_READ_ATTRIBUTES, &fileObject, &deviceObject);
        if (!NT_SUCCESS(status)) {
            *BytesRead = (ULONG)status;
            return status;
        }
        if (NT_SUCCESS(status)) {
            pdo = IoGetDeviceAttachmentBaseRef(deviceObject);
            status = QueryPciBusInterface(pdo, &bus);
            ObDereferenceObject(pdo);
            if (!NT_SUCCESS(status)) {
                *BytesRead = (ULONG)status;
                status = QueryPciBusInterface(deviceObject, &bus);
            }
            if (!NT_SUCCESS(status)) {
                PDEVICE_OBJECT top = IoGetAttachedDeviceReference(deviceObject);
                status = QueryPciBusInterface(top, &bus);
                ObDereferenceObject(top);
            }
            if (NT_SUCCESS(status) && bus.ReadConfig != NULL && bus.InterfaceReference != NULL && bus.InterfaceDereference != NULL) {
                bus.InterfaceReference(bus.Context);
                RtlZeroMemory(&config, sizeof(config));
                got = bus.ReadConfig(bus.Context, 0, 0x10, &config, 0, sizeof(config));
                *BytesRead = got;
                *VendorId = config.VendorID;
                *DeviceId = config.DeviceID;
                if (got >= sizeof(config) && config.VendorID == 0x8086 && config.DeviceID == 0x9A78) {
                    got = bus.ReadConfig(bus.Context, 0, 0x10, Asls, 0xFC, sizeof(*Asls));
                    *BytesRead = got;
                    bus.InterfaceDereference(bus.Context);
                    ObDereferenceObject(fileObject);
                    return got == sizeof(*Asls) && *Asls != 0 ? STATUS_SUCCESS : STATUS_DEVICE_NOT_READY;
                }
                bus.InterfaceDereference(bus.Context);
            }
            ObDereferenceObject(fileObject);
            if (!NT_SUCCESS(status)) {
                *BytesRead = (ULONG)status;
                return status;
            }
        }
    }

    status = IoGetDeviceInterfaces(&GUID_DEVINTERFACE_DISPLAY_ADAPTER, NULL, 0, &interfaceList);
    if (!NT_SUCCESS(status) || interfaceList == NULL) return status;
    for (current = interfaceList; *current != UNICODE_NULL; ) {
        UNICODE_STRING interfaceName;
        PFILE_OBJECT fileObject = NULL;
        PDEVICE_OBJECT deviceObject = NULL, pdo;
        PCI_BUS_INTERFACE_STANDARD bus = {0};
        PCI_COMMON_CONFIG config;
        ULONG got;
        PWSTR end = current;
        while (*end != UNICODE_NULL) ++end;
        RtlInitUnicodeString(&interfaceName, current);
        current = end + 1;
        status = IoGetDeviceObjectPointer(&interfaceName, FILE_READ_ATTRIBUTES, &fileObject, &deviceObject);
        if (!NT_SUCCESS(status)) continue;
        pdo = IoGetDeviceAttachmentBaseRef(deviceObject);
        status = QueryPciBusInterface(pdo, &bus);
        ObDereferenceObject(pdo);
        if (NT_SUCCESS(status) && bus.ReadConfig != NULL && bus.InterfaceReference != NULL && bus.InterfaceDereference != NULL) {
            bus.InterfaceReference(bus.Context);
            RtlZeroMemory(&config, sizeof(config));
            got = bus.ReadConfig(bus.Context, 0, 0x10, &config, 0, sizeof(config));
            *BytesRead = got;
            *VendorId = config.VendorID;
            *DeviceId = config.DeviceID;
            if (got >= sizeof(config) && config.VendorID == 0x8086 && config.DeviceID == 0x9A78) {
                got = bus.ReadConfig(bus.Context, 0, 0x10, Asls, 0xFC, sizeof(*Asls));
                *BytesRead = got;
                bus.InterfaceDereference(bus.Context);
                ObDereferenceObject(fileObject);
                ExFreePool(interfaceList);
                return got == sizeof(*Asls) && *Asls != 0 ? STATUS_SUCCESS : STATUS_DEVICE_NOT_READY;
            }
            bus.InterfaceDereference(bus.Context);
        }
        ObDereferenceObject(fileObject);
    }
    ExFreePool(interfaceList);
    return status;
}

static NTSTATUS FindDpInterface(VOID)
{
    WCHAR nameBuffer[32];
    UNICODE_STRING deviceName;
    PFILE_OBJECT fileObject = NULL;
    PDEVICE_OBJECT deviceObject = NULL;
    ULONG index;
    PWSTR interfaceList = NULL;
    PWSTR current;
    NTSTATUS status = STATUS_OBJECT_NAME_NOT_FOUND;

    // Prefer the registered PnP display-adapter interface. VideoN names are
    // legacy and are not necessarily published by WDDM miniports.
    status = IoGetDeviceInterfaces(&GUID_DEVINTERFACE_DISPLAY_ADAPTER, NULL, 0, &interfaceList);
    g_InterfaceEnumerationStatus = status;
    if (NT_SUCCESS(status) && interfaceList != NULL) {
        for (current = interfaceList; *current != UNICODE_NULL; ) {
            UNICODE_STRING interfaceName;
            PWSTR end = current;
            while (*end != UNICODE_NULL) ++end;
            ++g_InterfaceCount;
            PDEVICE_OBJECT top;
            RtlInitUnicodeString(&interfaceName, current);
            current = end + 1;
            fileObject = NULL;
            deviceObject = NULL;
            status = IoGetDeviceObjectPointer(&interfaceName, FILE_READ_ATTRIBUTES, &fileObject, &deviceObject);
            g_LastOpenStatus = status;
            if (!NT_SUCCESS(status)) continue;

            top = IoGetAttachedDeviceReference(deviceObject);
            status = QueryDpInterface(top, &g_DpInterface);
            g_LastQueryInterfaceStatus = status;
            g_InterfaceFunctionMask = (g_DpInterface.InterfaceReference ? 1u : 0u) |
                (g_DpInterface.InterfaceDereference ? 2u : 0u) |
                (g_DpInterface.DxgkDdiQueryDPCaps ? 4u : 0u) |
                (g_DpInterface.DxgkDdiGetDPAddress ? 8u : 0u) |
                (g_DpInterface.DxgkDdiDPAuxIoTransmission ? 16u : 0u);
            ObDereferenceObject(top);
            if (NT_SUCCESS(status) && g_DpInterface.InterfaceReference != NULL && g_DpInterface.InterfaceDereference != NULL &&
                g_DpInterface.DxgkDdiQueryDPCaps != NULL && g_DpInterface.DxgkDdiDPAuxIoTransmission != NULL &&
                g_DpInterface.DxgkDdiGetDPAddress != NULL) {
                g_VideoFileObject = fileObject;
                g_VideoDeviceObject = deviceObject;
                g_DpInterface.InterfaceReference(g_DpInterface.Context);
                g_DpInterfaceReferenced = TRUE;
                RtlZeroMemory(&g_DpCaps, sizeof(g_DpCaps));
                status = g_DpInterface.DxgkDdiQueryDPCaps(g_DpInterface.Context, &g_DpCaps);
                g_LastDpcapsStatus = status;
                if (NT_SUCCESS(status)) {
                    g_QueryStatus = status;
                    ExFreePool(interfaceList);
                    return status;
                }
                // A display interface can be published by a non-target stack
                // or adapter with no active DP roots. Keep searching.
                g_DpInterface.InterfaceDereference(g_DpInterface.Context);
                g_DpInterfaceReferenced = FALSE;
                RtlZeroMemory(&g_DpInterface, sizeof(g_DpInterface));
                ObDereferenceObject(fileObject);
                fileObject = NULL;
                g_LastQueryInterfaceStatus = status;
                continue;
            }
            ObDereferenceObject(fileObject);
            fileObject = NULL;
        }
        ExFreePool(interfaceList);
    }

    // Some miniports expose the DP direct-call interface through the child
    // monitor stack. Try enabled monitor interfaces as a separate target set.
    interfaceList = NULL;
    status = IoGetDeviceInterfaces(&GUID_DEVINTERFACE_MONITOR, NULL, 0, &interfaceList);
    if (NT_SUCCESS(status) && interfaceList != NULL) {
        for (current = interfaceList; *current != UNICODE_NULL; ) {
            UNICODE_STRING interfaceName;
            PWSTR end = current;
            PDEVICE_OBJECT top;
            while (*end != UNICODE_NULL) ++end;
            ++g_InterfaceCount;
            RtlInitUnicodeString(&interfaceName, current);
            current = end + 1;
            fileObject = NULL;
            deviceObject = NULL;
            status = IoGetDeviceObjectPointer(&interfaceName, FILE_READ_ATTRIBUTES, &fileObject, &deviceObject);
            g_LastOpenStatus = status;
            if (!NT_SUCCESS(status)) continue;

            top = IoGetAttachedDeviceReference(deviceObject);
            status = QueryDpInterface(top, &g_DpInterface);
            g_LastQueryInterfaceStatus = status;
            g_InterfaceFunctionMask = (g_DpInterface.InterfaceReference ? 1u : 0u) |
                (g_DpInterface.InterfaceDereference ? 2u : 0u) |
                (g_DpInterface.DxgkDdiQueryDPCaps ? 4u : 0u) |
                (g_DpInterface.DxgkDdiGetDPAddress ? 8u : 0u) |
                (g_DpInterface.DxgkDdiDPAuxIoTransmission ? 16u : 0u);
            ObDereferenceObject(top);
            if (NT_SUCCESS(status) && g_DpInterface.InterfaceReference != NULL && g_DpInterface.InterfaceDereference != NULL &&
                g_DpInterface.DxgkDdiQueryDPCaps != NULL && g_DpInterface.DxgkDdiDPAuxIoTransmission != NULL &&
                g_DpInterface.DxgkDdiGetDPAddress != NULL) {
                g_VideoFileObject = fileObject;
                g_VideoDeviceObject = deviceObject;
                g_DpInterface.InterfaceReference(g_DpInterface.Context);
                g_DpInterfaceReferenced = TRUE;
                RtlZeroMemory(&g_DpCaps, sizeof(g_DpCaps));
                status = g_DpInterface.DxgkDdiQueryDPCaps(g_DpInterface.Context, &g_DpCaps);
                g_LastDpcapsStatus = status;
                if (NT_SUCCESS(status)) {
                    g_QueryStatus = status;
                    ExFreePool(interfaceList);
                    return status;
                }
                g_DpInterface.InterfaceDereference(g_DpInterface.Context);
                g_DpInterfaceReferenced = FALSE;
                RtlZeroMemory(&g_DpInterface, sizeof(g_DpInterface));
            }
            ObDereferenceObject(fileObject);
            fileObject = NULL;
        }
        ExFreePool(interfaceList);
    }

    for (index = 0; index < 16; ++index) {
        RtlStringCchPrintfW(nameBuffer, RTL_NUMBER_OF(nameBuffer), L"\\Device\\Video%lu", index);
        RtlInitUnicodeString(&deviceName, nameBuffer);
        status = IoGetDeviceObjectPointer(&deviceName, FILE_READ_ATTRIBUTES, &fileObject, &deviceObject);
        if (!NT_SUCCESS(status)) continue;

        PDEVICE_OBJECT top = IoGetAttachedDeviceReference(deviceObject);
        status = QueryDpInterface(top, &g_DpInterface);
        ObDereferenceObject(top);
        if (NT_SUCCESS(status) && g_DpInterface.InterfaceReference != NULL && g_DpInterface.InterfaceDereference != NULL && g_DpInterface.DxgkDdiQueryDPCaps != NULL &&
            g_DpInterface.DxgkDdiDPAuxIoTransmission != NULL &&
            g_DpInterface.DxgkDdiGetDPAddress != NULL) {
            g_VideoFileObject = fileObject;
            g_VideoDeviceObject = deviceObject;
            g_TopVideoDeviceObject = NULL;
            g_DpInterface.InterfaceReference(g_DpInterface.Context);
            g_DpInterfaceReferenced = TRUE;
            RtlZeroMemory(&g_DpCaps, sizeof(g_DpCaps));
            status = g_DpInterface.DxgkDdiQueryDPCaps(g_DpInterface.Context, &g_DpCaps);
            g_QueryStatus = status;
            return status;
        }
        ObDereferenceObject(fileObject);
        fileObject = NULL;
    }
    g_QueryStatus = status;
    return status;
}

static NTSTATUS DispatchCreateClose(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
    UNREFERENCED_PARAMETER(DeviceObject);
    Irp->IoStatus.Status = STATUS_SUCCESS;
    Irp->IoStatus.Information = 0;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return STATUS_SUCCESS;
}

static NTSTATUS DispatchDeviceControl(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
    PIO_STACK_LOCATION irpSp = IoGetCurrentIrpStackLocation(Irp);
    ULONG code = irpSp->Parameters.DeviceIoControl.IoControlCode;
    ULONG inLength = irpSp->Parameters.DeviceIoControl.InputBufferLength;
    ULONG outLength = irpSp->Parameters.DeviceIoControl.OutputBufferLength;
    PVOID buffer = Irp->AssociatedIrp.SystemBuffer;
    NTSTATUS status = STATUS_INVALID_DEVICE_REQUEST;
    ULONG_PTR information = 0;
    UNREFERENCED_PARAMETER(DeviceObject);

    if (code == IOCTL_AUX_ACPI_QUERY_XBQC) {
        if (inLength < sizeof(ULONG) || outLength < sizeof(AUX_ACPI_QUERY_OUT)) {
            status = STATUS_BUFFER_TOO_SMALL;
        } else {
            ULONG pathIndex = *(ULONG *)buffer;
            PAUX_ACPI_QUERY_OUT out = (PAUX_ACPI_QUERY_OUT)buffer;
            RtlZeroMemory(out, sizeof(*out));
            out->TransportStatus = QueryFirmwareBrightness(
                pathIndex,
                &out->BrightnessValue,
                &out->ArgumentCount,
                &out->ArgumentType,
                &out->ArgumentLength);
            out->AcpiStackFound = out->TransportStatus != STATUS_NOT_FOUND;
            information = sizeof(*out);
            status = STATUS_SUCCESS;
        }
    } else if (code == IOCTL_AUX_READ_DPCD_MMIO) {
        if (inLength < sizeof(AUX_MMIO_DPCD_IN) || outLength < sizeof(AUX_MMIO_DPCD_OUT)) {
            status = STATUS_BUFFER_TOO_SMALL;
        } else {
            ULONG address = ((PAUX_MMIO_DPCD_IN)buffer)->Address;
            ULONG length = ((PAUX_MMIO_DPCD_IN)buffer)->Length;
            PAUX_MMIO_DPCD_OUT out = (PAUX_MMIO_DPCD_OUT)buffer;
            status = ReadDpcdViaAuxMmio(address, length, out);
            information = sizeof(*out);
            status = STATUS_SUCCESS;
        }
    } else if (code == IOCTL_AUX_SET_NATIVE_BRIGHTNESS) {
        if (inLength < sizeof(ULONG) || outLength < sizeof(AUX_NATIVE_BRIGHTNESS_OUT)) {
            status = STATUS_BUFFER_TOO_SMALL;
        } else {
            ULONG requested = *(ULONG *)buffer;
            PAUX_NATIVE_BRIGHTNESS_OUT out = (PAUX_NATIVE_BRIGHTNESS_OUT)buffer;
            RtlZeroMemory(out, sizeof(*out));
            ExAcquireFastMutex(&g_AuxMmioMutex);
            status = SetNativeBrightnessViaAuxMmio(requested, out);
            ExReleaseFastMutex(&g_AuxMmioMutex);
            information = sizeof(*out);
            status = STATUS_SUCCESS;
        }
    } else if (code == IOCTL_AUX_ACPI_SET_XBCM) {
        if (inLength < sizeof(ULONG) || outLength < sizeof(AUX_ACPI_SET_OUT)) {
            status = STATUS_BUFFER_TOO_SMALL;
        } else {
            ULONG requested = *(ULONG *)buffer;
            PAUX_ACPI_SET_OUT out = (PAUX_ACPI_SET_OUT)buffer;
            RtlZeroMemory(out, sizeof(*out));
            out->RequestedBrightness = requested;
            out->TransportStatus = SetFirmwareBrightness(requested);
            out->ReadbackStatus = QueryFirmwareBrightness(
                5,
                &out->ReadbackBrightness,
                &out->ReadbackCount,
                &out->ReadbackType,
                &out->ReadbackLength);
            information = sizeof(*out);
            status = STATUS_SUCCESS;
        }
    } else if (code == IOCTL_AUX_READ_GPU_MMIO) {
        static const ULONG offsets[] = { 0x64010, 0x64110, 0x64210, 0x64310, 0xE4110, 0xE4210 };
        PAUX_MMIO_OUT out = (PAUX_MMIO_OUT)buffer;
        PHYSICAL_ADDRESS physicalAddress;
        PUCHAR mapped;
        ULONG index;
        if (outLength < sizeof(AUX_MMIO_OUT)) {
            status = STATUS_BUFFER_TOO_SMALL;
        } else {
            RtlZeroMemory(out, sizeof(*out));
            // WMI confirms this device's BAR0 is 0x90000000-0x90FFFFFF.
            // This IOCTL only reads candidate Intel AUX control registers.
            physicalAddress.QuadPart = 0x90000000;
            mapped = MmMapIoSpace(physicalAddress, 0x100000, MmNonCached);
            if (mapped == NULL) {
                out->Status = STATUS_INSUFFICIENT_RESOURCES;
            } else {
                for (index = 0; index < RTL_NUMBER_OF(offsets); ++index)
                    out->RegisterValues[index] = READ_REGISTER_ULONG((PULONG)(mapped + offsets[index]));
                MmUnmapIoSpace(mapped, 0x100000);
                out->Status = STATUS_SUCCESS;
            }
            information = sizeof(*out);
            status = STATUS_SUCCESS;
        }
    } else if (code == IOCTL_AUX_DUMP_OPREGION) {
        const ULONG maxOpRegionSize = 0x10000;
        PAUX_OPREGION_OUT out = (PAUX_OPREGION_OUT)buffer;
        ULONG asls = 0, transferred = 0, sizeKb = 0, opRegionSize = 0;
        USHORT vendorId = 0, deviceId = 0;
        PVOID mapped = NULL;
        PHYSICAL_ADDRESS physicalAddress;

        if (outLength < FIELD_OFFSET(AUX_OPREGION_OUT, Data)) {
            status = STATUS_BUFFER_TOO_SMALL;
        } else {
            RtlZeroMemory(out, FIELD_OFFSET(AUX_OPREGION_OUT, Data));
            // Use the display adapter's standard PCI bus interface to read
            // ASLS; the legacy HAL config-space helper is not reliable here.
            status = ReadIntelGpuAsls(&asls, &transferred, &vendorId, &deviceId);
            out->PciBytes = transferred;
            out->VendorId = vendorId;
            out->DeviceId = deviceId;
            if (NT_SUCCESS(status)) {
                physicalAddress.QuadPart = asls;
                mapped = MmMapIoSpace(physicalAddress, 20, MmNonCached);
                if (mapped == NULL) status = STATUS_INSUFFICIENT_RESOURCES;
                else {
                    if (RtlCompareMemory(mapped, "IntelGraphicsMem", 16) != 16)
                        status = STATUS_NOT_FOUND;
                    else RtlCopyMemory(&sizeKb, (PUCHAR)mapped + 16, sizeof(sizeKb));
                    MmUnmapIoSpace(mapped, 20);
                    mapped = NULL;
                    if (NT_SUCCESS(status)) {
                        if (sizeKb < 1 || sizeKb > maxOpRegionSize / 1024) status = STATUS_INVALID_BUFFER_SIZE;
                        else {
                            opRegionSize = sizeKb * 1024;
                            if (outLength < FIELD_OFFSET(AUX_OPREGION_OUT, Data) + opRegionSize)
                                status = STATUS_BUFFER_TOO_SMALL;
                            else {
                                mapped = MmMapIoSpace(physicalAddress, opRegionSize, MmNonCached);
                                if (mapped == NULL) status = STATUS_INSUFFICIENT_RESOURCES;
                                else {
                                    RtlCopyMemory(out->Data, mapped, opRegionSize);
                                    out->AslsAddress = asls;
                                    out->OpRegionSize = opRegionSize;
                                    out->BytesCopied = opRegionSize;
                                    out->VbtOffset = 0x400;
                                    information = FIELD_OFFSET(AUX_OPREGION_OUT, Data) + opRegionSize;
                                    MmUnmapIoSpace(mapped, opRegionSize);
                                    mapped = NULL;
                                }
                            }
                        }
                    }
                }
            }
            out->Status = status;
            out->AslsAddress = asls;
            out->OpRegionSize = opRegionSize;
            if (information == 0) information = FIELD_OFFSET(AUX_OPREGION_OUT, Data);
            status = STATUS_SUCCESS; // return detailed result in the header
        }
    } else if (code == IOCTL_AUX_STATUS) {
        if (outLength < sizeof(AUX_STATUS_OUT)) status = STATUS_BUFFER_TOO_SMALL;
        else {
            PAUX_STATUS_OUT out = (PAUX_STATUS_OUT)buffer;
            RtlZeroMemory(out, sizeof(*out));
            out->QueryStatus = g_QueryStatus;
            out->EnumerationStatus = g_InterfaceEnumerationStatus;
            out->LastOpenStatus = g_LastOpenStatus;
            out->LastQueryInterfaceStatus = g_LastQueryInterfaceStatus;
            out->InterfaceVersion = g_DpInterface.Version;
            out->InterfaceSize = g_DpInterface.Size;
            out->NumRootPorts = g_DpCaps.NumRootPorts;
            out->InterfaceCount = g_InterfaceCount;
            out->DPVersionMajor = g_DpCaps.DPVersionMajor;
            out->DPVersionMinor = g_DpCaps.DPVersionMinor;
            out->LastDpcapsStatus = g_LastDpcapsStatus;
            out->InterfaceFunctionMask = g_InterfaceFunctionMask;
            information = sizeof(*out);
            status = STATUS_SUCCESS;
        }
    } else if (code == IOCTL_AUX_GET_DP_ADDRESS) {
        if (inLength < sizeof(AUX_TARGET_IN) || outLength < sizeof(AUX_TARGET_OUT)) status = STATUS_BUFFER_TOO_SMALL;
        else if (!NT_SUCCESS(g_QueryStatus)) status = g_QueryStatus;
        else {
            PAUX_TARGET_IN in = (PAUX_TARGET_IN)buffer;
            PAUX_TARGET_OUT out = (PAUX_TARGET_OUT)buffer;
            DXGKARG_GETDPADDRESS args;
            RtlZeroMemory(&args, sizeof(args));
            args.TargetId = in->TargetId;
            status = g_DpInterface.DxgkDdiGetDPAddress(g_DpInterface.Context, &args);
            RtlZeroMemory(out, sizeof(*out));
            out->Status = status;
            out->NativeError = args.DPNativeError;
            out->RootPortIndex = args.RootPortIndex;
            out->NumLinks = args.NumLinks;
            RtlCopyMemory(out->RelativeAddress, args.RelAddress, sizeof(args.RelAddress));
            information = sizeof(*out);
        }
    } else if (code == IOCTL_AUX_READ_DPCD) {
        // Route the guarded backlight write through the already validated
        // read-access control code. The device itself is admin/SYSTEM-only;
        // marker and strict address/length allowlist distinguish this request.
        if (inLength >= sizeof(AUX_WRITE_IN) && outLength >= sizeof(AUX_WRITE_OUT) &&
            ((PAUX_WRITE_IN)buffer)->Reserved[0] == 0xA5) {
            PAUX_WRITE_IN in = (PAUX_WRITE_IN)buffer;
            PAUX_WRITE_OUT out = (PAUX_WRITE_OUT)buffer;
            DXGKARG_DPAUXIOTRANSMISSION args;
            BOOLEAN allowed = FALSE;
            if (in->DpcdAddress == 0x720 && in->Length == 1) allowed = TRUE;
            if (in->DpcdAddress == 0x721 && in->Length == 1) allowed = TRUE;
            if (in->DpcdAddress == 0x722 && in->Length == 2) allowed = TRUE;
            if (in->DpcdAddress == 0x724 && in->Length == 1) allowed = TRUE;
            if (in->DpcdAddress == 0x728 && in->Length == 1) allowed = TRUE;
            if (!NT_SUCCESS(g_QueryStatus)) status = g_QueryStatus;
            else if (in->RootPortIndex >= g_DpCaps.NumRootPorts || !allowed) status = STATUS_INVALID_PARAMETER;
            else {
                RtlZeroMemory(&args, sizeof(args));
                args.RootPortIndex = in->RootPortIndex;
                args.DPCDAddress = in->DpcdAddress;
                args.NumBytesRequested = in->Length;
                args.CanUseCachedData = 0;
                args.Write = 1;
                RtlCopyMemory(args.Data, in->Data, in->Length);
                status = g_DpInterface.DxgkDdiDPAuxIoTransmission(g_DpInterface.Context, &args);
                RtlZeroMemory(out, sizeof(*out));
                out->Status = status;
                out->NativeError = args.DPNativeError;
                out->BytesDone = args.NumBytesDone;
                information = sizeof(*out);
                // Return the transport diagnostics through the output buffer,
                // even when the miniport reports a failed AUX write.
                status = STATUS_SUCCESS;
            }
        }
        else if (inLength < sizeof(AUX_READ_IN) || outLength < sizeof(AUX_READ_OUT)) status = STATUS_BUFFER_TOO_SMALL;
        else if (!NT_SUCCESS(g_QueryStatus)) status = g_QueryStatus;
        else {
            PAUX_READ_IN in = (PAUX_READ_IN)buffer;
            PAUX_READ_OUT out = (PAUX_READ_OUT)buffer;
            DXGKARG_DPAUXIOTRANSMISSION args;
            if (in->RootPortIndex >= g_DpCaps.NumRootPorts || in->Length == 0 || in->Length > MAX_PROBE_BYTES || in->DpcdAddress > 0xFFFFF) {
                status = STATUS_INVALID_PARAMETER;
            } else {
                RtlZeroMemory(&args, sizeof(args));
                args.RootPortIndex = in->RootPortIndex;
                args.DPCDAddress = in->DpcdAddress;
                args.NumBytesRequested = in->Length;
                args.CanUseCachedData = 0;
                args.Write = 0;
                status = g_DpInterface.DxgkDdiDPAuxIoTransmission(g_DpInterface.Context, &args);
                RtlZeroMemory(out, sizeof(*out));
                out->Status = status;
                out->NativeError = args.DPNativeError;
                out->BytesDone = args.NumBytesDone;
                RtlCopyMemory(out->Data, args.Data, min(args.NumBytesDone, MAX_PROBE_BYTES));
                information = sizeof(*out);
            }
        }
    } else if (code == IOCTL_AUX_WRITE_BACKLIGHT_DPCD) {
        if (inLength < sizeof(AUX_WRITE_IN) || outLength < sizeof(AUX_WRITE_OUT)) status = STATUS_BUFFER_TOO_SMALL;
        else if (!NT_SUCCESS(g_QueryStatus)) status = g_QueryStatus;
        else {
            PAUX_WRITE_IN in = (PAUX_WRITE_IN)buffer;
            PAUX_WRITE_OUT out = (PAUX_WRITE_OUT)buffer;
            DXGKARG_DPAUXIOTRANSMISSION args;
            BOOLEAN allowed = FALSE;
            if (in->DpcdAddress == 0x720 && in->Length == 1) allowed = TRUE;
            if (in->DpcdAddress == 0x721 && in->Length == 1) allowed = TRUE;
            if (in->DpcdAddress == 0x722 && in->Length == 2) allowed = TRUE;
            if (in->DpcdAddress == 0x724 && in->Length == 1) allowed = TRUE;
            if (in->DpcdAddress == 0x728 && in->Length == 1) allowed = TRUE;
            if (in->RootPortIndex >= g_DpCaps.NumRootPorts || !allowed) {
                status = STATUS_INVALID_PARAMETER;
            } else {
                RtlZeroMemory(&args, sizeof(args));
                args.RootPortIndex = in->RootPortIndex;
                args.DPCDAddress = in->DpcdAddress;
                args.NumBytesRequested = in->Length;
                args.CanUseCachedData = 0;
                args.Write = 1;
                RtlCopyMemory(args.Data, in->Data, in->Length);
                status = g_DpInterface.DxgkDdiDPAuxIoTransmission(g_DpInterface.Context, &args);
                RtlZeroMemory(out, sizeof(*out));
                out->Status = status;
                out->NativeError = args.DPNativeError;
                out->BytesDone = args.NumBytesDone;
                information = sizeof(*out);
                status = STATUS_SUCCESS;
            }
        }
    }

    Irp->IoStatus.Status = status;
    Irp->IoStatus.Information = information;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return status;
}

static VOID DriverUnload(PDRIVER_OBJECT DriverObject)
{
    UNICODE_STRING link;
    RtlInitUnicodeString(&link, SYMBOLIC_NAME);
    if (g_SymbolicLinkCreated) IoDeleteSymbolicLink(&link);
    if (g_DpInterfaceReferenced) g_DpInterface.InterfaceDereference(g_DpInterface.Context);
    if (g_VideoFileObject != NULL) ObDereferenceObject(g_VideoFileObject);
    if (g_DeviceObject != NULL) IoDeleteDevice(g_DeviceObject);
    UNREFERENCED_PARAMETER(DriverObject);
}

NTSTATUS DriverEntry(PDRIVER_OBJECT DriverObject, PUNICODE_STRING RegistryPath)
{
    UNICODE_STRING deviceName, linkName, sddl;
    NTSTATUS status;
    UNREFERENCED_PARAMETER(RegistryPath);

    ExInitializeFastMutex(&g_AuxMmioMutex);
    status = FindDpInterface();
    RtlInitUnicodeString(&deviceName, DEVICE_NAME);
    RtlInitUnicodeString(&sddl, L"D:P(A;;GA;;;SY)(A;;GA;;;BA)");
    status = IoCreateDeviceSecure(DriverObject, 0, &deviceName, FILE_DEVICE_UNKNOWN,
        FILE_DEVICE_SECURE_OPEN, FALSE, &sddl, &g_DeviceClassGuid, &g_DeviceObject);
    if (!NT_SUCCESS(status)) return status;
    g_DeviceObject->Flags |= DO_BUFFERED_IO;
    RtlInitUnicodeString(&linkName, SYMBOLIC_NAME);
    status = IoCreateSymbolicLink(&linkName, &deviceName);
    if (!NT_SUCCESS(status)) { IoDeleteDevice(g_DeviceObject); g_DeviceObject=NULL; return status; }
    g_SymbolicLinkCreated = TRUE;
    DriverObject->DriverUnload = DriverUnload;
    DriverObject->MajorFunction[IRP_MJ_CREATE] = DispatchCreateClose;
    DriverObject->MajorFunction[IRP_MJ_CLOSE] = DispatchCreateClose;
    DriverObject->MajorFunction[IRP_MJ_CLEANUP] = DispatchCreateClose;
    DriverObject->MajorFunction[IRP_MJ_DEVICE_CONTROL] = DispatchDeviceControl;
    g_DeviceObject->Flags &= ~DO_DEVICE_INITIALIZING;
    return STATUS_SUCCESS;
}







