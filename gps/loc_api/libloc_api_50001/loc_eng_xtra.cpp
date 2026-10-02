/* Copyright (c) 2009-2013, The Linux Foundation. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above
 *       copyright notice, this list of conditions and the following
 *       disclaimer in the documentation and/or other materials provided
 *       with the distribution.
 *     * Neither the name of The Linux Foundation, nor the names of its
 *       contributors may be used to endorse or promote products derived
 *       from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED "AS IS" AND ANY EXPRESS OR IMPLIED
 * WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
 * BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE
 * OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN
 * IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 */

#define LOG_NDDEBUG 0
#define LOG_TAG "LocSvc_eng"

#include <stdint.h>
#include <pthread.h>
#include <time.h>
#include <loc_eng.h>
#include <MsgTask.h>
#include <LocTimer.h>
#include "log_util.h"
#include "platform_lib_includes.h"
#include "XtraFormatGuard.h"
#include "XtraQmiInjector.h"

using namespace loc_core;

static uint64_t xtraRetryClockMs()
{
    struct timespec now;
    clock_gettime(CLOCK_BOOTTIME, &now);
    return (uint64_t)now.tv_sec * 1000u + (uint64_t)now.tv_nsec / 1000000u;
}

/* The framework download callback runs on the adapter's MsgTask, the thread
   loc_eng_init creates through the framework's create_thread_cb, the same
   thread LocEngRequestXtra uses for the modem's own fresh-orbit request. */
struct LocEngXtraRetryRequest : public LocMsg {
    loc_eng_data_s_type* const mLocEng;
    const unsigned int mEpoch;
    const unsigned int mRetry;
    inline LocEngXtraRetryRequest(loc_eng_data_s_type* locEng,
                                  unsigned int epoch, unsigned int retry) :
        LocMsg(), mLocEng(locEng), mEpoch(epoch), mRetry(retry)
    {
        locallog();
    }
    virtual void proc() const;
    inline void locallog() const {
        LOC_LOGV("LocEngXtraRetryRequest: retry=%u/3", mRetry);
    }
    inline virtual void log() const {
        locallog();
    }
};

class XtraRetryTimer : public LocTimer {
    /* LocTimerDelegate::expire() clears the timer under its own lock and
       calls timeOutCallback() after releasing it, so an accepted injection
       or a reschedule can land between the two. mDueMs identifies the
       schedule the callback belongs to; a callback that arrives before it
       belongs to a stopped schedule. */
    static const uint64_t kExpirySlackMs = 1000;
    pthread_mutex_t mMutex;
    pthread_cond_t mCallbackDone;
    loc_eng_data_s_type* mLocEng;
    unsigned int mRetriesRequested;
    bool mScheduled;
    uint64_t mDueMs;
    bool mActive;
    uint64_t mLastRetryMs;
    bool mCallbackRunning;
    bool mInjectionRunning;
    unsigned int mEpoch;

    void retireLocked() {
        mActive = false;
        ++mEpoch;
        mLocEng = NULL;
        mRetriesRequested = 0;
        mScheduled = false;
        stop();
        while (mCallbackRunning || mInjectionRunning)
            pthread_cond_wait(&mCallbackDone, &mMutex);
    }

public:
    XtraRetryTimer() : mLocEng(NULL), mRetriesRequested(0),
                       mScheduled(false), mDueMs(0), mActive(false),
                       mLastRetryMs(0), mCallbackRunning(false),
                       mInjectionRunning(false), mEpoch(0) {
        pthread_mutex_init(&mMutex, NULL);
        pthread_cond_init(&mCallbackDone, NULL);
    }

    ~XtraRetryTimer() {
        stop();
        pthread_cond_destroy(&mCallbackDone);
        pthread_mutex_destroy(&mMutex);
    }

    void initialize(loc_eng_data_s_type* locEng) {
        pthread_mutex_lock(&mMutex);
        retireLocked();
        mActive = true;
        mLocEng = locEng;
        pthread_mutex_unlock(&mMutex);
    }

    void cleanup() {
        pthread_mutex_lock(&mMutex);
        retireLocked();
        pthread_mutex_unlock(&mMutex);
    }

    void recordResult(bool accepted) {
        static const uint32_t delaysMs[] = {30000, 120000, 600000};
        pthread_mutex_lock(&mMutex);
        if (mActive) {
            if (accepted) {
                stop();
                mScheduled = false;
                mRetriesRequested = 0;
            } else if (!mScheduled && mLocEng != NULL) {
                const uint64_t now = xtraRetryClockMs();
                if (mRetriesRequested == 3 && now - mLastRetryMs >= 3600000u)
                    mRetriesRequested = 0;
                if (mRetriesRequested < 3) {
                    const uint32_t delay = delaysMs[mRetriesRequested];
                    mScheduled = start(delay, false);
                    if (mScheduled) {
                        mDueMs = now + delay;
                        LOC_LOGI("XTRA retry scheduled: delay_ms=%u retry=%u/3",
                                 delay, mRetriesRequested + 1);
                    } else {
                        LOC_LOGE("XTRA retry timer failed to start");
                    }
                }
            }
        }
        pthread_mutex_unlock(&mMutex);
    }

