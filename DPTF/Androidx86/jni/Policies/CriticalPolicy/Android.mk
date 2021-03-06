################################################################################
## Copyright (C) 2006 The Android Open Source Project
## 
## Unless otherwise agreed by Intel in writing, you may not remove or alter
## this notice or any other notice embedded in Materials by Intel or Intel’s
## suppliers or licensors in any way.
##
## Licensed under the Apache License, Version 2.0 (the "License"); you may not 
## use this file except in compliance with the License.
##
## You may obtain a copy of the License at
##     http://www.apache.org/licenses/LICENSE-2.0
##
## Unless required by applicable law or agreed to in writing, software
## distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
## WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
## 
## See the License for the specific language governing permissions and
## limitations under the License.
##
################################################################################
LOCAL_PATH := $(call my-dir)

include $(CLEAR_VARS)

DPTF_SRC = ../../../../Sources
DPTF_INC = $(LOCAL_PATH)/$(DPTF_SRC)

LOCAL_MODULE := DptfPolicyCritical
LOCAL_MODULE_OWNER := intel
LOCAL_PROPRIETARY_MODULE := true

LOCAL_C_INCLUDES := \
        prebuilts/ndk/current/sources/cxx-stl/llvm-libc++/include \
	$(DPTF_INC) \
	$(DPTF_INC)/../../Common \
	$(DPTF_INC)/SharedLib \
	$(DPTF_INC)/SharedLib/BasicTypesLib \
	$(DPTF_INC)/SharedLib/EsifTypesLib \
	$(DPTF_INC)/SharedLib/DptfTypesLib \
	$(DPTF_INC)/SharedLib/DptfObjectsLib \
	$(DPTF_INC)/SharedLib/ParticipantControlsLib \
	$(DPTF_INC)/SharedLib/ParticipantLib \
	$(DPTF_INC)/SharedLib/EventsLib \
	$(DPTF_INC)/SharedLib/MessageLoggingLib \
	$(DPTF_INC)/SharedLib/XmlLib \
	$(DPTF_INC)/Policies/PolicyLib \
	$(DPTF_INC)/Policies/CriticalPolicy \
	$(DPTF_INC)/ThirdParty

LOCAL_SHARED_LIBRARIES := \
	libnativehelper \
	libcutils \
	libutils \
	liblog \
	libdl

LOCAL_CFLAGS += -std=c++11 -fexceptions -frtti -Wall -fPIC \
	-DESIF_ATTR_OS_ANDROID -DESIF_ATTR_USER \
	-Wno-error=non-virtual-dtor

LOCAL_STATIC_LIBRARIES := DptfPolicy DptfParticipantLib DptfObjectsLib DptfParticipantControlsLib DptfParticipant DptfShared DptfEventsLib DptfMessageLoggingLib DptfXmlLib DptfEsifTypesLib DptfTypesLib DptfBasicTypesLib 

FILE_LIST := $(wildcard $(LOCAL_PATH)/$(DPTF_SRC)/Policies/CriticalPolicy/*.cpp)
LOCAL_SRC_FILES := $(FILE_LIST:$(LOCAL_PATH)/%=%)

LOCAL_SDK_VERSION := 21
LOCAL_NDK_STL_VARIANT := c++_shared
include $(BUILD_SHARED_LIBRARY)
