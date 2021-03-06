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

#include "PerformanceControlCapabilitiesArbitrator.h"
#include "Utility.h"

PerformanceControlCapabilitiesArbitrator::PerformanceControlCapabilitiesArbitrator()
{
}

PerformanceControlCapabilitiesArbitrator::~PerformanceControlCapabilitiesArbitrator()
{
}

void PerformanceControlCapabilitiesArbitrator::commitPolicyRequest(
	UIntN policyIndex,
	const PerformanceControlDynamicCaps& caps)
{
	updatePolicyRequest(caps, policyIndex, m_requestedUpperPState, m_requestedLowerPState);
}

Bool PerformanceControlCapabilitiesArbitrator::hasArbitratedPerformanceControlCapabilities() const
{
	if (m_requestedLowerPState.empty() && m_requestedUpperPState.empty())
	{
		return false;
	}
	return true;
}

PerformanceControlDynamicCaps PerformanceControlCapabilitiesArbitrator::arbitrate(
	UIntN policyIndex,
	const PerformanceControlDynamicCaps& caps)
{
	auto tempPolicyUpperRequests = m_requestedUpperPState;
	auto tempPolicyLowerRequests = m_requestedLowerPState;
	updatePolicyRequest(caps, policyIndex, tempPolicyUpperRequests, tempPolicyLowerRequests);
	auto lowerPStateIndex = getSmallestLowerPStateIndex(tempPolicyLowerRequests);
	auto upperPStateIndex = getBiggestUpperPStateIndex(tempPolicyUpperRequests);
	if (upperPStateIndex > lowerPStateIndex)
	{
		upperPStateIndex = lowerPStateIndex;
	}
	return PerformanceControlDynamicCaps(lowerPStateIndex, upperPStateIndex);
}

Bool PerformanceControlCapabilitiesArbitrator::arbitrateLockRequests(UIntN policyIndex, Bool lock)
{
	Bool previousLock = getArbitratedLock();
	updatePolicyLockRequest(lock, policyIndex);
	Bool newLock = getArbitratedLock();
	return (previousLock != newLock);
}

PerformanceControlDynamicCaps PerformanceControlCapabilitiesArbitrator::getArbitratedPerformanceControlCapabilities()
{
	auto lowerPStateIndex = getSmallestLowerPStateIndex(m_requestedLowerPState);
	auto upperPStateIndex = getBiggestUpperPStateIndex(m_requestedUpperPState);
	if (upperPStateIndex > lowerPStateIndex)
	{
		upperPStateIndex = lowerPStateIndex;
	}
	return PerformanceControlDynamicCaps(lowerPStateIndex, upperPStateIndex);
}

Bool PerformanceControlCapabilitiesArbitrator::getArbitratedLock() const
{
	for (auto lockRequest = m_requestedLocks.begin(); lockRequest != m_requestedLocks.end(); lockRequest++)
	{
		if (lockRequest->second == true)
		{
			return true;
		}
	}

	return false;
}

void PerformanceControlCapabilitiesArbitrator::removeRequestsForPolicy(UIntN policyIndex)
{
	m_requestedUpperPState.erase(policyIndex);
	m_requestedLowerPState.erase(policyIndex);
	m_requestedLocks.erase(policyIndex);
}

void PerformanceControlCapabilitiesArbitrator::updatePolicyRequest(
	const PerformanceControlDynamicCaps& caps,
	UIntN policyIndex,
	std::map<UIntN, UIntN>& upperRequests,
	std::map<UIntN, UIntN>& lowerRequests)
{
	auto newMax = caps.getCurrentUpperLimitIndex();
	auto newMin = caps.getCurrentLowerLimitIndex();

	if (newMax == Constants::Invalid && newMin == Constants::Invalid)
	{
		upperRequests.erase(policyIndex);
		lowerRequests.erase(policyIndex);
	}
	else
	{
		upperRequests[policyIndex] = newMax;
		lowerRequests[policyIndex] = newMin;
	}
}

void PerformanceControlCapabilitiesArbitrator::updatePolicyLockRequest(Bool lock, UIntN policyIndex)
{
	m_requestedLocks[policyIndex] = lock;
}

UIntN PerformanceControlCapabilitiesArbitrator::getBiggestUpperPStateIndex(std::map<UIntN, UIntN>& upperRequests) const
{
	UIntN biggestMaxPerfIndex = Constants::Invalid;
	for (auto request = upperRequests.begin(); request != upperRequests.end(); ++request)
	{
		if (biggestMaxPerfIndex == Constants::Invalid)
		{
			biggestMaxPerfIndex = request->second;
		}
		else
		{
			biggestMaxPerfIndex = std::max(biggestMaxPerfIndex, request->second);
		}
	}
	return biggestMaxPerfIndex;
}

UIntN PerformanceControlCapabilitiesArbitrator::getSmallestLowerPStateIndex(std::map<UIntN, UIntN>& lowerRequests) const
{
	UIntN smallestLowPerfIndex = Constants::Invalid;
	for (auto request = lowerRequests.begin(); request != lowerRequests.end(); ++request)
	{
		if (smallestLowPerfIndex == Constants::Invalid)
		{
			smallestLowPerfIndex = request->second;
		}
		else
		{
			smallestLowPerfIndex = std::min(smallestLowPerfIndex, request->second);
		}
	}
	return smallestLowPerfIndex;
}
