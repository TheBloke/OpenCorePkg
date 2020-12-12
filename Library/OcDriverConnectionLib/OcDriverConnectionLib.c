/** @file
  Copyright (C) 2020, vit9696. All rights reserved.

  All rights reserved.

  This program and the accompanying materials
  are licensed and made available under the terms and conditions of the BSD License
  which accompanies this distribution.  The full text of the license may be found at
  http://opensource.org/licenses/bsd-license.php

  THE PROGRAM IS DISTRIBUTED UNDER THE BSD LICENSE ON AN "AS IS" BASIS,
  WITHOUT WARRANTIES OR REPRESENTATIONS OF ANY KIND, EITHER EXPRESS OR IMPLIED.
**/

#include <Library/MemoryAllocationLib.h>
#include <Library/DebugLib.h>
#include <Library/OcDriverConnectionLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Protocol/PlatformDriverOverride.h>

#include <Library/DevicePathLib.h>
#include <Library/BaseLib.h>
#include <IHandle.h>

//
// NULL-terminated list of driver handles that will be served by EFI_PLATFORM_DRIVER_OVERRIDE_PROTOCOL.
//
STATIC EFI_HANDLE *mPriorityDrivers;

//
// Saved original EFI_PLATFORM_DRIVER_OVERRIDE_PROTOCOL.GetDriver when doing override.
//
STATIC EFI_PLATFORM_DRIVER_OVERRIDE_GET_DRIVER mOrgPlatformGetDriver;

