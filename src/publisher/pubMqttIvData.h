//-----------------------------------------------------------------------------
// 2023 Ahoy, https://ahoydtu.de
// Creative Commons - https://creativecommons.org/licenses/by-nc-sa/4.0/deed
//-----------------------------------------------------------------------------

#ifndef __PUB_MQTT_IV_DATA_H__
#define __PUB_MQTT_IV_DATA_H__

#include "../utils/dbg.h"
#include "../hm/hmSystem.h"
#include "pubMqttDefs.h"

typedef std::function<void(const char *subTopic, const char *payload, bool retained)> pubMqttPublisherType;

template<class HMSYSTEM>
class PubMqttIvData {
    public:
        void setup(HMSYSTEM *sys, uint32_t *utcTs, std::queue<uint8_t> *sendList) {
            mSys          = sys;
            mUtcTimestamp = utcTs;
            mSendList     = sendList;
            mState        = IDLE;

            memset(mIvLastRTRpub, 0, MAX_NUM_INVERTERS * 4);
            mRTRDataHasBeenSent = false;

            mTable[IDLE]        = &PubMqttIvData::stateIdle;
            mTable[START]       = &PubMqttIvData::stateStart;
            mTable[FIND_NXT_IV] = &PubMqttIvData::stateFindNxtIv;
            mTable[SEND_DATA]   = &PubMqttIvData::stateSend;
            mTable[SEND_TOTALS] = &PubMqttIvData::stateSendTotals;
        }

        void loop() {
            (this->*mTable[mState])();
            yield();
        }

        bool start(void) {
            if(IDLE != mState)
                return false;

            mRTRDataHasBeenSent = false;
            mState = START;
            return true;
        }

        void setPublishFunc(pubMqttPublisherType cb) {
            mPublish = cb;
        }

    private:
        enum State {IDLE, START, FIND_NXT_IV, SEND_DATA, SEND_TOTALS, NUM_STATES};
        typedef void (PubMqttIvData::*StateFunction)();

        void stateIdle() {
            ; // nothing to do
        }

        void stateStart() {
            mLastIvId = 0;
            if(!mSendList->empty()) {
                mCmd = mSendList->front();

                if((RealTimeRunData_Debug != mCmd) || !mRTRDataHasBeenSent) {
                    mSendTotals = (RealTimeRunData_Debug == mCmd);
                    memset(mTotal, 0, sizeof(float) * 4);
                    mState = FIND_NXT_IV;
                } else
                    mSendList->pop();
            } else
                mState = IDLE;
        }

        void stateFindNxtIv() {
            bool found = false;

            for (; mLastIvId < mSys->getNumInverters(); mLastIvId++) {
                mIv = mSys->getInverterByPos(mLastIvId);
                if (NULL != mIv) {
                    if (mIv->config->enabled) {
                        found = true;
                        break;
                    }
                }
            }

            mPos = 0;
            if(found)
                mState = SEND_DATA;
            else if(mSendTotals)
                mState = SEND_TOTALS;
            else
                mState = IDLE;
        }

        void stateSend() {
            record_t<> *rec = mIv->getRecordStruct(mCmd);
            uint32_t lastTs = mIv->getLastTs(rec);
            bool pubData = (lastTs > 0);
            if (mCmd == RealTimeRunData_Debug)
                pubData &= (lastTs != mIvLastRTRpub[mIv->id]);

            if (pubData) {
                mIvLastRTRpub[mIv->id] = lastTs;
                //for (uint8_t i = 0; i < rec->length; i++) {
                if(mPos < rec->length) {
                    bool retained = false;
                    if (mCmd == RealTimeRunData_Debug) {
                        switch (rec->assign[mPos].fieldId) {
                            case FLD_YT:
                            case FLD_YD:
                                if ((rec->assign[mPos].ch == CH0) && (!mIv->isProducing(*mUtcTimestamp))) { // avoids returns to 0 on restart
                                    mPos++;
                                    return;
                                }
                                retained = true;
                                break;
                        }

                        // calculate total values for RealTimeRunData_Debug
                        if (CH0 == rec->assign[mPos].ch) {
                            switch (rec->assign[mPos].fieldId) {
                                case FLD_PAC:
                                    mTotal[0] += mIv->getValue(mPos, rec);
                                    break;
                                case FLD_YT:
                                    mTotal[1] += mIv->getValue(mPos, rec);
                                    break;
                                case FLD_YD:
                                    mTotal[2] += mIv->getValue(mPos, rec);
                                    break;
                                case FLD_PDC:
                                    mTotal[3] += mIv->getValue(mPos, rec);
                                    break;
                            }
                        }
                    }

                    snprintf(mSubTopic, 32 + MAX_NAME_LENGTH, "%s/ch%d/%s", mIv->config->name, rec->assign[mPos].ch, fields[rec->assign[mPos].fieldId]);
                    snprintf(mVal, 40, "%g", ah::round3(mIv->getValue(mPos, rec)));
                    mPublish(mSubTopic, mVal, retained);
                    mPos++;
                } else
                    mState = FIND_NXT_IV;
            } else
                mState = FIND_NXT_IV;
        }

        void stateSendTotals() {
            uint8_t fieldId;
            //for (uint8_t i = 0; i < 4; i++) {
            if(mPos < 4) {
                bool retained = true;
                switch (mPos) {
                    default:
                    case 0:
                        fieldId = FLD_PAC;
                        retained = false;
                        break;
                    case 1:
                        fieldId = FLD_YT;
                        break;
                    case 2:
                        fieldId = FLD_YD;
                        break;
                    case 3:
                        fieldId = FLD_PDC;
                        retained = false;
                        break;
                }
                snprintf(mSubTopic, 32 + MAX_NAME_LENGTH, "total/%s", fields[fieldId]);
                snprintf(mVal, 40, "%g", ah::round3(mTotal[mPos]));
                mPublish(mSubTopic, mVal, retained);
                mPos++;
            } else {
                mSendList->pop();
                mState = START;
            }

            mRTRDataHasBeenSent = true;
        }

        HMSYSTEM *mSys;
        uint32_t *mUtcTimestamp;
        pubMqttPublisherType mPublish;
        State mState;
        StateFunction mTable[NUM_STATES];

        uint8_t mCmd;
        uint8_t mLastIvId;
        bool mSendTotals;
        float mTotal[4];

        Inverter<> *mIv;
        uint8_t mPos;
        uint32_t mIvLastRTRpub[MAX_NUM_INVERTERS];
        bool mRTRDataHasBeenSent;

        char mSubTopic[32 + MAX_NAME_LENGTH + 1];
        char mVal[40];

        std::queue<uint8_t> *mSendList;
};

#endif /*__PUB_MQTT_IV_DATA_H__*/
