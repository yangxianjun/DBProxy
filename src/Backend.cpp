#include <fstream>

#include "RedisParse.h"
#include "Client.h"
#include "SSDBProtocol.h"
#include "SSDBWaitReply.h"

#include "Backend.h"

std::vector<BackendLogicSession*>    gBackendClients;

BackendExtNetSession::BackendExtNetSession(BaseLogicSession::PTR logicSession) : ExtNetSession(logicSession)
{
    cout << "������������������" << endl;
    mRedisParse = nullptr;
}

BackendExtNetSession::~BackendExtNetSession()
{
    cout << "�Ͽ��������������" << endl;
    if (mRedisParse != nullptr)
    {
        parse_tree_del(mRedisParse);
        mRedisParse = nullptr;
    }
}

/*  �յ�db server��reply�������������߼���Ϣ����   */
int BackendExtNetSession::onMsg(const char* buffer, int len)
{
    bool isExist = mRedisParse != nullptr;
    int totalLen = 0;

    const char c = buffer[0];
    if (mRedisParse != nullptr ||
        !IS_NUM(c))
    {
        /*  redis reply */
        char* parseEndPos = (char*)buffer;
        char* parseStartPos = parseEndPos;
        string lastPacket;
        while (totalLen < len)
        {
            if (mRedisParse == nullptr)
            {
                mRedisParse = parse_tree_new();
            }

            int parseRet = parse(mRedisParse, &parseEndPos, (char*)buffer+len);
            totalLen += (parseEndPos - parseStartPos);

            if (parseRet == REDIS_OK)
            {
                if (mCache.empty())
                {
                    pushDataMsgToLogicThread(parseStartPos, parseEndPos - parseStartPos);
                }
                else
                {
                    mCache += string(parseStartPos, parseEndPos - parseStartPos);
                    pushDataMsgToLogicThread(mCache.c_str(), mCache.size());
                    mCache.clear();
                }
                parseStartPos = parseEndPos;
                parse_tree_del(mRedisParse);
                mRedisParse = nullptr;
            }
            else if (parseRet == REDIS_RETRY)
            {
                mCache += string(parseStartPos, parseEndPos - parseStartPos);
                break;
            }
            else
            {
                break;
            }
        }
    }
    else
    {
        /*  ssdb reply    */
        char* parseStartPos = (char*)buffer;
        int leftLen = len;
        int packetLen = 0;
        while ((packetLen = SSDBProtocolResponse::check_ssdb_packet(parseStartPos, leftLen)) > 0)
        {
            pushDataMsgToLogicThread(parseStartPos, packetLen);

            totalLen += packetLen;
            leftLen -= packetLen;
            parseStartPos += packetLen;
        }
    }

    return totalLen;
}

void BackendLogicSession::onEnter() 
{
    gBackendClients.push_back(this);
}

void BackendLogicSession::onClose()
{
    for (auto it = gBackendClients.begin(); it != gBackendClients.end(); ++it)
    {
        if (*it == this)
        {
            gBackendClients.erase(it);
            break;
        }
    }
    
    /*  ����db server�Ͽ��󣬶Եȴ��˷�������Ӧ�Ŀͻ����������ô���(���ظ��ͻ���)  */
    while (!mPendingWaitReply.empty())
    {
        ClientLogicSession* client = nullptr;
        auto w = mPendingWaitReply.front();
        auto wp = w.lock();
        if (wp != nullptr)
        {
            wp->setError();
            client = wp->getClient();
        }
        mPendingWaitReply.pop();
        if (client != nullptr)
        {
            client->processCompletedReply();
        }
    }
}

void BackendLogicSession::pushPendingWaitReply(std::weak_ptr<BaseWaitReply> w)
{
    mPendingWaitReply.push(w);
}

void BackendLogicSession::setID(int id)
{
    mID = id;
}

int BackendLogicSession::getID() const
{
    return mID;
}

/*  �յ�����㷢�͹�����db reply  */
void BackendLogicSession::onMsg(const char* buffer, int len)
{
    if (!mPendingWaitReply.empty())
    {
        ClientLogicSession* client = nullptr;
        auto w = mPendingWaitReply.front();
        auto wp = w.lock();
        if (wp != nullptr)
        {
            wp->onBackendReply(getSocketID(), buffer, len);
            client = wp->getClient();
        }
        mPendingWaitReply.pop();
        if (client != nullptr)
        {
            client->processCompletedReply();
        }
    }
}

BackendLogicSession* findBackendByID(int id)
{
    BackendLogicSession* ret = nullptr;
    for (auto& v : gBackendClients)
    {
        if (v->getID() == id)
        {
            ret = v;
            break;
        }
    }

    return ret;
}