    unsigned int currentEpoch() {
        pthread_mutex_lock(&mMutex);
        const unsigned int epoch = mEpoch;
        pthread_mutex_unlock(&mMutex);
        return epoch;
    }

    bool beginInjection(unsigned int epoch) {
        pthread_mutex_lock(&mMutex);
        const bool current = mActive && mEpoch == epoch;
        if (current)
            mInjectionRunning = true;
        pthread_mutex_unlock(&mMutex);
        return current;
    }

    void endInjection() {
        pthread_mutex_lock(&mMutex);
        mInjectionRunning = false;
        pthread_cond_broadcast(&mCallbackDone);
        pthread_mutex_unlock(&mMutex);
    }

    bool beginRetryCallback(unsigned int epoch) {
        pthread_mutex_lock(&mMutex);
        const bool current = mActive && mEpoch == epoch;
        if (current)
            mCallbackRunning = true;
        pthread_mutex_unlock(&mMutex);
        return current;
    }

    void endRetryCallback() {
        pthread_mutex_lock(&mMutex);
        mCallbackRunning = false;
        pthread_cond_broadcast(&mCallbackDone);
        pthread_mutex_unlock(&mMutex);
    }

    virtual void timeOutCallback() {
        pthread_mutex_lock(&mMutex);
        const uint64_t now = xtraRetryClockMs();
        if (!mScheduled || now + kExpirySlackMs < mDueMs) {
            LOC_LOGI("XTRA retry expiry ignored: scheduled=%d", mScheduled);
            pthread_mutex_unlock(&mMutex);
            return;
        }
        mScheduled = false;
        if (mActive && mLocEng != NULL && mLocEng->adapter != NULL &&
                mRetriesRequested < 3) {
            ++mRetriesRequested;
            mLastRetryMs = now;
            LOC_LOGI("XTRA retry requesting download: retry=%u/3",
                     mRetriesRequested);
            mLocEng->adapter->sendMsg(new LocEngXtraRetryRequest(
                    mLocEng, mEpoch, mRetriesRequested));
        }
        pthread_mutex_unlock(&mMutex);
    }
};

static XtraRetryTimer xtraRetryTimer;

void LocEngXtraRetryRequest::proc() const
{
    if (!xtraRetryTimer.beginRetryCallback(mEpoch)) {
        LOC_LOGI("XTRA retry skipped after HAL cleanup: retry=%u/3", mRetry);
        return;
    }
    gps_xtra_download_request callback =
            mLocEng->xtra_module_data.download_request_cb;
    if (callback != NULL) {
        CALLBACK_LOG_CALLFLOW("download_request_cb", %p, mLocEng);
        callback();
    } else {
        LOC_LOGE("XTRA retry has no download callback: retry=%u/3", mRetry);
    }
    xtraRetryTimer.endRetryCallback();
}

void loc_eng_xtra_cleanup()
{
    xtraRetryTimer.cleanup();
}

struct LocEngRequestXtraServer : public LocMsg {
    LocEngAdapter* mAdapter;
    inline LocEngRequestXtraServer(LocEngAdapter* adapter) :
        LocMsg(), mAdapter(adapter)
    {
        locallog();
    }
    inline virtual void proc() const {
        mAdapter->requestXtraServer();
    }
    inline void locallog() const {
        LOC_LOGV("LocEngRequestXtraServer");
    }
    inline virtual void log() const {
        locallog();
    }
};

/* GnssPsdsDownloader rotates mirrors. The fingerprint identifies the exact
   downloaded payload associated with each QMI injection result. */
static uint32_t xtra_fnv1a(const char* data, int len)
{
    uint32_t hash = 0x811c9dc5u;
    for (int i = 0; i < len; i++) {
        hash ^= (uint8_t)data[i];
        hash *= 0x01000193u;
    }
    return hash;
}

