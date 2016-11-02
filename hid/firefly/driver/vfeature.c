/*++

Copyright (c) Microsoft Corporation.  All rights reserved.

    THIS CODE AND INFORMATION IS PROVIDED "AS IS" WITHOUT WARRANTY OF ANY
    KIND, EITHER EXPRESSED OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE
    IMPLIED WARRANTIES OF MERCHANTABILITY AND/OR FITNESS FOR A PARTICULAR
    PURPOSE.

Module Name:

    vfeature.c

Abstract:

    This module sets the state of a vendor specific HID feature.

Environment:

    Kernel mode

--*/

#include "firefly.h"

#pragma warning(disable:4201)  // nameless struct/union
#pragma warning(disable:4214)  // bit field types other than int

#include <hidpddi.h>
#include <hidclass.h>

#pragma warning(default:4201)
#pragma warning(default:4214)

#ifdef ALLOC_PRAGMA
#pragma alloc_text(PAGE, FireflySetFeature)
#endif

//用到两个IOCTLs：
//IOCTL_HID_GET_COLLECTION_INFORMATION
//IOCTL_HID_GET_COLLECTION_DESCRIPTOR
NTSTATUS
FireflySetFeature(
    IN  PDEVICE_CONTEXT DeviceContext, //需要名字来Open远程iotartget，用这个做参数比较好
    IN  UCHAR           PageId,    //就是UsagePage
    IN  USHORT          FeatureId, //就是Usage
    IN  BOOLEAN         EnableFeature
    )
