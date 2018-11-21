/** @file

  Implements the UEFI Front Page (Settings Menu).

  Copyright (c) 2015 - 2018, Microsoft Corporation.
  
  All rights reserved.
  Redistribution and use in source and binary forms, with or without 
  modification, are permitted provided that the following conditions are met:
  1. Redistributions of source code must retain the above copyright notice,
  this list of conditions and the following disclaimer.
  2. Redistributions in binary form must reproduce the above copyright notice,
  this list of conditions and the following disclaimer in the documentation
  and/or other materials provided with the distribution.

  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
  ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
  WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
  IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT,
  INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
  BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, 
  DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
  LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE
  OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF 
  ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

**/

#include <Uefi.h>
#include "FrontPage.h"
#include "FrontPageUi.h"
#include "FrontPageConfigAccess.h"

#include <IndustryStandard/SmBios.h>

#include <Guid/GlobalVariable.h>
#include <Guid/MsBootMenuGuid.h>
#include <Guid/MdeModuleHii.h>
#include <Guid/DebugImageInfoTable.h>

#include <Pi/PiFirmwareFile.h>

#include <Protocol/GraphicsOutput.h>
#include <Protocol/Smbios.h>
#include <Protocol/OnScreenKeyboard.h>
#include <Protocol/SimpleWindowManager.h>
#include <Protocol/FirmwareManagement.h>

#include <Library/DebugLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DevicePathLib.h>
#include <Library/HiiLib.h>
#include <Library/PrintLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Library/UefiBootManagerLib.h>
#include <Library/DxeServicesLib.h>
#include <Library/BmpSupportLib.h>
#include <Library/BootGraphicsProviderLib.h>
#include <Library/BootGraphicsLib.h>
#include <Library/GraphicsConsoleHelperLib.h>
#include <Library/MsUiThemeLib.h>
#include <Library/ResetUtilityLib.h>
#include <Library/MsColorTableLib.h>
#include <Library/MsNVBootReasonLib.h>

#include <MsDisplayEngine.h>
#include <UIToolKit/SimpleUIToolKit.h>

#define FP_OSK_WIDTH_PERCENT        75      // On-screen keyboard is 75% the width of the screen.

UINTN       mCallbackKey;
EFI_HANDLE  mImageHandle;

// Protocols.
//
EFI_GRAPHICS_OUTPUT_PROTOCOL        *mGop;
EFI_HII_FONT_PROTOCOL               *mFont;

// UI Elements.
//
UINT32  mTitleBarWidth, mTitleBarHeight;
UINT32  mMasterFrameWidth, mMasterFrameHeight;
ListBox *mTopMenu;
BOOLEAN mShowFullMenu = FALSE;      // By default we won't show the full FrontPage menu (requires validation if there's a system password).

// Master Frame - Form Notifications.
//
UINT32                            mCurrentFormIndex;
EFI_EVENT                         mMasterFrameNotifyEvent;
DISPLAY_ENGINE_SHARED_STATE       mDisplayEngineState;
BOOLEAN                           mTerminateFrontPage = FALSE;
BOOLEAN                           mResetRequired;
FRONT_PAGE_AUTH_TOKEN_PROTOCOL   *mFrontPageAuthTokenProtocol = NULL;
DFCI_AUTHENTICATION_PROTOCOL     *mAuthProtocol = NULL;
EFI_HII_CONFIG_ROUTING_PROTOCOL  *mHiiConfigRouting;
DFCI_SETTING_ACCESS_PROTOCOL     *mSettingAccess;
DFCI_AUTH_TOKEN                   mAuthToken;

extern EFI_GUID  gMsEventMasterFrameNotifyGroupGuid;

//
// Boot video resolution and text mode.
//
UINT32    mBootHorizontalResolution    = 0;
UINT32    mBootVerticalResolution      = 0;

EFI_FORM_BROWSER2_PROTOCOL          *mFormBrowser2;
MS_ONSCREEN_KEYBOARD_PROTOCOL       *mOSKProtocol;
MS_SIMPLE_WINDOW_MANAGER_PROTOCOL   *mSWMProtocol;

// Map Top Menu entries to HII Form IDs.
//
#define UNUSED_INDEX    (UINT16)-1
struct
{
    UINT16          FullMenuIndex;      // Master Frame full menu index.
    UINT16          LimitedMenuIndex;   // Master Frame limited menu index.
    EFI_STRING_ID   MenuString;         // Master Frame menu string.
    EFI_GUID        FormSetGUID;        // HII FormSet GUID.
    EFI_FORM_ID     FormId;             // HII Form ID.

} mFormMap[] =
{
//    Index (Full)  Index (Limited)     String                                      Formset Guid                       Form ID
//-------------------------------------------------------------------------------------------------------------------------------------------------------------------
    { 0,            0,                  STRING_TOKEN (STR_MF_MENU_OP_PCINFO),       FRONT_PAGE_CONFIG_FORMSET_GUID,    FRONT_PAGE_FORM_ID_PCINFO   },  // PC info
    { 1,            UNUSED_INDEX,       STRING_TOKEN (STR_MF_MENU_OP_BOOTORDER),    MS_BOOT_MENU_FORMSET_GUID,         MS_BOOT_ORDER_FORM_ID       },  // Boot Order
    { 2,            1,                  STRING_TOKEN(STR_MF_MENU_OP_EXIT),          FRONT_PAGE_CONFIG_FORMSET_GUID,    FRONT_PAGE_FORM_ID_EXIT     }   // Exit
};

// Frontpage form set GUID
//
EFI_GUID gMsFrontPageConfigFormSetGuid = FRONT_PAGE_CONFIG_FORMSET_GUID;

#pragma pack(1)

///
/// HII specific Vendor Device Path definition.
///
typedef struct
{
    VENDOR_DEVICE_PATH             VendorDevicePath;
    EFI_DEVICE_PATH_PROTOCOL       End;
} HII_VENDOR_DEVICE_PATH;

#pragma pack()

FRONT_PAGE_CALLBACK_DATA  mFrontPagePrivate = {
    FRONT_PAGE_CALLBACK_DATA_SIGNATURE,
    NULL,
    NULL,
    NULL,
    {
        ExtractConfig,    // Lives in FrontPageConfigAccess.c
        RouteConfig,      // Lives in FrontPageConfigAccess.c
        UiCallback        // Lives in FrontPageUi.c
    }
};

HII_VENDOR_DEVICE_PATH  mFrontPageHiiVendorDevicePath = {
    {
        {
            HARDWARE_DEVICE_PATH,
            HW_VENDOR_DP,
            {
                (UINT8) (sizeof (VENDOR_DEVICE_PATH)),
                (UINT8) ((sizeof (VENDOR_DEVICE_PATH)) >> 8)
            }
        },
        FRONT_PAGE_CONFIG_FORMSET_GUID
    },
    {
        END_DEVICE_PATH_TYPE,
        END_ENTIRE_DEVICE_PATH_SUBTYPE,
        {
            (UINT8) (END_DEVICE_PATH_LENGTH),
            (UINT8) ((END_DEVICE_PATH_LENGTH) >> 8)
        }
    }
};

EFI_STATUS GetAndDisplayBitmap(EFI_GUID *FileGuid, UINTN XCoord, BOOLEAN XCoordAdj);

EFI_STATUS GetAuthToken(CHAR16 *PasswordBuffer);

