/******************************************************************************
** Copyright (c) 2013-2017 Intel Corporation All Rights Reserved
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

#pragma once

#include "Dptf.h"
#include "PolicyServicesInterfaceContainer.h"
#include "DomainProperties.h"
#include "ActiveControlStaticCapsCachedProperty.h"
#include "XmlNode.h"
#include "ActiveCoolingControlFacadeInterface.h"

// TODO: rename as a Facade
// this class provides an easy-to-use interface and arbitration for fan speed requests

class dptf_export ActiveCoolingControl : public ActiveCoolingControlFacadeInterface
{
public:
	ActiveCoolingControl(
		UIntN participantIndex,
		UIntN domainIndex,
		const DomainProperties& domainProperties,
		const ParticipantProperties& participantProperties,
		const PolicyServicesInterfaceContainer& policyServices);
	~ActiveCoolingControl(void);

	// control capabilities
	virtual Bool supportsActiveCoolingControls() override;
	virtual Bool supportsFineGrainControl() override;

	// fan speed requests
	virtual void requestFanSpeedPercentage(UIntN requestorIndex, const Percentage& fanSpeed) override;
	virtual void requestActiveControlIndex(UIntN requestorIndex, UIntN activeControlIndex) override;
	virtual void forceFanOff(void) override;
	virtual void setControl(Percentage activeCoolingControlFanSpeed) override;

	// properties
	virtual const ActiveControlStaticCaps& getCapabilities() override;
	virtual void refreshCapabilities() override;
	virtual ActiveControlStatus getStatus() override;
	virtual UIntN getSmallestNonZeroFanSpeed() override;
	virtual Bool hasValidActiveControlSet() override;

	// status
	virtual std::shared_ptr<XmlNode> getXml() override;

	static const UIntN FanOffIndex = 10;

private:
	// services
	PolicyServicesInterfaceContainer m_policyServices;

	// control properties
	DomainProperties m_domainProperties;
	ParticipantProperties m_participantProperties;
	UIntN m_participantIndex;
	UIntN m_domainIndex;
	ActiveControlStaticCapsCachedProperty m_staticCaps;

	// fan speed request arbitration
	std::map<UIntN, Percentage> m_fanSpeedRequestTable;
	std::map<UIntN, UIntN> m_activeControlRequestTable;
	Percentage m_lastFanSpeedRequest;
	UIntN m_lastFanSpeedRequestIndex;
	void updateFanSpeedRequestTable(UIntN requestorIndex, const Percentage& fanSpeed);
	Percentage chooseHighestFanSpeedRequest();
	void updateActiveControlRequestTable(UIntN requestorIndex, UIntN activeControlIndex);
	UIntN chooseHighestActiveControlIndex();
};
