// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. 
// To get started please visit https://microsoft.github.io/azure-iot-developer-kit/docs/projects/connect-iot-hub?utm_source=ArduinoExtension&utm_medium=ReleaseNote&utm_campaign=VSCode
#include "AZ3166WiFi.h"
#include "AzureIotHub.h"
#include "DevKitMQTTClient.h"
#include "DevKitOTAUtils.h"
#include "OTAUpdateClient.h"
#include "SystemTime.h"

static char* currentFirmwareVersion = "1.0.0";

static bool hasWifi = false;

static bool enableOTA = true;
static const FW_INFO* fwInfo = NULL;
static MAP_HANDLE OTAStatusMap = NULL;

//////////////////////////////////////////////////////////////////////////////////////////////////////////
// Utilities
// Initialize Wi-Fi
static void InitWifi()
{
  Screen.print(2, "Connecting...");
  
  if (WiFi.begin() == WL_CONNECTED)
  {
    IPAddress ip = WiFi.localIP();
    Screen.print(1, ip.get_address());
    hasWifi = true;
    Screen.print(2, "Running... \r\n");
  }
  else
  {
    hasWifi = false;
    Screen.print(1, "No Wi-Fi\r\n ");
  }
}

// Report the OTA update status to Azure
static void ReportOTAStatus(char* currentFwVersion, char* pendingFwVersion, char* fwUpdateStatus, char* fwUpdateSubstatus, char* lastFwUpdateStartTime, char* lastFwUpdateEndTime)
{
  OTAStatusMap = Map_Create(NULL);
  if (currentFwVersion)
  {
    Map_Add(OTAStatusMap, OTA_CURRENT_FW_VERSION, currentFwVersion);
  }
  if (pendingFwVersion)
  {
    Map_Add(OTAStatusMap, OTA_PENDING_FW_VERSION, pendingFwVersion);
  }
  if (fwUpdateStatus)
  {
    Map_Add(OTAStatusMap, OTA_FW_UPDATE_STATUS, fwUpdateStatus);
  }
  if (fwUpdateSubstatus)
  {
    Map_Add(OTAStatusMap, OTA_FW_UPDATE_SUBSTATUS, fwUpdateSubstatus);
  }
  if (lastFwUpdateStartTime)
  {
    Map_Add(OTAStatusMap, OTA_LAST_FW_UPDATE_STARTTIME, lastFwUpdateStartTime);
  }
  if (lastFwUpdateEndTime)
  {
    Map_Add(OTAStatusMap, OTA_LAST_FW_UPDATE_ENDTIME, lastFwUpdateEndTime);
  }
  IoTHubClient_ReportOTAStatus(OTAStatusMap);
  Map_Destroy(OTAStatusMap);
  DevKitMQTTClient_Check();
}
// Enter a failed state, print failed message and report status
static void OTAUpdateFailed(const char* failedMsg)
{
  ReportOTAStatus(currentFirmwareVersion, fwInfo ? fwInfo->fwVersion : NULL, OTA_STATUS_ERROR, (char*)failedMsg, NULL, NULL);
  enableOTA = false;
  Screen.print(1, "OTA error:");
  Screen.print(2, failedMsg);
  Screen.print(3, "Stop checking.");
  LogInfo("Failed to update firmware %s: %s, disable OTA update.", fwInfo ? fwInfo->fwVersion : "<unknown>", failedMsg);
}