/**

  Acquire the string associated with the Index from smbios structure and return it.
  The caller is responsible for free the string buffer.

  @param    OptionalStrStart  The start position to search the string
  @param    Index             The index of the string to extract
  @param    String            The string that is extracted

  @retval   EFI_SUCCESS       The function returns EFI_SUCCESS if found the string.
  @retval   EFI_NOT_FOUND     If the target string is not found.

**/
STATIC
EFI_STATUS
GetOptionalStringByIndex (
  IN      CHAR8                   *OptionalStrStart,
  IN      UINT8                   Index,
  OUT     CHAR16                  **String
  )
{
  UINTN          StrSize;

  if (Index == 0) {
    *String = AllocateZeroPool (sizeof (CHAR16));
    return EFI_SUCCESS;
  }

  StrSize = 0;
  do {
    Index--;
    OptionalStrStart += StrSize;
    StrSize           = AsciiStrSize (OptionalStrStart);
  } while (OptionalStrStart[StrSize] != 0 && Index != 0);

  if ((Index != 0) || (StrSize == 1)) {
    //
    // Meet the end of strings set but Index is non-zero, or
    // Find an empty string
    //
    FreePool(*String);
    *String = NULL;
    return EFI_NOT_FOUND;
  } else {
    *String = AllocatePool (StrSize * sizeof (CHAR16));
    AsciiStrToUnicodeStrS (OptionalStrStart, *String, StrSize);
  }

  return EFI_SUCCESS;
}

/**
  Updates HII display strings based on associated EFI variable state.

@param[in]  HiiHandle       - Hii database handle.

@retval     EFI_SUCCESS     Updated display strings.

**/
STATIC
EFI_STATUS
UpdateDisplayStrings (IN    EFI_HII_HANDLE  HiiHandle)
{
  EFI_STATUS                Status = EFI_SUCCESS;
  CHAR16                    *NewString;

  EFI_SMBIOS_PROTOCOL       *SmbiosProtocol;
  EFI_SMBIOS_HANDLE         SmbiosHandle = SMBIOS_HANDLE_PI_RESERVED;
  EFI_SMBIOS_TABLE_HEADER   *Record;
  SMBIOS_TYPE               Type;
  SMBIOS_TABLE_TYPE1        *Type1Record;
  SMBIOS_TABLE_TYPE3        *Type3Record;

  Status = gBS->LocateProtocol(&gEfiSmbiosProtocolGuid, NULL, (VOID**)&SmbiosProtocol);
  if (EFI_ERROR(Status)) {
    DEBUG((EFI_D_ERROR, "Could not locate SMBIOS protocol.  %r\n", Status));
    return Status;
  }

  SmbiosHandle = SMBIOS_HANDLE_PI_RESERVED; // Reset handle
  Type = SMBIOS_TYPE_SYSTEM_INFORMATION;    // Smbio type1
  Status = SmbiosProtocol->GetNext(SmbiosProtocol, &SmbiosHandle, &Type, &Record, NULL);
  if (!EFI_ERROR(Status))
  {
    Type1Record = (SMBIOS_TABLE_TYPE1 *) Record;
    Status = GetOptionalStringByIndex ((CHAR8*)((UINT8*)Type1Record + Type1Record->Hdr.Length), Type1Record->ProductName, &NewString);
    if (!EFI_ERROR(Status)) {
      HiiSetString (HiiHandle, STRING_TOKEN (STR_INF_VIEW_PC_MODEL_VALUE), NewString, NULL);
      FreePool (NewString);
    }

    NewString = AllocatePool((GUID_STRING_LENGTH + 1) * sizeof(CHAR16));
    if (NewString != NULL)
    {
      SetMem(NewString, (GUID_STRING_LENGTH + 1) * sizeof(CHAR16), 0);
      UnicodeSPrint (NewString,
                    (GUID_STRING_LENGTH * sizeof(CHAR16)),
                    L"%g",
                    (EFI_GUID *)&Type1Record->Uuid
                    );
      HiiSetString (HiiHandle, STRING_TOKEN (STR_INF_VIEW_PC_UUID_VALUE), NewString, NULL);
      FreePool(NewString);
    }
  }

  SmbiosHandle = SMBIOS_HANDLE_PI_RESERVED; // Reset handle
  Type = SMBIOS_TYPE_SYSTEM_ENCLOSURE;      // Smbio type3
  Status = SmbiosProtocol->GetNext(SmbiosProtocol, &SmbiosHandle, &Type, &Record, NULL);
  if (!EFI_ERROR(Status))
  {
    Type3Record = (SMBIOS_TABLE_TYPE3 *) Record;
    Status = GetOptionalStringByIndex ((CHAR8*)((UINT8*)Type3Record + Type3Record->Hdr.Length), Type3Record->AssetTag, &NewString);
    if (!EFI_ERROR(Status)) {
      HiiSetString (HiiHandle, STRING_TOKEN (STR_INF_VIEW_PC_ASSET_TAG_VALUE), NewString, NULL);
      FreePool (NewString);
    }

    Status = GetOptionalStringByIndex ((CHAR8*)((UINT8*)Type3Record + Type3Record->Hdr.Length), Type3Record->SerialNumber, &NewString);
    if (!EFI_ERROR(Status)) {
      HiiSetString (HiiHandle, STRING_TOKEN (STR_INF_VIEW_PC_SERIALNUM_VALUE), NewString, NULL);
      FreePool (NewString);
    }
  }

  return Status;
}

