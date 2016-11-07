/******************************************************************************
** Copyright (c) 2013-2016 Intel Corporation All Rights Reserved
**
** Licensed under the Apache License, Version 2.0 (the "License"); you may not
** use this file except in compliance with the License.
**
** You may obtain a copy of the License at
**     http://www.apache.org/licenses/LICENSE-2.0
**
** Unless required by applicable law or agreed to in writing, software
** distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
** WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
**
** See the License for the specific language governing permissions and
** limitations under the License.
**
******************************************************************************/

#include "WIPolicyOperatingSystemMobileNotification.h"
#include "PolicyManager.h"
#include "EsifServices.h"
#include "OsMobileNotificationType.h"

WIPolicyOperatingSystemMobileNotification::WIPolicyOperatingSystemMobileNotification(
    DptfManagerInterface* dptfManager, UIntN mobileNotification) :
    WorkItem(dptfManager, FrameworkEvent::PolicyOperatingSystemMobileNotification),
    m_mobileNotification(mobileNotification)
{
}

WIPolicyOperatingSystemMobileNotification::~WIPolicyOperatingSystemMobileNotification(void)
{
}

void WIPolicyOperatingSystemMobileNotification::execute(void)
{
    writeWorkItemStartingInfoMessage();

    PolicyManager* policyManager = getPolicyManager();
    UIntN policyListCount = policyManager->getPolicyListCount();

    for (UIntN i = 0; i < policyListCount; i++)
    {
        std::string functionName = "";
        try
        {
            Policy* policy = policyManager->getPolicyPtr(i);

            OsMobileNotificationType::Type notificationType =
                (OsMobileNotificationType::Type)(((UInt32)m_mobileNotification & 0xFFFF0000) >> 16);
            UInt32 notificationValue = (UInt32)m_mobileNotification & 0xFFFF;

            switch (notificationType)
            {
            case OsMobileNotificationType::EmergencyCallMode:
                getDptfManager()->getEventCache()->emergencyCallModeState.set(notificationValue);
                functionName = "Policy::executePolicyOperatingSystemEmergencyCallModeStateChanged";
                policy->executePolicyOperatingSystemEmergencyCallModeStateChanged((OnOffToggle::Type)notificationValue);
                break;

            case OsMobileNotificationType::ScreenState:
                getDptfManager()->getEventCache()->screenState.set(notificationValue);
                functionName = "Policy::executePolicyOperatingSystemMobileNotification";
                policy->executePolicyOperatingSystemMobileNotification(notificationType, notificationValue);
                break;

            default:
                functionName = "Policy::executePolicyOperatingSystemMobileNotification";
                policy->executePolicyOperatingSystemMobileNotification(notificationType, notificationValue);
                break;
            }
        }
        catch (policy_index_invalid ex)
        {
            // do nothing.  No item in the policy list at this index.
        }
        catch (std::exception& ex)
        {
            writeWorkItemErrorMessagePolicy(ex, functionName, i);
        }
    }
}