/*++

Routine Description:

    This routine sets the HID feature by sending HID ioctls to our device.
    These IOCTLs will be handled by HIDUSB and converted into USB requests
    and send to the device.

Arguments:

    DeviceContext - Context for our device

    PageID  - UsagePage of the light control feature.

    FeatureId - Usage ID of the feature.

    EnanbleFeature - True to turn the light on, Falst to turn if off.


Return Value:

    NT Status code

--*/
{
    WDF_MEMORY_DESCRIPTOR       inputDescriptor, outputDescriptor;
    NTSTATUS                    status;
    HID_COLLECTION_INFORMATION  collectionInformation = {0};
    PHIDP_PREPARSED_DATA        preparsedData;
    HIDP_CAPS                   caps;
    USAGE                       usage;
    ULONG                       usageLength;
    PCHAR                       report;
    WDFIOTARGET                 hidTarget;
    WDF_IO_TARGET_OPEN_PARAMS   openParams;

    PAGED_CODE();

    //
    // Preinit for error.
    //
    preparsedData = NULL;
    report = NULL;
    hidTarget = NULL;
    
    //下面注意打开远程iotarget，不是本地iotarget
    status = WdfIoTargetCreate(WdfObjectContextGetObject(DeviceContext), 
                            WDF_NO_OBJECT_ATTRIBUTES, 
                            &hidTarget);    
    if (!NT_SUCCESS(status)) {
        KdPrint(("FireFly: WdfIoTargetCreate failed 0x%x\n", status));        
        return status;
    }

    //
    // Open it up, write access only!
    //
    WDF_IO_TARGET_OPEN_PARAMS_INIT_OPEN_BY_NAME(
                                    &openParams,
                                    &DeviceContext->PdoName,
                                    FILE_WRITE_ACCESS); //为什么只有写的权限？

    //
    // We will let the framework to respond automatically to the pnp
    // state changes of the target by closing and opening the handle.
    //
    openParams.ShareAccess = FILE_SHARE_WRITE | FILE_SHARE_READ;

    status = WdfIoTargetOpen(hidTarget, &openParams);
    if (!NT_SUCCESS(status)) {
        KdPrint(("FireFly: WdfIoTargetOpen failed 0x%x\n", status));                
        goto ExitAndFree;
    }
    

    WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&outputDescriptor,
                                      (PVOID) &collectionInformation, //real buffer
                                      sizeof(HID_COLLECTION_INFORMATION));

    //
    // Now get the collection information for this device
    //
    status = WdfIoTargetSendIoctlSynchronously(hidTarget,
                                  NULL,
                                  IOCTL_HID_GET_COLLECTION_INFORMATION,
                                  NULL,
                                  &outputDescriptor, //实际是&collectionInformation
                                  NULL,
                                  NULL);

    if (!NT_SUCCESS(status)) {
        KdPrint(("FireFly: WdfIoTargetSendIoctlSynchronously failed 0x%x\n", status));                
        goto ExitAndFree;
    }

    preparsedData = (PHIDP_PREPARSED_DATA) ExAllocatePoolWithTag(
        NonPagedPoolNx, collectionInformation.DescriptorSize, 'ffly');//要collectionInformation.DescriptorSize信息

    if (preparsedData == NULL) {
        status = STATUS_INSUFFICIENT_RESOURCES;
        goto ExitAndFree;
    }

    WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&outputDescriptor,
                                      (PVOID) preparsedData,//real buffer
                                      collectionInformation.DescriptorSize);

    status = WdfIoTargetSendIoctlSynchronously(hidTarget,
                                  NULL,
                                  IOCTL_HID_GET_COLLECTION_DESCRIPTOR,
                                  NULL,
                                  &outputDescriptor,//实际是preparsedData
                                  NULL,
                                  NULL);

    if (!NT_SUCCESS(status)) {
        KdPrint(("FireFly: WdfIoTargetSendIoctlSynchronously failed 0x%x\n", status));                
        goto ExitAndFree;
    }

    //
    // Now get the capabilities.
    //
    RtlZeroMemory(&caps, sizeof(HIDP_CAPS));

    status = HidP_GetCaps(preparsedData, &caps);//取caps

    if (!NT_SUCCESS(status)) {

        goto ExitAndFree;
    }

    //
    // Create a report to send to the device.
    //
    report = (PCHAR) ExAllocatePoolWithTag(
        NonPagedPoolNx, caps.FeatureReportByteLength, 'ffly');//要caps.FeatureReportByteLength信息

    if (report == NULL) {
        goto ExitAndFree;
    }

    //
    // Start with a zeroed report. If we are disabling the feature, this might
    // be all we need to do.
    //
    RtlZeroMemory(report, caps.FeatureReportByteLength);
    status = STATUS_SUCCESS;

    if (EnableFeature) {

        //
        // Edit the report to reflect the enabled feature
        //
        usage = FeatureId;
        usageLength = 1;

        status = HidP_SetUsages(
            HidP_Feature,
            PageId,
            0,
            &usage, // pointer to the usage list
            &usageLength, //1， number of usages in the usage list
            preparsedData,
            report, //目前全是0，等这个函数设置呢
            caps.FeatureReportByteLength
            );
        if (!NT_SUCCESS(status)) {
            KdPrint(("FireFly: HidP_SetUsages failed 0x%x\n", status));                
            goto ExitAndFree;
        }
    }

    WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&inputDescriptor,
                                      report,//real buffer
                                      caps.FeatureReportByteLength);
    status = WdfIoTargetSendIoctlSynchronously(hidTarget,
                                  NULL,
                                  IOCTL_HID_SET_FEATURE,
                                  &inputDescriptor, //实际是&report
                                  NULL,
                                  NULL,
                                  NULL);
    if (!NT_SUCCESS(status)) {
        KdPrint(("FireFly: WdfIoTargetSendIoctlSynchronously failed 0x%x\n", status));                
        goto ExitAndFree;
    }

ExitAndFree:

    if (preparsedData != NULL) {
        ExFreePool(preparsedData);
        preparsedData = NULL;
    }

    if (report != NULL) {
        ExFreePool(report);
        report = NULL;
    }

    if (hidTarget != NULL) {
        WdfObjectDelete(hidTarget);
    }

    return status;
}