/**
Function to populate the PC INFO firmware version form with the current fw versions
found using FMP.

**/
VOID
UpdateFormWithFirmwareVersions(IN EFI_HII_HANDLE  HiiHandle) {
  EFI_STATUS                    Status;
  VOID                         *StartOpCodeHandle;
  VOID                         *EndOpCodeHandle = NULL;
  EFI_IFR_GUID_LABEL           *StartLabel;
  EFI_IFR_GUID_LABEL           *EndLabel;
  EFI_STRING_ID                 StringId;
  EFI_STRING_ID                 StringId1;

  EFI_FIRMWARE_MANAGEMENT_PROTOCOL              **FmpList;
  UINTN                                         FmpCount;
  UINTN                                         Index;
  EFI_FIRMWARE_MANAGEMENT_PROTOCOL              *Fmp;
  UINTN                                         DescriptorSize;
  EFI_FIRMWARE_IMAGE_DESCRIPTOR                 *FmpImageInfoBuf;
  UINT8                                         FmpImageInfoCount;
  UINT32                                        FmpImageInfoDescriptorVer;
  UINTN                                         ImageInfoSize;
  UINT32                                        PackageVersion;
  CHAR16                                        *PackageVersionName;

  FmpImageInfoBuf = NULL;
  Fmp = NULL;
  FmpList = NULL;
  PackageVersionName = NULL;

  do {
    //
    // Init OpCode Handle and Allocate space for creation of UpdateData Buffer
    //
    StartOpCodeHandle = HiiAllocateOpCodeHandle();
    if (StartOpCodeHandle == NULL)
    {
      ASSERT(StartOpCodeHandle != NULL);
      break;
    }

    EndOpCodeHandle = HiiAllocateOpCodeHandle();
    if (EndOpCodeHandle == NULL)
    {
      ASSERT(EndOpCodeHandle != NULL);
      break;
    }

    //
    // Create Hii Extend Label OpCode as the start opcode
    //
    StartLabel = (EFI_IFR_GUID_LABEL *)HiiCreateGuidOpCode(StartOpCodeHandle, &gEfiIfrTianoGuid, NULL, sizeof(EFI_IFR_GUID_LABEL));
    StartLabel->ExtendOpCode = EFI_IFR_EXTEND_OP_LABEL;

    //
    // Create Hii Extend Label OpCode as the end opcode
    //
    EndLabel = (EFI_IFR_GUID_LABEL *)HiiCreateGuidOpCode(EndOpCodeHandle, &gEfiIfrTianoGuid, NULL, sizeof(EFI_IFR_GUID_LABEL));
    EndLabel->ExtendOpCode = EFI_IFR_EXTEND_OP_LABEL;

    StartLabel->Number = LABEL_PCINFO_FW_VERSION_TAG_START;
    EndLabel->Number = LABEL_PCINFO_FW_VERSION_TAG_END;

    //
    // Get all FMP instances and then use the descriptor to get string name and version
    //
    Status = EfiLocateProtocolBuffer(&gEfiFirmwareManagementProtocolGuid, &FmpCount, (VOID*)&FmpList);
    if (EFI_ERROR(Status))
    {
      DEBUG((DEBUG_ERROR, "EfiLocateProtocolBuffer(gEfiFirmwareManagementProtocolGuid) returned error.  %r \n", Status));
      break;
    }

    for (Index = 0; Index < FmpCount; Index++)
    {
      Fmp = (EFI_FIRMWARE_MANAGEMENT_PROTOCOL*)(FmpList[Index]);
      //get the GetImageInfo for the FMP

      ImageInfoSize = 0;
      //
      // get necessary descriptor size
      // this should return TOO SMALL
      Status = Fmp->GetImageInfo(
        Fmp,                       // FMP Pointer
        &ImageInfoSize,               // Buffer Size (in this case 0)
        NULL,                         // NULL so we can get size
        &FmpImageInfoDescriptorVer,   // DescriptorVersion
        &FmpImageInfoCount,           // DescriptorCount
        &DescriptorSize,              // DescriptorSize
        &PackageVersion,              // PackageVersion
        &PackageVersionName           // PackageVersionName
        );

      if (Status != EFI_BUFFER_TOO_SMALL) {
        DEBUG((DEBUG_ERROR, "%a - Unexpected Failure in GetImageInfo.  Status = %r\n", __FUNCTION__, Status));
        continue;
      }

      FmpImageInfoBuf = NULL;
      FmpImageInfoBuf = AllocateZeroPool(ImageInfoSize);
      if (FmpImageInfoBuf == NULL) {
        DEBUG((DEBUG_ERROR, "%a - Failed to get memory for descriptors.\n", __FUNCTION__));
        continue;
      }

      PackageVersionName = NULL;
      Status = Fmp->GetImageInfo(
        Fmp,
        &ImageInfoSize,               // ImageInfoSize
        FmpImageInfoBuf,              // ImageInfo
        &FmpImageInfoDescriptorVer,   // DescriptorVersion
        &FmpImageInfoCount,           // DescriptorCount
        &DescriptorSize,              // DescriptorSize
        &PackageVersion,              // PackageVersion
        &PackageVersionName           // PackageVersionName
        );

      if (EFI_ERROR(Status)) {
        DEBUG((DEBUG_ERROR, "%a - Failure in GetImageInfo.  Status = %r\n", __FUNCTION__, Status));
        goto FmpCleanUp;
      }

      if (FmpImageInfoCount == 0)
      {
        DEBUG((DEBUG_INFO, "%a - No Image Info descriptors.\n", __FUNCTION__));
        goto FmpCleanUp;
      }

      if (FmpImageInfoCount > 1)
      {
        DEBUG((DEBUG_INFO, "%a - Found %d descriptors.  For FrontPage we only show the 1st descriptor.\n", __FUNCTION__, FmpImageInfoCount));
      }

      if ((StringId = HiiSetString(HiiHandle, 0, FmpImageInfoBuf->ImageIdName, NULL)) == 0)
      {
        DEBUG((DEBUG_ERROR, "%a - Failed to set string for fmp ImageIdName: %s. \n", __FUNCTION__, FmpImageInfoBuf->ImageIdName));
        goto FmpCleanUp;
      }

      if ((StringId1 = HiiSetString(HiiHandle, 0, FmpImageInfoBuf->VersionName, NULL)) == 0)
      {
        DEBUG((DEBUG_ERROR, "%a - Failed to set string for fmp VersionName: %s. \n", __FUNCTION__, FmpImageInfoBuf->VersionName));
        goto FmpCleanUp;
      }

      // Create a Subtitle OpCode to group the Firmware version "key-value" pair that follows.
      //
      HiiCreateSubTitleOpCode (StartOpCodeHandle,
                               STRING_TOKEN(STR_NULL_STRING),
                               STRING_TOKEN(STR_NULL_STRING),
                               EFI_IFR_FLAGS_HORIZONTAL,
                               0
                              );

      HiiCreateTextOpCode(StartOpCodeHandle,
                          StringId,
                          STRING_TOKEN(STR_NULL_STRING),
                          STRING_TOKEN(STR_NULL_STRING)
                         );

      HiiCreateTextOpCode(StartOpCodeHandle,
                          StringId1,
                          STRING_TOKEN(STR_NULL_STRING),
                          STRING_TOKEN(STR_NULL_STRING)
                         );

      // Create empty 3rd text Opcode add an additional column to the display grid, thus moving the firmware version to the left (better alignment).
      //
      HiiCreateTextOpCode(StartOpCodeHandle,
                          STRING_TOKEN(STR_NULL_STRING),
                          STRING_TOKEN(STR_NULL_STRING),
                          STRING_TOKEN(STR_NULL_STRING)
                         );

    FmpCleanUp:
      //clean up -
      FreePool(FmpImageInfoBuf);
      FmpImageInfoBuf = NULL;
      if (PackageVersionName != NULL)
      {
        FreePool(PackageVersionName);
        PackageVersionName = NULL;
      }
    } //for loop for all fmp handles

    //Free up the FmpList of pointers
    if (FmpList != NULL) { FreePool(FmpList); }

    Status = HiiUpdateForm(HiiHandle,     // HII handle
      &gMsFrontPageConfigFormSetGuid,     // Formset GUID
      FRONT_PAGE_FORM_ID_PCINFO,          // Form ID
      StartOpCodeHandle,                  // Label for where to insert opcodes
      EndOpCodeHandle                     // Replace data
      );
  } while (FALSE);

  if (StartOpCodeHandle != NULL) {
    HiiFreeOpCodeHandle(StartOpCodeHandle);
  }

  if (EndOpCodeHandle != NULL) {
    HiiFreeOpCodeHandle(EndOpCodeHandle);
  }
}

