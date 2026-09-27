LOCAL_PATH := $(call my-dir)

include $(CLEAR_VARS)

LOCAL_MODULE := gnss_xtra_probe
LOCAL_MODULE_TAGS := optional debug
LOCAL_PROPRIETARY_MODULE := true

# QMI_LOC_GET_PREDICTED_ORBITS_DATA_SOURCE_REQ_V02 and
# QMI_LOC_GET_PREDICTED_ORBITS_DATA_VALIDITY_REQ_V02 are read-only queries;
# this binary issues no QMI_LOC_INJECT_PREDICTED_ORBITS_DATA_REQ and links
# no vendor library, resolving locClientOpen/loc_sync_send_req/
# loc_sync_process_ind/locClientClose by dlsym against the already-running
# libloc_api_v02.so instead, so it carries no build- or load-time
# dependency on the GPS HAL and no risk to the position-fix path.
LOCAL_SRC_FILES := gnss_xtra_probe.c

LOCAL_C_INCLUDES := \
    $(LOCAL_PATH)/../loc_api_v02 \
    $(LOCAL_PATH)/../include

LOCAL_SHARED_LIBRARIES := liblog libdl libc

LOCAL_CFLAGS := \
    -Wall \
    -Wextra \
    -Werror \
    -D_ANDROID_

include $(BUILD_EXECUTABLE)