// Check for new firmware
static void CheckNewFirmware(void)
{
  if (!enableOTA)
  {
    return;
  }

  char timeStr[30];
  time_t t = time(NULL);
  strftime(timeStr, 30, "%Y-%m-%d-%R:%S", gmtime(&t));

  // Check if there is new firmware info.
  fwInfo = IoTHubClient_GetLatestFwInfo();
  if (fwInfo == NULL)
  {
    // No firmware update info
    return;
  }
  
  if (fwInfo->fwVersion == NULL || fwInfo->fwPackageURI == NULL)
  {
    // Disable 
    LogInfo("Invalid new firmware infomation retrieved, disable OTA update.");
    enableOTA = false;
    return;
  }

  // Check if the URL is https as we require it for safety purpose.
  if (strlen(fwInfo->fwPackageURI) < 6 || (strncmp("https:", fwInfo->fwPackageURI, 6) != 0))
  {
    // Report error status, URINotHTTPS
    OTAUpdateFailed("URINotHTTPS");
    return;
  }

  // Check if this is a new version.
  if (IoTHubClient_FwVersionCompare(fwInfo->fwVersion, currentFirmwareVersion) <= 0)
  {
    // The firmware version from cloud <= the running firmware version
    return;
  }

  // New firemware
  Screen.print(1, "New firmware:");
  Screen.print(2, fwInfo->fwVersion);
  LogInfo("New firmware is available: %s.", fwInfo->fwVersion);
  
  // Close IoT Hub Client to release the TLS resource for firmware download.
  DevKitMQTTClient_Close();

  Screen.print(3, " downloading...");
  LogInfo(">> Downloading from %s...", fwInfo->fwPackageURI);
  // Report downloading status.
  ReportOTAStatus(currentFirmwareVersion, fwInfo->fwVersion, OTA_STATUS_DOWNLOADING, fwInfo->fwPackageURI, timeStr, NULL);
  
  // Download the firmware. This can be customized according to the board type.
  OTAUpdateClient& otaClient = OTAUpdateClient::getInstance();
  int result = otaClient.updateFromUrl(fwInfo->fwPackageURI);
  if (result == 0)
  {
    // Report error status, DownloadFailed
    OTAUpdateFailed("DownloadFailed");
    return;
  }
  if (result != fwInfo->fwSize)
  {
    // Report error status, FileSizeNotMatch
    OTAUpdateFailed("FileSizeNotMatch");
    return;
  }
  
  Screen.print(3, " done.");
  LogInfo(">> done.");
          
  // Reopen the IoT Hub Client for reporting status.
  DevKitMQTTClient_Init(true);

  // Check the firmware if there is checksum.
  if (fwInfo->fwPackageCheckValue != NULL)
  {
    Screen.print(3, " verifying...");

    // Report status
    ReportOTAStatus(currentFirmwareVersion, fwInfo->fwVersion, OTA_STATUS_VERIFYING, fwInfo->fwPackageCheckValue, NULL, NULL);
    
    if (otaClient.calculateFirmwareCRC16(fwInfo->fwSize) == strtoul(fwInfo->fwPackageCheckValue, NULL, 16))
    {
      Screen.print(3, " passed.");
      LogInfo(">> CRC check passed.");
    }
    else 
    {
      // Report error status, VerifyFailed
      OTAUpdateFailed("VerifyFailed");
      Screen.print(3, " CRC failed.");
      return;
    }
  }
  
  // Counting down and reboot
  Screen.clean();
  Screen.print(0, "Reboot system");
  LogInfo("Reboot system to apply the new firmware:");
  char msg[2] = { 0 };
  for (int i = 0; i < 5; i++)
  {
    msg[0] = '0' + 5 - i;
    Screen.print(2, msg);
    LogInfo(msg);
    delay(1000);
  }
  
  // Report status
  t = time(NULL);
  strftime(timeStr, 30, "%Y-%m-%d-%R:%S", gmtime(&t));
  ReportOTAStatus(currentFirmwareVersion, fwInfo->fwVersion, OTA_STATUS_APPLYING, NULL, NULL, timeStr);
  // Reboot system to apply the firmware.
  mico_system_reboot();
}

//////////////////////////////////////////////////////////////////////////////////////////////////////////
// Arduino sketch
void setup()
{
  Screen.init();
  Screen.print(0, "IoT DevKit");
  Screen.print(2, "Initializing...");
  
  Screen.print(3, " > Serial");
  Serial.begin(115200);

  // Initialize the WiFi module
  Screen.print(3, " > WiFi");
  hasWifi = false;
  InitWifi();
  if (!hasWifi)
  {
    return;
  }
  
  // Print out the version
  Screen.print(1, "FW version:");
  Screen.print(2, currentFirmwareVersion);
  Screen.print(3, " checking...");

  // Initialize MQTT client
  DevKitMQTTClient_SetOption(OPTION_MINI_SOLUTION_NAME, "FirmwareOTA");
  DevKitMQTTClient_Init(true);
  
  LogInfo("FirmwareOTA demo: %s", currentFirmwareVersion);
  // Report OTA status
  ReportOTAStatus(currentFirmwareVersion, NULL, OTA_STATUS_CURRENT, NULL, NULL, NULL);
}

void loop()
{
  if (hasWifi)
  {
    DevKitMQTTClient_Check();

    // Check for new firmware
    CheckNewFirmware();
  }
  delay(1000);
}