/**
  Initialize HII information for the FrontPage


  @param InitializeHiiData    TRUE if HII elements need to be initialized.

  @retval  EFI_SUCCESS        The operation is successful.
  @retval  EFI_DEVICE_ERROR   If the dynamic opcode creation failed.

**/
EFI_STATUS
InitializeFrontPage (
                    IN BOOLEAN    InitializeHiiData
                    )
{
    EFI_STATUS                  Status = EFI_SUCCESS;
    CHAR16                      *StringBuffer;
    EFI_HII_HANDLE              HiiHandle;

    StringBuffer = NULL;

    if (InitializeHiiData)
    {

        mCallbackKey  = 0;

        //
        // Locate Hii relative protocols
        //
        Status = gBS->LocateProtocol (&gEfiFormBrowser2ProtocolGuid, NULL, (VOID **) &mFormBrowser2);
        if (EFI_ERROR (Status))
        {
            return Status;
        }
        Status = gBS->LocateProtocol (&gEfiHiiConfigRoutingProtocolGuid, NULL, (VOID **) &mHiiConfigRouting);
        if (EFI_ERROR (Status))
        {
            return Status;
        }

        //
        // Install Device Path Protocol and Config Access protocol to driver handle
        //
        Status = gBS->InstallMultipleProtocolInterfaces (
                                                        &mFrontPagePrivate.DriverHandle,
                                                        &gEfiDevicePathProtocolGuid,
                                                        &mFrontPageHiiVendorDevicePath,
                                                        &gEfiHiiConfigAccessProtocolGuid,
                                                        &mFrontPagePrivate.ConfigAccess,
                                                        NULL
                                                        );

        ASSERT_EFI_ERROR (Status);

        //
        // Publish our HII data
        //
        mFrontPagePrivate.HiiHandle = HiiAddPackages (
                                                     &gMsFrontPageConfigFormSetGuid,
                                                     mFrontPagePrivate.DriverHandle,
                                                     FrontPageVfrBin,
                                                     FrontPageStrings,
                                                     NULL
                                                     );
        if (mFrontPagePrivate.HiiHandle == NULL)
        {
            return EFI_OUT_OF_RESOURCES;
        }
    }

    HiiHandle = mFrontPagePrivate.HiiHandle;

    // Update PC information display strings from EFI variables.
    //
    UpdateDisplayStrings (HiiHandle);
    UpdateFormWithFirmwareVersions(HiiHandle);

    return Status;
}


/**
  Uninitialize HII information for the FrontPage


  @param InitializeHiiData    TRUE if HII elements need to be initialized.

  @retval  EFI_SUCCESS        The operation is successful.
  @retval  EFI_DEVICE_ERROR   If the dynamic opcode creation failed.

**/
EFI_STATUS
UninitializeFrontPage (
                      VOID
                      )
{
    EFI_STATUS Status = EFI_SUCCESS;

    //Dispose the auth token we acquired for the front page.
    if (mAuthProtocol != NULL) {
        Status = mAuthProtocol->DisposeAuthToken(mAuthProtocol, &mAuthToken);
        if (EFI_ERROR(Status)){
            DEBUG((EFI_D_ERROR, " Dispose Auth Token Failed %r\n", Status));
        }
    }

    if (NULL != mFrontPageAuthTokenProtocol) {
        Status = gBS->UninstallMultipleProtocolInterfaces(
            mImageHandle,
            &gMsFrontPageAuthTokenProtocolGuid,
            mFrontPageAuthTokenProtocol,
            NULL
            );
        FreePool (mFrontPageAuthTokenProtocol);
        mFrontPageAuthTokenProtocol = NULL;
        ASSERT_EFI_ERROR(Status);
    }

    Status = gBS->UninstallMultipleProtocolInterfaces (
                                                      mFrontPagePrivate.DriverHandle,
                                                      &gEfiDevicePathProtocolGuid,
                                                      &mFrontPageHiiVendorDevicePath,
                                                      &gEfiHiiConfigAccessProtocolGuid,
                                                      &mFrontPagePrivate.ConfigAccess,
                                                      NULL
                                                      );
    ASSERT_EFI_ERROR (Status);
    //
    // Remove our published HII data
    //
    HiiRemovePackages (mFrontPagePrivate.HiiHandle);
    if (mFrontPagePrivate.LanguageToken != NULL)
    {
        FreePool (mFrontPagePrivate.LanguageToken);
        mFrontPagePrivate.LanguageToken = (EFI_STRING_ID *)NULL;
    }

    gBS->CloseEvent(mMasterFrameNotifyEvent);

    return Status;
}


/**
  Call the browser and display the front page

  @return   Status code that will be returned by
            EFI_FORM_BROWSER2_PROTOCOL.SendForm ().

**/
EFI_STATUS
CallFrontPage (IN UINT32    FormIndex)
{
    EFI_STATUS                    Status = EFI_SUCCESS;
    UINT16  Count, Index = 0;
    EFI_BROWSER_ACTION_REQUEST    ActionRequest;
#define MAX_FORMSET_HANDLES 5
    EFI_HII_HANDLE                Handles[MAX_FORMSET_HANDLES];
    UINTN                         HandleCount;


    // Locate Boot Menu form - this should already be registered.
    //
    EFI_GUID          BootMenu = MS_BOOT_MENU_FORMSET_GUID;
    EFI_HII_HANDLE    *BootHandle  = HiiGetHiiHandles(&BootMenu);


    Handles[0] = mFrontPagePrivate.HiiHandle;
    HandleCount = 1;

    if (BootHandle != NULL)
        Handles[HandleCount++] = BootHandle[0];

    DEBUG((DEBUG_INFO,"MAX_FORMSET_HANDLES=%d, CurrentFormsetHandles=%d\n",MAX_FORMSET_HANDLES,HandleCount ));
    ASSERT(HandleCount < MAX_FORMSET_HANDLES);

    ActionRequest = EFI_BROWSER_ACTION_REQUEST_NONE;


    // Search through the form mapping table to find the form set GUID and ID corresponding to the selected index.
    //
    for (Count=0 ; Count < (sizeof(mFormMap) / sizeof(mFormMap[0])); Count++)
    {
        Index = ((FALSE == mShowFullMenu) ? mFormMap[Count].LimitedMenuIndex : mFormMap[Count].FullMenuIndex);

        if (Index == FormIndex)
        {
            break;
        }
    }

    // If we didn't find it, exit with an error.
    //
    if (Index != FormIndex)
    {
        Status = EFI_NOT_FOUND;
        goto Exit;
    }

    // Call the browser to display the selected form.
    //
    Status = mFormBrowser2->SendForm (mFormBrowser2,
                                      Handles,
                                      HandleCount,
                                      &mFormMap[Count].FormSetGUID,
                                      mFormMap[Count].FormId,
                                      (EFI_SCREEN_DESCRIPTOR *)NULL,
                                      &ActionRequest
                                     );

    // If the user selected the "Restart now" button to exit the Frontpage, set the exit flag.
    //
    if (ActionRequest == EFI_BROWSER_ACTION_REQUEST_EXIT)
    {
        mTerminateFrontPage = TRUE;
    }

    // Check whether user change any option setting which needs a reset to be effective
    //
    if (ActionRequest == EFI_BROWSER_ACTION_REQUEST_RESET)
    {
        mResetRequired = TRUE;
    }

Exit:

    return Status;
}