STATIC
EFI_STATUS
EFIAPI
OcPlatformGetDriver (
  IN     EFI_PLATFORM_DRIVER_OVERRIDE_PROTOCOL          *This,
  IN     EFI_HANDLE                                     ControllerHandle,
  IN OUT EFI_HANDLE                                     *DriverImageHandle
  )
{
  EFI_HANDLE     *HandlePtr;
  BOOLEAN        FoundLast;

  if (ControllerHandle == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  //
  // We have no custom overrides.
  //
  if (mPriorityDrivers == NULL || mPriorityDrivers[0] == NULL) {
    //
    // Forward request to the original driver if we have it.
    //
    if (mOrgPlatformGetDriver != NULL) {
      return mOrgPlatformGetDriver (This, ControllerHandle, DriverImageHandle);
    }

    //
    // Report not found for the first request.
    //
    if (*DriverImageHandle == NULL) {
      return EFI_NOT_FOUND;
    }

    //
    // Report invalid parameter for the handle we could not have returned.
    //
    return EFI_INVALID_PARAMETER;
  }

  //
  // Handle first driver in the overrides.
  //
  if (*DriverImageHandle == NULL) {
    *DriverImageHandle = mPriorityDrivers[0];
    return EFI_SUCCESS;
  }

  //
  // Otherwise lookup the current driver in the list.
  //

  FoundLast = FALSE;

  for (HandlePtr = &mPriorityDrivers[0]; HandlePtr[0] != NULL; ++HandlePtr) {
    //
    // Found driver in the list, return next one.
    //
    if (HandlePtr[0] == *DriverImageHandle) {
      *DriverImageHandle = HandlePtr[1];
      //
      // Next driver is not last, return it.
      //
      if (*DriverImageHandle != NULL) {
        return EFI_SUCCESS;
      }

      //
      // Next driver is last, exit the loop.
      //
      FoundLast = TRUE;
      break;
    }
  }

  //
  // We have no original protocol.
  //
  if (mOrgPlatformGetDriver == NULL) {
    //
    // Finalise the list by reporting not found.
    //
    if (FoundLast) {
      return EFI_NOT_FOUND;
    }

    //
    // Error as the passed handle is not valid.
    //
    return EFI_INVALID_PARAMETER;
  }

  //
  // Forward the call to the original driver:
  // - if FoundLast, then it is starting to iterating original list and DriverImageHandle
  //   is nulled above.
  // - otherwise, then it is iterating the original list or is invalid.
  //
  return mOrgPlatformGetDriver (This, ControllerHandle, DriverImageHandle);
}

STATIC
EFI_STATUS
EFIAPI
OcPlatformGetDriverPath (
  IN     EFI_PLATFORM_DRIVER_OVERRIDE_PROTOCOL          *This,
  IN     EFI_HANDLE                                     ControllerHandle,
  IN OUT EFI_DEVICE_PATH_PROTOCOL                       **DriverImagePath
  )
{
  return EFI_UNSUPPORTED;
}

STATIC
EFI_STATUS
EFIAPI
OcPlatformDriverLoaded (
  IN EFI_PLATFORM_DRIVER_OVERRIDE_PROTOCOL          *This,
  IN EFI_HANDLE                                     ControllerHandle,
  IN EFI_DEVICE_PATH_PROTOCOL                       *DriverImagePath,
  IN EFI_HANDLE                                     DriverImageHandle
  )
{
  return EFI_UNSUPPORTED;
}

STATIC
EFI_PLATFORM_DRIVER_OVERRIDE_PROTOCOL
mOcPlatformDriverOverrideProtocol = {
  OcPlatformGetDriver,
  OcPlatformGetDriverPath,
  OcPlatformDriverLoaded
};

EFI_STATUS
OcRegisterDriversToHighestPriority (
  IN EFI_HANDLE  *PriorityDrivers
  )
{
  EFI_PLATFORM_DRIVER_OVERRIDE_PROTOCOL  *PlatformDriverOverride;
  EFI_STATUS                             Status;

  ASSERT (PriorityDrivers != NULL);

  mPriorityDrivers = PriorityDrivers;
  Status = gBS->LocateProtocol (
    &gEfiPlatformDriverOverrideProtocolGuid,
    NULL,
    (VOID **) &PlatformDriverOverride
    );

  if (!EFI_ERROR(Status)) {
    mOrgPlatformGetDriver = PlatformDriverOverride->GetDriver;
    PlatformDriverOverride->GetDriver = OcPlatformGetDriver;
    return Status;
  }

  return gBS->InstallMultipleProtocolInterfaces (
    &gImageHandle,
    &gEfiPlatformDriverOverrideProtocolGuid,
    &mOcPlatformDriverOverrideProtocol,
    NULL
    );
}

EFI_STATUS
OcConnectDrivers (
  VOID
  )
{
  EFI_STATUS                           Status;
  UINTN                                HandleCount;
  EFI_HANDLE                           *HandleBuffer;
  UINTN                                DeviceIndex;
  UINTN                                HandleIndex;
  UINTN                                ProtocolIndex;
  UINTN                                InfoIndex;
  EFI_GUID                             **ProtocolGuids;
  UINTN                                ProtocolCount;
  EFI_OPEN_PROTOCOL_INFORMATION_ENTRY  *ProtocolInfos;
  UINTN                                ProtocolInfoCount;
  BOOLEAN                              Connect;
  CHAR16                               *UnicodeDevicePath;
  IHANDLE                              *TemporaryIHandle;

  //
  // We locate only handles with device paths as connecting other handles
  // will crash APTIO IV.
  //
  Status = gBS->LocateHandleBuffer (
    ByProtocol,
    &gEfiDevicePathProtocolGuid,
    NULL,
    &HandleCount,
    &HandleBuffer
    );

  if (EFI_ERROR (Status)) {
    return Status;
  }

  //
  // Veto handles based on device path
  //
  // This technically only invalidates the signature on the vetoed handles, which is the most performant and reliable method of vetoing them. If handles were being iterated via linked list references instead of via array index, the commented out RemoveEntryList call would be necessary as well.
  // Background: The Gigabyte X299X Designare 10G motherboard has a nasty firmware bug that hard-resets the motherboard and triggers a boot failure (resetting various settings) when driver connection to a specific child handle is attempted. To avoid this issue, we compute the unicode device paths for all handles returned by the previous query and compare them to a list of vetoed device paths which must be removed, as recursive ConnectController calls reaching/impacting any of these device paths would trigger the bug.
  // Note that the PNP0F03 & PNP0C08 device paths may not trigger the bug reliably (or possibly at all). However, it's safer to just veto these ones as well.
  //
  for (DeviceIndex = 0; DeviceIndex < HandleCount; ++DeviceIndex) {
    //DEBUG ((DEBUG_INFO, "OCDC: Getting DP [i=%d/%d]\n", DeviceIndex, HandleCount));

    // Get device path for this handle and convert it to text
    UnicodeDevicePath = ConvertDevicePathToText (DevicePathFromHandle (HandleBuffer[DeviceIndex]), FALSE, FALSE);
    
    if (UnicodeDevicePath != NULL) {
      //DEBUG ((DEBUG_INFO, "OCDC: DP [i=%d/%d] is %s\n", DeviceIndex, HandleCount, UnicodeDevicePath));

      // Check if this device path matches a vetoed device path
      if ( (StrCmp(UnicodeDevicePath, L"PciRoot(0x0)") == 0)
        || (StrCmp(UnicodeDevicePath, L"PciRoot(0x0)/Pci(0x1F,0x0)") == 0)
        || (StrCmp(UnicodeDevicePath, L"PciRoot(0x0)/Pci(0x1F,0x0)/Acpi(PNP0303,0x0)") == 0)
        || (StrCmp(UnicodeDevicePath, L"PciRoot(0x0)/Pci(0x1F,0x0)/Acpi(PNP0F03,0x0)") == 0)
        || (StrCmp(UnicodeDevicePath, L"PciRoot(0x0)/Pci(0x1F,0x0)/Acpi(PNP0C08,0x0)") == 0)
        || (StrCmp(UnicodeDevicePath, L"PciRoot(0x0)/Pci(0x1F,0x0)/Acpi(PNP0C08,0x1)") == 0)
        ) {
        // Device path matches a vetoed device path!
        DEBUG ((DEBUG_INFO, "OCDC: DP [i=%d/%d] %s matches a vetoed device path - vetoing handle\n", DeviceIndex, HandleCount, UnicodeDevicePath));
        
        // Loosely based on UDK/MdeModulePkg/Universal/HiiDatabaseDxe/Database.c:3682
        TemporaryIHandle = (IHANDLE *)HandleBuffer[DeviceIndex];
        
        // Invalidate signature
        TemporaryIHandle->Signature = 0;

        // Only necessary if iterating via linked list, otherwise you can do without this
        // RemoveEntryList(&TemporaryIHandle->AllHandles);
        
        // Not sure if/where this is appropriate, technically you could free the memory this way, but with the pointers intact, it should get freed when HandleBuffer is freed...
        // FreePool (TemporaryIHandle);
      }

      FreePool (UnicodeDevicePath);
    }
  }

  for (DeviceIndex = 0; DeviceIndex < HandleCount; ++DeviceIndex) {
    //DEBUG ((DEBUG_INFO, "OCDC: Getting DP [i=%d/%d] 0x%016x\n", DeviceIndex, HandleCount, HandleBuffer[DeviceIndex]));

    //
    // Only connect parent handles as we connect recursively.
    // This improves the performance by more than 30 seconds
    // with drives installed into Marvell SATA controllers on APTIO IV.
    //
    Connect = TRUE;
    for (HandleIndex = 0; HandleIndex < HandleCount && Connect; ++HandleIndex) {
      //
      // Retrieve the list of all the protocols on each handle
      //
      Status = gBS->ProtocolsPerHandle (
        HandleBuffer[HandleIndex],
        &ProtocolGuids,
        &ProtocolCount
        );

      if (EFI_ERROR (Status)) {
        continue;
      }

      for (ProtocolIndex = 0; ProtocolIndex < ProtocolCount && Connect; ++ProtocolIndex) {
        //
        // Retrieve the list of agents that have opened each protocol
        //
        Status = gBS->OpenProtocolInformation (
          HandleBuffer[HandleIndex],
          ProtocolGuids[ProtocolIndex],
          &ProtocolInfos,
          &ProtocolInfoCount
          );

        if (!EFI_ERROR (Status)) {
          for (InfoIndex = 0; InfoIndex < ProtocolInfoCount && Connect; ++InfoIndex) {
            if (ProtocolInfos[InfoIndex].ControllerHandle == HandleBuffer[DeviceIndex]
              && (ProtocolInfos[InfoIndex].Attributes & EFI_OPEN_PROTOCOL_BY_CHILD_CONTROLLER) == EFI_OPEN_PROTOCOL_BY_CHILD_CONTROLLER) {
              Connect = FALSE;
            }
          }
          FreePool (ProtocolInfos);
        }
      }

      FreePool (ProtocolGuids);
    }

    if (Connect) {
      //
      // We connect all handles to all drivers as otherwise fs drivers may not be seen.
      //
      gBS->ConnectController (HandleBuffer[DeviceIndex], NULL, NULL, TRUE);
    }
  }

  FreePool (HandleBuffer);

  return EFI_SUCCESS;
}
