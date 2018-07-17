// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. 
// To get started please visit https://microsoft.github.io/azure-iot-developer-kit/docs/projects/connect-iot-hub?utm_source=ArduinoExtension&utm_medium=ReleaseNote&utm_campaign=VSCode
#include "AZ3166WiFi.h"
#include "AzureIotHub.h"
#include "DevKitMQTTClient.h"
#include "DevKitOTAUtils.h"
#include "OTAUpdateClient.h"
#include "SystemTime.h"

static bool hasWifi = false;
// This string is hard-coded for firmware comparision in the example. In practice, user can call getDevkitVersion() to get the version of current firmware.
const char* currentFirmwareVersion = "1.1.1"

// Get the current time in YYYY-MM-DD-HH:MM:SS format
char *getTimeStamp()
{
  time_t t = time(NULL);
  size_t len = sizeof("2011-10-08-07:07:09");
  char* buf = (char*)malloc(len + 1);
  strftime(buf, len, "%Y-%m-%d-%R:%S", gmtime(&t));
  buf[len] = '\0';
  LogInfo("Time is %s", buf);
  return(buf);
}

bool enabledOTA = true;
const FW_INFO* fwInfo = NULL;
MAP_HANDLE OTAStatusMap = NULL;

//////////////////////////////////////////////////////////////////////////////////////////////////////////
// Utilities
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

  Screen.print(3, " > IoT Hub");
  DevKitMQTTClient_SetOption(OPTION_MINI_SOLUTION_NAME, "FirmwareOTA");
  DevKitMQTTClient_Init(true);
  OTAStatusMap = Map_Create(NULL);
  Map_Add(OTAStatusMap, OTA_CURRENT_FW_VERSION, currentFirmwareVersion);
  Map_Add(OTAStatusMap, OTA_FW_UPDATE_STATUS, OTA_STATUS_CURRENT);          
  IoTHubClient_ReportOTAStatus(OTAStatusMap);
  Map_Destroy(OTAStatusMap);
  fwInfo = new FW_INFO;
}

// Enter a failed state, print failed message and report status
void enterFailed(const char* failedMsg)
{
  Screen.clean();
  Screen.print(failedMsg);
  OTAStatusMap = Map_Create(NULL);
  Map_Add(OTAStatusMap, OTA_FW_UPDATE_STATUS, OTA_STATUS_ERROR);
  Map_Add(OTAStatusMap, OTA_FW_UPDATE_SUBSTATUS, failedMsg);
  IoTHubClient_ReportOTAStatus(OTAStatusMap);
  Map_Destroy(OTAStatusMap);
  enabledOTA = false;
}

void loop()
{
  if (hasWifi && enabledOTA)
  {
    DevKitMQTTClient_Check();

    // Check if there is new firmware info.
    fwInfo = IoTHubClient_GetLatestFwInfo();
    if (fwInfo)
    {

      // Check if the URL is https as we require it for safety purpose.
      if (strlen(fwInfo->fwPackageURI) < 6 || (strncmp("https:", fwInfo->fwPackageURI, 6) != 0))
      {
        OTAStatusMap = Map_Create(NULL);
        Map_Add(OTAStatusMap, OTA_FW_UPDATE_STATUS, OTA_STATUS_ERROR);
        Map_Add(OTAStatusMap, OTA_FW_UPDATE_SUBSTATUS, "URINotHTTPS");
        IoTHubClient_ReportOTAStatus(OTAStatusMap);
        Map_Destroy(OTAStatusMap);
      }
      else
      {

        // Check if this is a new version.
        if (IoTHubClient_FwVersionCompare(fwInfo->fwVersion, currentFirmwareVersion) == 1)
        {
          Screen.clean();
          Screen.print(0, "New firmware.");
          Screen.print(1, fwInfo->fwVersion);

          // Report downloading status.
          char *timeStamp = getTimeStamp();
          OTAStatusMap = Map_Create(NULL);
          Map_Add(OTAStatusMap, OTA_FW_UPDATE_STATUS, OTA_STATUS_DOWNLOADING);
          Map_Add(OTAStatusMap, OTA_PENDING_FW_VERSION, fwInfo->fwVersion);
          Map_Add(OTAStatusMap, OTA_LAST_FW_UPDATE_STARTTIME, timeStamp);
          IoTHubClient_ReportOTAStatus(OTAStatusMap);
          Map_Destroy(OTAStatusMap);
          free(timeStamp);

          // Close IoT Hub Client to release the TLS resource for firmware download.
          DevKitMQTTClient_Close();
          Screen.clean();
          Screen.print("Downloading...");

          // Download the firmware. This can be customized according to the board type.
          OTAUpdateClient& otaClient = OTAUpdateClient::getInstance();
          int result = otaClient.updateFromUrl(fwInfo->fwPackageURI);
          
          if (result == 0)
          {
            Screen.print(0, "Finished downloading");

            // Reopen the IoT Hub Client for reporting status.
            DevKitMQTTClient_Init(true);

            // Check the firmware if there is checksum.
            if (fwInfo -> fwPackageCheckValue != NULL)
            {
              Screen.print(1, "Verifying...");

              OTAStatusMap = Map_Create(NULL);
              Map_Add(OTAStatusMap, OTA_FW_UPDATE_STATUS, OTA_STATUS_VERIFYING);
              IoTHubClient_ReportOTAStatus(OTAStatusMap);
              Map_Destroy(OTAStatusMap);

              if (otaClient.checkFirmwareCRC16(strtoul(fwInfo->fwPackageCheckValue, NULL, 16), fwInfo->fwSize) == 0)
              {
                Screen.clean();
                Screen.print("Verify success\n");
                LogInfo("Verify success");
              }
              else 
              {
                enterFailed("VerifyFailed");
              }
            }
            
            char *timeStamp = getTimeStamp();
            OTAStatusMap = Map_Create(NULL);
            Map_Add(OTAStatusMap, OTA_FW_UPDATE_STATUS, OTA_STATUS_APPLYING);
            Map_Add(OTAStatusMap, OTA_LAST_FW_UPDATE_ENDTIME, timeStamp);
            IoTHubClient_ReportOTAStatus(OTAStatusMap);
            Map_Destroy(OTAStatusMap);
            free(timeStamp);

            int countDown = 5;
            char countDownStr[5];
            Screen.print(1, "System reboot in");
            do
            {
              int n = sprintf(countDownStr, "%d", countDown);
              Screen.print(2, countDownStr);
              delay(1000);
            } while (countDown--); 

            // Reboot system to apply the firmware.
            mico_system_reboot();
          }
          else
          {
            enterFailed("DownloadFailed");
          }
        }
      }
    }
  }
  delay(10);
}