/**
  Creates the top-level menu in the Master Frame for selecting amongst the various HII forms.

  NOTE: Selectable menu options is dependent on whether there is a System firmware password and on whether the user knows it.


  @param OrigX          Menu's origin (x-axis).
  @param OrigY          Menu's origin (y-axis).
  @param CellWidth      Menu's width.
  @param CellHeight     Menu's height.
  @param CellTextXOffset   Menu entry text indentation.

  @retval  EFI_SUCCESS        The operation is successful.
  @retval  EFI_DEVICE_ERROR   Failed to create the menu.

**/
STATIC
ListBox*
CreateTopMenu(IN UINT32 OrigX,
              IN UINT32 OrigY,
              IN UINT32 CellWidth,
              IN UINT32 CellHeight,
              IN UINT32 CellTextXOffset)
{
    EFI_FONT_INFO   FontInfo;
    EFI_STATUS Status;

    // If no password is set, show the full menu.
    //
    // if no password is set still we need to get an auth token with a null password.
    Status = GetAuthToken(NULL);
    if ((Status == EFI_SUCCESS) && (mAuthToken != 0)){
       mShowFullMenu = TRUE;
    }

    // Create a listbox with menu options.  The contents of the menu depend on whether a system password is
    // set and whether the user entered the password correctly or not.  If the user cancels the password dialog
    // then only a limited menu is available.
    //
    //
    UINT16  Count, Index;
    UINT16  MenuOptionCount  = (sizeof(mFormMap) / sizeof(mFormMap[0]));
    UIT_LB_CELLDATA  *MenuOptions = AllocateZeroPool((MenuOptionCount + 1) * sizeof(UIT_LB_CELLDATA));     // NOTE: the list relies on a zero-initialized list terminator (hence +1).

    ASSERT (NULL != MenuOptions);
    if (NULL == MenuOptions)
    {
        return NULL;
    }

    for (Count=0 ; Count < MenuOptionCount ; Count++)
    {
        Index = ((FALSE == mShowFullMenu) ? mFormMap[Count].LimitedMenuIndex : mFormMap[Count].FullMenuIndex);

        if (UNUSED_INDEX != Index && Index < MenuOptionCount)
        {
            MenuOptions[Index].CellText = HiiGetString (mFrontPagePrivate.HiiHandle, mFormMap[Count].MenuString, NULL);
        }
    }

    // Create the ListBox that encapsulates the top-level menu.
    //
    FontInfo.FontSize    = FP_MFRAME_MENU_TEXT_FONT_HEIGHT;
    FontInfo.FontStyle   = EFI_HII_FONT_STYLE_NORMAL;

    ListBox *TopMenu = new_ListBox(OrigX,
                                   OrigY,
                                   CellWidth,
                                   CellHeight,
                                   0,
                                   &FontInfo,
                                   CellTextXOffset,
                                   &gMsColorTable.MasterFrameCellNormalColor,
                                   &gMsColorTable.MasterFrameCellHoverColor,
                                   &gMsColorTable.MasterFrameCellSelectColor,
                                   &gMsColorTable.MasterFrameCellGrayoutColor,
                                   MenuOptions,
                                   NULL
                                  );
    // Free HII string buffer.
    //
    if (NULL != MenuOptions)
    {
        FreePool(MenuOptions);
    }


    return TopMenu;
}


/**
  Draws the Front Page Title Bar.


  @param None.

  @retval  EFI_SUCCESS        Success.

**/
EFI_STATUS
RenderTitlebar(VOID)
{
    EFI_STATUS                Status = EFI_SUCCESS;
    EFI_FONT_DISPLAY_INFO     StringInfo;
    EFI_IMAGE_OUTPUT          *pBltBuffer;
    EFI_LOADED_IMAGE_PROTOCOL *ImageInfo;
    CHAR8                     Parameter;
    EFI_GUID                  *IconFile = NULL;
    UINTN                     DataSize;
    CHAR8                     *RebootReason;

    // Draw the titlebar background.
    //
    mGop->Blt(mGop,
              &gMsColorTable.TitleBarBackgroundColor,
              EfiBltVideoFill,
              0,
              0,
              0,
              0,
              mTitleBarWidth,
              mTitleBarHeight,
              mTitleBarWidth * sizeof (EFI_GRAPHICS_OUTPUT_BLT_PIXEL)
             );

    GetAndDisplayBitmap(PcdGetPtr(PcdFrontPageLogoFile), (mMasterFrameWidth  * FP_TBAR_MSLOGO_X_PERCENT) / 100, FALSE);   // 2nd param is x coordinate

    Status = gBS->HandleProtocol(mImageHandle, &gEfiLoadedImageProtocolGuid, (VOID **)&ImageInfo);
    ASSERT_EFI_ERROR(Status);

    if ((ImageInfo->LoadOptionsSize == 0) ||
        (ImageInfo->LoadOptions == NULL)) {
        DataSize = 0;
        Status = GetRebootReason(NULL, &DataSize);
        if (Status != EFI_BUFFER_TOO_SMALL) {
            DEBUG((DEBUG_ERROR,__FUNCTION__ " error getting RebootReason size. Code = %r\n",Status));
        }
        else {
            RebootReason = AllocatePool(DataSize);
            Status = GetRebootReason(RebootReason, &DataSize);
            if (EFI_ERROR(Status)) {
                DEBUG((DEBUG_ERROR,__FUNCTION__ " error reading RebootReason. Code = %r\n",Status));
                Parameter = 'B';
            } else {
                Parameter = RebootReason[0];
                Status = ClearRebootReason();
                if (EFI_ERROR(Status)) {
                    DEBUG((DEBUG_ERROR,__FUNCTION__ " error clearing RebootReason. Code = %r\n",Status));
                }
            }
            FreePool(RebootReason);
        }
    } else {
        Parameter = *((CHAR8 *) ImageInfo->LoadOptions);
    }
    DEBUG((DEBUG_ERROR, __FUNCTION__ " Parameter = %c - LoadOption=%p\n",Parameter,ImageInfo->LoadOptions));

    switch (Parameter) {
    case 'V' :  // VOL+
        IconFile = PcdGetPtr(PcdVolumeUpIndicatorFile);
        break;
    case 'B' : // BOOTFAIL
        IconFile = PcdGetPtr(PcdBootFailIndicatorFile);
        break;
    case 'O' : // OSIndication
        IconFile = PcdGetPtr(PcdFirmwareSettingsIndicatorFile);
        break;
    default:
        IconFile = NULL;
    }

    if (NULL != IconFile) {
        GetAndDisplayBitmap(IconFile, (mTitleBarWidth * FP_TBAR_ENTRY_INDICATOR_X_PERCENT) / 100, TRUE);
    }

    // Prepare string blitting buffer.
    //
    pBltBuffer = (EFI_IMAGE_OUTPUT *) AllocateZeroPool (sizeof (EFI_IMAGE_OUTPUT));

    ASSERT (pBltBuffer != NULL);
    if (NULL == pBltBuffer)
    {
        Status = EFI_OUT_OF_RESOURCES;
        goto Exit;
    }

    pBltBuffer->Width        = (UINT16)mBootHorizontalResolution;
    pBltBuffer->Height       = (UINT16)mBootVerticalResolution;
    pBltBuffer->Image.Screen = mGop;

    // Select a font (size & style) and font colors.
    //
    StringInfo.FontInfoMask         = EFI_FONT_INFO_ANY_FONT;
    StringInfo.FontInfo.FontSize    = FP_TBAR_TEXT_FONT_HEIGHT;
    StringInfo.FontInfo.FontStyle   = EFI_HII_FONT_STYLE_NORMAL;

    CopyMem (&StringInfo.ForegroundColor, &gMsColorTable.TitleBarTextColor,       sizeof (EFI_GRAPHICS_OUTPUT_BLT_PIXEL));
    CopyMem (&StringInfo.BackgroundColor, &gMsColorTable.TitleBarBackgroundColor, sizeof (EFI_GRAPHICS_OUTPUT_BLT_PIXEL));

    // Determine the size the TitleBar text string will occupy on the screen.
    //
    UINT32      MaxDescent;
    SWM_RECT    StringRect;

    GetTextStringBitmapSize (HiiGetString (mFrontPagePrivate.HiiHandle, STRING_TOKEN (STR_FRONT_PAGE_TITLE), NULL),
                             &StringInfo.FontInfo,
                             FALSE,
                             EFI_HII_OUT_FLAG_CLIP |
                             EFI_HII_OUT_FLAG_CLIP_CLEAN_X | EFI_HII_OUT_FLAG_CLIP_CLEAN_Y |
                             EFI_HII_IGNORE_LINE_BREAK,
                             &StringRect,
                             &MaxDescent
                            );

    // Render the string to the screen, vertically centered.
    //
    mSWMProtocol->StringToWindow (mSWMProtocol,
                                  mImageHandle,
                                  EFI_HII_OUT_FLAG_CLIP |
                                  EFI_HII_OUT_FLAG_CLIP_CLEAN_X | EFI_HII_OUT_FLAG_CLIP_CLEAN_Y |
                                  EFI_HII_IGNORE_LINE_BREAK | EFI_HII_DIRECT_TO_SCREEN,
                                  HiiGetString (mFrontPagePrivate.HiiHandle, STRING_TOKEN (STR_FRONT_PAGE_TITLE), NULL),
                                  &StringInfo,
                                  &pBltBuffer,
                                  ((mMasterFrameWidth  * FP_TBAR_TEXT_X_PERCENT) / 100),                     // Based on Master Frame width - so the logo bitmap aligns with the text in the menu.
                                  ((mTitleBarHeight / 2) - ((StringRect.Bottom - StringRect.Top + 1) / 2)),  // Vertically center.
                                  NULL,
                                  NULL,
                                  NULL
                                 );

Exit:

    if (NULL != pBltBuffer)
    {
        FreePool(pBltBuffer);
    }

    return Status;
}


