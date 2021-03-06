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

TOP_PATH := $(call my-dir)

include $(TOP_PATH)/SharedLib/Android.mk
include $(TOP_PATH)/SharedLib/BasicTypesLib/Android.mk
include $(TOP_PATH)/SharedLib/DptfObjectsLib/Android.mk
include $(TOP_PATH)/SharedLib/DptfTypesLib/Android.mk
include $(TOP_PATH)/SharedLib/EsifTypesLib/Android.mk
include $(TOP_PATH)/SharedLib/EventsLib/Android.mk
include $(TOP_PATH)/SharedLib/MessageLoggingLib/Android.mk
include $(TOP_PATH)/SharedLib/ParticipantControlsLib/Android.mk
include $(TOP_PATH)/SharedLib/ParticipantLib/Android.mk
include $(TOP_PATH)/SharedLib/XmlLib/Android.mk
include $(TOP_PATH)/PolicyLib/Android.mk
include $(TOP_PATH)/UnifiedParticipant/Android.mk
include $(TOP_PATH)/Policies/Android.mk
include $(TOP_PATH)/Manager/Android.mk
include $(TOP_PATH)/Resources/Android.mk