struct LocEngInjectXtraData : public LocMsg {
    char* mData;
    const int mLen;
    const unsigned int mEpoch;
    inline LocEngInjectXtraData(char* data, int len):
        LocMsg(),
        mData(new char[len]), mLen(len), mEpoch(xtraRetryTimer.currentEpoch())
    {
        memcpy((void*)mData, (void*)data, len);
        locallog();
    }
    inline ~LocEngInjectXtraData()
    {
        delete[] mData;
    }
    inline virtual void proc() const {
        if (!xtraRetryTimer.beginInjection(mEpoch)) {
            LOC_LOGI("XTRA injection skipped after HAL cleanup");
            return;
        }
        /* Each part is limited to 1024 bytes by location_service_v02.h. */
        LOC_LOGI("XTRA QMI dispatch: length=%d fnv1a=0x%08x "
                 "magic=%02x%02x expected_parts=%d",
                 mLen, xtra_fnv1a(mData, mLen),
                 mLen > 0 ? (uint8_t)mData[0] : 0,
                 mLen > 1 ? (uint8_t)mData[1] : 0,
                 mLen > 0 ? ((mLen - 1) / 1024) + 1 : 0);
        /* The prebuilt adapter reports QMI transport success even when the
           modem rejects the assembled file. Read each modem indication. */
        const XtraInjectionResult result = injectXtraWithModemStatus(mData, mLen);
        LOC_LOGI("XTRA QMI result: accepted=%d client=%d modem=%d part=%u",
                 result.accepted, result.clientStatus, result.modemStatus,
                 result.partNumber);
        xtraRetryTimer.recordResult(result.accepted);
        xtraRetryTimer.endInjection();
    }
    inline  void locallog() const {
        LOC_LOGI("XTRA injection queued: length=%d fnv1a=0x%08x data=%p",
                 mLen, xtra_fnv1a(mData, mLen), mData);
    }
    inline virtual void log() const {
        locallog();
    }
};

struct LocEngSetXtraVersionCheck : public LocMsg {
    LocEngAdapter *mAdapter;
    int mCheck;
    inline LocEngSetXtraVersionCheck(LocEngAdapter* adapter,
                                        int check):
        mAdapter(adapter), mCheck(check) {}
    inline virtual void proc() const {
        locallog();
        mAdapter->setXtraVersionCheck(mCheck);
    }
    inline void locallog() const {
        LOC_LOGD("%s:%d]: mCheck: %d",
                 __func__, __LINE__, mCheck);
    }
    inline virtual void log() const {
        locallog();
    }
};

/*===========================================================================
FUNCTION    loc_eng_xtra_init

DESCRIPTION
   Initialize XTRA module.

DEPENDENCIES
   N/A

RETURN VALUE
   0: success

SIDE EFFECTS
   N/A

===========================================================================*/
int loc_eng_xtra_init (loc_eng_data_s_type &loc_eng_data,
                       GpsXtraExtCallbacks* callbacks)
{
    int ret_val = -1;
    loc_eng_xtra_data_s_type *xtra_module_data_ptr;
    ENTRY_LOG();

    if(callbacks == NULL) {
        LOC_LOGE("loc_eng_xtra_init: failed, cb is NULL");
    } else {
        xtra_module_data_ptr = &loc_eng_data.xtra_module_data;
        xtra_module_data_ptr->download_request_cb = callbacks->download_request_cb;
        xtra_module_data_ptr->report_xtra_server_cb = callbacks->report_xtra_server_cb;
        xtraRetryTimer.initialize(&loc_eng_data);

        ret_val = 0;
    }
    EXIT_LOG(%d, ret_val);
    return ret_val;
}

/*===========================================================================
FUNCTION    loc_eng_xtra_inject_data

DESCRIPTION
   Injects XTRA file into the engine but buffers the data if engine is busy.

DEPENDENCIES
   N/A

RETURN VALUE
   0

SIDE EFFECTS
   N/A

===========================================================================*/
int loc_eng_xtra_inject_data(loc_eng_data_s_type &loc_eng_data,
                             char* data, int length)
{
    ENTRY_LOG();
    const XtraFormatStatus format =
            classifyXtraFormat(data, length > 0 ? length : 0);
    if (format != XTRA_FORMAT_SUPPORTED) {
        LOC_LOGE("XTRA injection refused: format=%d length=%d", format, length);
        xtraRetryTimer.recordResult(false);
        EXIT_LOG(%d, -1);
        return -1;
    }
    LocEngAdapter* adapter = loc_eng_data.adapter;
    adapter->sendMsg(new LocEngInjectXtraData(data, length));
    EXIT_LOG(%d, 0);
    return 0;
}
/*===========================================================================
FUNCTION    loc_eng_xtra_request_server

DESCRIPTION
   Request the Xtra server url from the modem

DEPENDENCIES
   N/A

RETURN VALUE
   0

SIDE EFFECTS
   N/A

===========================================================================*/
int loc_eng_xtra_request_server(loc_eng_data_s_type &loc_eng_data)
{
    ENTRY_LOG();
    LocEngAdapter* adapter = loc_eng_data.adapter;
    adapter->sendMsg(new LocEngRequestXtraServer(adapter));
    EXIT_LOG(%d, 0);
    return 0;
}
/*===========================================================================
FUNCTION    loc_eng_xtra_version_check

DESCRIPTION
   Injects the enable/disable value for checking XTRA version
   that is specified in gps.conf

DEPENDENCIES
   N/A

RETURN VALUE
   none

SIDE EFFECTS
   N/A

===========================================================================*/
void loc_eng_xtra_version_check(loc_eng_data_s_type &loc_eng_data,
                                int check)
{
    ENTRY_LOG();
    LocEngAdapter *adapter = loc_eng_data.adapter;
    adapter->sendMsg(new LocEngSetXtraVersionCheck(adapter, check));
    EXIT_LOG(%d, 0);
}