/**
  Draws the FrontPage Master Frame and its Top-Level menu.


  @param None.

  @retval  EFI_SUCCESS        Success.

**/
EFI_STATUS
RenderMasterFrame(VOID)
{
    EFI_STATUS  Status      = EFI_SUCCESS;
    VOID        *pContext   = NULL;


    // Verify that the top-level menu was created.
    //
    ASSERT (NULL != mTopMenu);
    if (NULL == mTopMenu)
    {
        Status = EFI_INVALID_PARAMETER;
        goto Exit;
    }

    // Draw the master frame background.
    //
    mGop->Blt(mGop,
              &gMsColorTable.MasterFrameBackgroundColor,
              EfiBltVideoFill,
              0,
              0,
              0,
              mTitleBarHeight,
              mMasterFrameWidth,
              mMasterFrameHeight,
              0
             );


    // Draw divider line.
    //
    mGop->Blt(mGop,
              &gMsColorTable.TitleBarBackgroundColor,
              EfiBltVideoFill,
              0,
              0,
              (mMasterFrameWidth - FP_MFRAME_DIVIDER_LINE_WIDTH_PIXELS),
              mTitleBarHeight,
              FP_MFRAME_DIVIDER_LINE_WIDTH_PIXELS,
              mMasterFrameHeight,
              0
             );


    // Draw the top-level menu.
    //
    mTopMenu->Base.Draw(mTopMenu,
                        FALSE,
                        NULL,
                        &pContext
                       );

Exit:

    return Status;
}

/**
  Master Frame callback (signalled by Display Engine) for receiving user input data (i.e., key, touch, mouse, etc.).


  @param    None.

  @retval   None.

**/
VOID
EFIAPI
MasterFrameNotifyCallback (IN  EFI_EVENT    Event,
                           IN  VOID         *Context)
{
    UINT32          SelectedIndex       = 0;
    VOID            *pSelectionContext  = NULL;
    OBJECT_STATE    MenuState           = NORMAL;
    SWM_INPUT_STATE *pInputState        = &mDisplayEngineState.InputState;
    LB_RETURN_DATA  ReturnData;


    // If we just need to redraw, do that and exit.
    //
    if (REDRAW == mDisplayEngineState.NotificationType)
    {
        mTopMenu->Base.Draw (mTopMenu,
                             mDisplayEngineState.ShowTopMenuHighlight,
                             &mDisplayEngineState.InputState,
                             &pSelectionContext
                            );

        goto Exit;
    }

    // We'll only handle user input from this point onwards.
    //
    if (USERINPUT != mDisplayEngineState.NotificationType)
    {
        goto Exit;
    }

    // If we are receiving touch/mouse data from the display engine and it's a finger/button down event, process it.
    //
    if ((SWM_INPUT_TYPE_TOUCH == pInputState->InputType/* && (pInputState->State.TouchState.ActiveButtons & 0x1) */) ||
        SWM_INPUT_TYPE_KEY   == pInputState->InputType)
    {
        // Draw the top-level menu in the master frame.
        //
        MenuState = mTopMenu->Base.Draw (mTopMenu,
                                         mDisplayEngineState.ShowTopMenuHighlight,
                                         &mDisplayEngineState.InputState,
                                         &pSelectionContext
                                        );

        // If nothing was selected (user may simply have moved the highlighted cell), there's no action to take.
        //
        if (SELECT != MenuState)
        {
            return;
        }

        // Get the currently selected top-level menu entry (may be none).
        //
        mTopMenu->GetSelectedCellIndex (mTopMenu,
                                        &ReturnData);

        SelectedIndex = ReturnData.SelectedCell;

        if (SelectedIndex != mCurrentFormIndex)
        {
            // Update the current form ID to the new one.
            //
            mCurrentFormIndex = SelectedIndex;

            // Signal the form (browser) to close so the new form will be displayed.
            //
            mDisplayEngineState.CloseFormRequest = TRUE;
            mTerminateFrontPage = FALSE;
        }
    }

Exit:
    mDisplayEngineState.NotificationType = NONE;

    return;
}


STATIC
EFI_STATUS
InitializeFrontPageUI (VOID)
{
    EFI_STATUS  Status = EFI_SUCCESS;


    // Establish initial FrontPage TitleBar and Master Frame dimensions based on the current screen size.
    //
    mTitleBarWidth              = mBootHorizontalResolution;
    mTitleBarHeight             = ((mBootVerticalResolution   * FP_TBAR_HEIGHT_PERCENT)  / 100);
    mMasterFrameWidth           = ((mBootHorizontalResolution * FP_MFRAME_WIDTH_PERCENT) / 100);
    mMasterFrameHeight          = (mBootVerticalResolution - mTitleBarHeight);

    DEBUG((DEBUG_INFO, "INFO [FP]: FP Dimensions: %d, %d, %d, %d, %d, %d\r\n", \
           mBootHorizontalResolution, mBootVerticalResolution, mTitleBarWidth, mTitleBarHeight, mMasterFrameWidth, mMasterFrameHeight));

    // Compute Master Frame origin and menu text indentation.
    //
    UINT32 MasterFrameMenuOrigX = 0;
    UINT32 MasterFrameMenuOrigY = mTitleBarHeight;
    UINT32 CellTextXOffset      = ((mMasterFrameWidth * FP_MFRAME_MENU_TEXT_OFFSET_PERCENT) / 100);

    // Create the top-level menu in the Master Frame.
    //
    mTopMenu = CreateTopMenu(MasterFrameMenuOrigX,
                             MasterFrameMenuOrigY,
                             (mMasterFrameWidth - FP_MFRAME_DIVIDER_LINE_WIDTH_PIXELS),
                             ((mMasterFrameHeight * FP_MFRAME_MENU_CELL_HEIGHT_PERCENT) / 100),
                             CellTextXOffset);

    ASSERT (NULL != mTopMenu);
    if (NULL == mTopMenu)
    {
        Status = EFI_OUT_OF_RESOURCES;
        goto Exit;
    }

    // Render the TitleBar at the top of the screen.
    //
    RenderTitlebar();

    // Render the Master Frame and its Top-Level menu contents.
    //
    RenderMasterFrame();

    // Create the Master Frame notification event.  This event is signalled by the display engine to note that
    // there is a user input event outside the form area to consider.
    //
    Status = gBS->CreateEventEx (EVT_NOTIFY_SIGNAL,
                                 TPL_CALLBACK,
                                 MasterFrameNotifyCallback,
                                 NULL,
                                 &gMsEventMasterFrameNotifyGroupGuid,
                                 &mMasterFrameNotifyEvent
                                );

    if (EFI_SUCCESS != Status)
    {
        DEBUG((DEBUG_ERROR, "ERROR [FP]: Failed to create master frame notification event.  Status = %r\r\n", Status));
        goto Exit;
    }

    // Set shared pointer to user input context structure in a PCD so it can be shared.
    //
    PcdSet64S(PcdCurrentPointerState, (UINT64) (UINTN)&mDisplayEngineState);

Exit:

    return Status;
}

VOID
ProcessBootNext ( VOID )
{
    EFI_STATUS                      Status;
    UINT16                         *BootNext;
    UINTN                           DataSize;
    CHAR16                          BootNextVariableName[sizeof ("Boot####")];
    EFI_BOOT_MANAGER_LOAD_OPTION    LoadOption;
    UINT64                          OsIndication;

    DEBUG((DEBUG_INFO, __FUNCTION__ " entry\n"));
    //
    // Cache and remove the "BootNext" NV variable.
    //
    GetEfiGlobalVariable2 (EFI_BOOT_NEXT_VARIABLE_NAME, (VOID **) &BootNext, &DataSize);
    if (DataSize != sizeof (UINT16)) {
        if (BootNext != NULL) {
            FreePool (BootNext);
        }
        BootNext = NULL;
    }

    Status = gRT->SetVariable (
                    EFI_BOOT_NEXT_VARIABLE_NAME,
                    &gEfiGlobalVariableGuid,
                    0,
                    0,
                    NULL
                    );
    //
    // Deleting NV variable shouldn't fail unless it doesn't exist.
    //
    ASSERT (Status == EFI_SUCCESS || Status == EFI_NOT_FOUND);

    if (NULL != BootNext) {
        UnicodeSPrint (BootNextVariableName, sizeof (BootNextVariableName), L"Boot%04x", *BootNext);
        DEBUG((DEBUG_INFO, "Acting on BootNext %4.4x\n",*BootNext));
        FreePool (BootNext);
        Status = EfiBootManagerVariableToLoadOption (BootNextVariableName, &LoadOption);
        if (!EFI_ERROR (Status)) {
            EfiBootManagerBoot (&LoadOption);
            EfiBootManagerFreeLoadOption (&LoadOption);
        }

        // Reboot to front page

        OsIndication = EFI_OS_INDICATIONS_BOOT_TO_FW_UI;
        Status = gRT->SetVariable (
            L"OsIndications",
            &gEfiGlobalVariableGuid,
            EFI_VARIABLE_BOOTSERVICE_ACCESS | EFI_VARIABLE_RUNTIME_ACCESS | EFI_VARIABLE_NON_VOLATILE,
            sizeof(UINT64),
            &OsIndication
            );
        if (EFI_ERROR(Status)) {
            DEBUG((DEBUG_ERROR,"Unable to set OsIndications\n"));
        }
        DEBUG((DEBUG_INFO, __FUNCTION__ " Resetting system\n"));
        gRT->ResetSystem(EfiResetWarm, EFI_SUCCESS, 0, NULL);
    }
}

/**
  This function is the main entry of the platform setup entry.
  The function will present the main menu of the system setup,
  this is the platform reference part and can be customize.
**/
EFI_STATUS
EFIAPI
UefiMain(IN EFI_HANDLE        ImageHandle,
         IN EFI_SYSTEM_TABLE  *SystemTable
        )
{
    EFI_STATUS  Status  = EFI_SUCCESS;
    UINT32      OSKMode = 0;


    //Delete BootNext if entry to BootManager.
    Status = gRT->SetVariable(
        L"BootNext",
        &gEfiGlobalVariableGuid,
        0,
        0,
        NULL
        );

    // Save image handle for later.
    //
    mImageHandle = ImageHandle;

    // Disable the watchdog timer
    //
    gBS->SetWatchdogTimer (0, 0, 0, (CHAR16 *)NULL);

    mResetRequired = FALSE;

    Status = gBS->LocateProtocol(&gDfciSettingAccessProtocolGuid,
        NULL,
        (VOID **)&mSettingAccess
        );
    if (EFI_ERROR(Status))
    {
        ASSERT_EFI_ERROR(Status);
        DEBUG((DEBUG_ERROR, __FUNCTION__"Couldn't locate system setting access protocol\n"));
    }

    // Force-connect all controllers.
    //
    EfiBootManagerConnectAll();

    // Set console mode: *not* VGA, no splashscreen logo.
    // Insure Gop is in Big Display mode prior to accessing GOP.
    SetGraphicsConsoleMode(GCM_NATIVE_RES);

    //
    // After the console is ready, get current video resolution
    // and text mode before launching setup at first time.
    //
    Status = gBS->LocateProtocol (&gEfiGraphicsOutputProtocolGuid,
                                  NULL,
                                  (VOID**)&mGop
                                 );

    if (EFI_ERROR (Status))
    {
        mGop = (EFI_GRAPHICS_OUTPUT_PROTOCOL *)NULL;
        goto Exit;
    }

    // Determine if the Font Protocol is available
    //
    Status = gBS->LocateProtocol (&gEfiHiiFontProtocolGuid,
                                  NULL,
                                  (VOID **)&mFont
                                 );

    ASSERT_EFI_ERROR(Status);
    if (EFI_ERROR(Status))
    {
        mFont = (EFI_HII_FONT_PROTOCOL *)NULL;
        Status = EFI_UNSUPPORTED;
        DEBUG((DEBUG_ERROR, "ERROR [FP]: Failed to find Font protocol (%r).\r\n", Status));
        goto Exit;
    }

    // Locate the Simple Window Manager protocol.
    //
    Status = gBS->LocateProtocol (&gMsSWMProtocolGuid,
                                  NULL,
                                  (VOID **)&mSWMProtocol
                                 );

    if (EFI_ERROR(Status))
    {
        mSWMProtocol = NULL;
        Status = EFI_UNSUPPORTED;
        DEBUG((DEBUG_ERROR, "ERROR [FP]: Failed to find the window manager protocol (%r).\r\n", Status));
        goto Exit;
    }

    // Locate the on-screen keyboard (OSK) protocol.
    //
    Status = gBS->LocateProtocol (&gMsOSKProtocolGuid,
                                  NULL,
                                  (VOID **)&mOSKProtocol
                                 );

    if (EFI_ERROR(Status))
    {
        Status = EFI_UNSUPPORTED;
        mOSKProtocol = (MS_ONSCREEN_KEYBOARD_PROTOCOL *)NULL;
        DEBUG((DEBUG_ERROR, "ERROR [FP]: Failed to find the on-screen keyboard protocol (%r).\r\n", Status));
        goto Exit;
    }

    // Set default on-screen keyboard size and position.  Disable icon auto-activation (set by BDS) since
    // we'll display the OSK ourselves when appropriate.
    //

    // Disable OSK icon auto-activation and self-refresh, and ensure keyboard is disabled.
    //
    mOSKProtocol->GetKeyboardMode(mOSKProtocol, &OSKMode);
    OSKMode &= ~(OSK_MODE_AUTOENABLEICON | OSK_MODE_SELF_REFRESH);
    mOSKProtocol->ShowKeyboard(mOSKProtocol,FALSE);
    mOSKProtocol->ShowKeyboardIcon(mOSKProtocol,FALSE);
    mOSKProtocol->SetKeyboardMode(mOSKProtocol, OSKMode);

    // Set keyboard size and position (75% of screen width, bottom-right corner, docked).
    //
    mOSKProtocol->SetKeyboardSize(mOSKProtocol, FP_OSK_WIDTH_PERCENT);
    mOSKProtocol->SetKeyboardPosition(mOSKProtocol, BottomRight, Docked);

    if (mGop != NULL)
    {
        //
        // Get current video resolution and text mode.
        //
        mBootHorizontalResolution = mGop->Mode->Info->HorizontalResolution;
        mBootVerticalResolution   = mGop->Mode->Info->VerticalResolution;
    }

    // Ensure screen is clear when switch Console from Graphics mode to Text mode
    //
    gST->ConOut->EnableCursor (gST->ConOut, FALSE);
    gST->ConOut->ClearScreen (gST->ConOut);

    // Initialize the Simple UI ToolKit.
    //
    Status = InitializeUIToolKit(ImageHandle);

    if (EFI_ERROR(Status))
    {
        DEBUG((DEBUG_ERROR, "ERROR [FP]: Failed to initialize the UI toolkit (%r).\r\n", Status));
        goto Exit;
    }

    // Initialize HII data (ex: register strings, etc.).
    //
    InitializeFrontPage(TRUE);

    // Initialize the FrontPage User Interface.
    //
    Status = InitializeFrontPageUI();

    if (EFI_SUCCESS != Status)
    {
        DEBUG((DEBUG_ERROR, "ERROR [FP]: Failed to initialize the FrontPage user interface.  Status = %r\r\n", Status));
        goto Exit;
    }


    // Set the default form ID to show on the canvas.
    //
    mCurrentFormIndex   = 0;
    Status              = EFI_SUCCESS;

    // Display the specified FrontPage form.
    //
    do
    {
        // By default, we'll terminate FrontPage after processing the next Form unless the flag is reset.
        //
        mTerminateFrontPage = TRUE;

        CallFrontPage (mCurrentFormIndex);

    } while (FALSE == mTerminateFrontPage);

    if (mResetRequired)
    {
        ResetSystemWithSubtype( EfiResetCold, &gFrontPageResetGuid );
    }

    ProcessBootNext ();

    // Clean-up
    //
    UninitializeFrontPage();

Exit:

    return Status;
}


EFI_STATUS  GetAndDisplayBitmap (EFI_GUID *FileGuid, UINTN XCoord, BOOLEAN XCoordAdj) {
    EFI_STATUS                       Status;
    UINT8                           *BMPData          = NULL;
    UINTN                            BMPDataSize      = 0;
    EFI_GRAPHICS_OUTPUT_BLT_PIXEL   *BltBuffer        = NULL;
    UINTN                            BltBufferSize;
    UINTN                            BitmapHeight;
    UINTN                            BitmapWidth;

    // Get the specified image from FV.
    //
    Status = GetSectionFromAnyFv(FileGuid,
                                 EFI_SECTION_RAW,
                                 0,
                                 (VOID **)&BMPData,
                                 &BMPDataSize
                                );

    if (EFI_ERROR(Status)) {
        DEBUG((DEBUG_ERROR, "ERROR [DE]: Failed to find bitmap file (GUID=%g) (%r).\r\n", FileGuid, Status));
        return Status;
    }

    // Convert the bitmap from BMP format to a GOP framebuffer-compatible form.
    //
    Status = TranslateBmpToGopBlt(BMPData,
                                BMPDataSize,
                                &BltBuffer,
                                &BltBufferSize,
                                &BitmapHeight,
                                &BitmapWidth
                               );
    if (EFI_ERROR(Status)) {
        FreePool(BMPData);
        DEBUG((DEBUG_ERROR, "ERROR [DE]: Failed to convert bitmap file to GOP format (%r).\r\n", Status));
        return Status;
    }

    if (XCoordAdj == TRUE)
    {
       XCoord -= BitmapWidth;
    }

    mGop->Blt(mGop,
              BltBuffer,
              EfiBltBufferToVideo,
              0,
              0,
              XCoord,   //Upper Right corner
              ((mTitleBarHeight / 2) - (BitmapHeight / 2)),
              BitmapWidth,
              BitmapHeight,
              0
             );

    FreePool(BMPData);
    FreePool(BltBuffer );
    return Status;
}

/**
Acquire a Auth Token and save it in a protocol
**/
EFI_STATUS GetAuthToken(CHAR16 *PasswordBuffer){

    EFI_STATUS                            Status;

    Status = gBS->LocateProtocol(
        &gDfciAuthenticationProtocolGuid,
        NULL,
        (VOID **)&mAuthProtocol
        );

    if (EFI_ERROR(Status))
    {
        DEBUG((DEBUG_ERROR, "%a - Failed to locate MsAuthProtocol. Can't use check auth. %r\n", __FUNCTION__, Status));
        mAuthProtocol = NULL;
        return Status;
    }
    if (PasswordBuffer != NULL){
        Status = mAuthProtocol->AuthWithPW(mAuthProtocol, PasswordBuffer, StrLen(PasswordBuffer), &mAuthToken);
        DEBUG((DEBUG_INFO, __FUNCTION__"Auth Token Acquired %x\n", mAuthToken, Status));
    }
    else{
        Status = mAuthProtocol->AuthWithPW(mAuthProtocol, NULL, 0, &mAuthToken);
        DEBUG((DEBUG_INFO, __FUNCTION__"Auth Token Acquired with NULL Password %x\n", mAuthToken, Status));
    }
    mFrontPageAuthTokenProtocol = (FRONT_PAGE_AUTH_TOKEN_PROTOCOL *) AllocateZeroPool(sizeof(mFrontPageAuthTokenProtocol));
//regardless of the auth token value we install the protocol.
//when system password is set, if user enters a invalid password, then the frontpage access will be restricted.
//when there isno system password set, if auth token with a null request is returned invalid we
//still allow only a restricted access of the menu. the protocol with invalid auth token will not be used.
    mFrontPageAuthTokenProtocol->AuthToken = (UINTN)mAuthToken;
    Status = gBS->InstallMultipleProtocolInterfaces(&mImageHandle,
        &gMsFrontPageAuthTokenProtocolGuid,
        mFrontPageAuthTokenProtocol,
        NULL);

    if (Status == EFI_SUCCESS){
        DEBUG((DEBUG_INFO, __FUNCTION__" FrontPageAuthTokenProtocol was successfully installed %r\n", Status));
    }

    return Status;
}
