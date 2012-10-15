/*
 * MOOSAsyncCommClient.cpp
 *
 *  Created on: Sep 18, 2012
 *      Author: pnewman
 */

#include <cmath>
#include <string>
#include <set>
#include <limits>
#include <iostream>
#include <iomanip>
#include <cassert>


#include "MOOS/libMOOS/Utils/MOOSUtils.h"
#include "MOOS/libMOOS/Utils/MOOSException.h"
#include "MOOS/libMOOS/Utils/MOOSScopedLock.h"

#include "MOOS/libMOOS/Comms/MOOSAsyncCommClient.h"
#include "MOOS/libMOOS/Comms/XPCTcpSocket.h"

namespace MOOS
{

#define TIMING_MESSAGE_PERIOD 3.0

bool AsyncCommsReaderDispatch(void * pParam)
{
	MOOSAsyncCommClient *pMe = (MOOSAsyncCommClient*)pParam;
	return pMe->ReadingLoop();
}

bool AsyncCommsWriterDispatch(void * pParam)
{
	MOOSAsyncCommClient *pMe = (MOOSAsyncCommClient*)pParam;

	return pMe->WritingLoop();
}

///default constructor
MOOSAsyncCommClient::MOOSAsyncCommClient()
{
	m_dfLastTimingMessage = 0.0;
}
///default destructor
MOOSAsyncCommClient::~MOOSAsyncCommClient()
{

}


std::string MOOSAsyncCommClient::HandShakeKey()
{
	return "asynchronous";
}


bool MOOSAsyncCommClient::StartThreads()
{
	m_bQuit = false;

    if(!WritingThread_.Initialise(AsyncCommsWriterDispatch,this))
        return false;

    if(!ReadingThread_.Initialise(AsyncCommsReaderDispatch,this))
            return false;

    if(!WritingThread_.Start())
        return false;

    if(!ReadingThread_.Start())
        return false;

	return true;
}

bool MOOSAsyncCommClient::Flush()
{
	return true;
}

bool MOOSAsyncCommClient::Post(CMOOSMsg & Msg)
{

	BASE::Post(Msg);

	m_OutLock.Lock();
	{
		OutGoingQueue_.AppendToMeInConstantTime(m_OutBox);
		//std::cerr<<"OutGoingQueue_ : "<<OutGoingQueue_.Size()<<"\n";
	}
	m_OutLock.UnLock();

	return true;
}

bool MOOSAsyncCommClient::OnCloseConnection()
{
	MOOS::ScopedLock WL(m_CloseConnectionLock);

	BASE::OnCloseConnection();
}


bool MOOSAsyncCommClient::WritingLoop()
{
	//we want errors not signals!
	signal(SIGPIPE, SIG_IGN);

	//this is the connect loop...
	m_pSocket = new XPCTcpSocket(m_lPort);

	while(!WritingThread_.IsQuitRequested())
	{

		int nMSToWait  = (int)(1000.0/m_nFundamentalFreq);

		if(ConnectToServer())
		{
			while(!WritingThread_.IsQuitRequested() && IsConnected() )
			{
				if(OutGoingQueue_.Size()==0)
				{
					OutGoingQueue_.WaitForPush(nMSToWait);
				}

				if(!DoWriting())
				{
					OnCloseConnection();
				}
			}
		}
		else
		{
			//this is bad....if ConnectToServer() returns false
			//it wasn't simply that we could not get hold of the server
			//it was that we misbehaved badly. We should quit..
			OnCloseConnection();
			break;
		}

	}
	//clean up on exit....
	if(m_pSocket!=NULL)
	{
		if(m_pSocket)
			delete m_pSocket;
		m_pSocket = NULL;
	}

	if(m_bQuiet)
		MOOSTrace("CMOOSAsyncCommClient::WritingLoop() quits\n");

	m_bConnected = false;

	return true;
}




bool MOOSAsyncCommClient::DoWriting()
{

	//this is the IO Loop
	try
	{

		if(!IsConnected())
			return false;

		MOOSMSG_LIST StuffToSend;

		OutGoingQueue_.AppendToOtherInConstantTime(StuffToSend);

		//and once in a while we shall send a timing
		//message (this is the new style of timing
		if((MOOS::Time()-m_dfLastTimingMessage)>TIMING_MESSAGE_PERIOD  )
		{
			CMOOSMsg Msg(MOOS_TIMING,"_async_timing",0.0,MOOS::Time());
			StuffToSend.push_front(Msg);
			m_dfLastTimingMessage= Msg.GetTime();
		}

		//convert our out box to a single packet
		CMOOSCommPkt PktTx;
		try
		{
			PktTx.Serialize(StuffToSend,true);
		}
		catch (CMOOSException e)
		{
			//clear the outbox
			throw CMOOSException("Serialisation Failed - this must be a lot of mail...");
		}


		//finally the send....
		SendPkt(m_pSocket,PktTx);

	}
	catch(const CMOOSException & e)
	{
		MOOSTrace("Exception in DoWriting() : %s\n",e.m_sReason);
		return false;//jump out to connect loop....
	}

	return true;


}



bool MOOSAsyncCommClient::ReadingLoop()
{
	//note we will rely on our sibling writing thread to handle
	//the connected and reconnecting...
	signal(SIGPIPE, SIG_IGN);


	while(!ReadingThread_.IsQuitRequested())
	{
		if(IsConnected())
		{
			if(!DoReading())
			{
				std::cerr<<"reading failed!\n";
			}
		}
		else
		{
			MOOSPause(100);
		}
	}
	std::cerr<<"READING LOOP quiting...\n";
	return true;
}

bool MOOSAsyncCommClient::DoReading()
{

	try
	{
		CMOOSCommPkt PktRx;

		ReadPkt(m_pSocket,PktRx);

		double dfLocalRxTime =MOOSLocalTime();

		m_InLock.Lock();
		{
			if(m_InBox.size()>m_nInPendingLimit)
			{
				MOOSTrace("Too many unread incoming messages [%d] : purging\n",m_InBox.size());
				MOOSTrace("The user must read mail occasionally");
				m_InBox.clear();
			}

			//extract... and please leave NULL messages there
			PktRx.Serialize(m_InBox,false,false,NULL);

			//now Serialize simply adds to the front of a list so looking
			//at the first element allows us to check for timing information
			//as supported by the threaded server class
			if(m_bDoLocalTimeCorrection)
			{
				switch(m_InBox.front().GetType())
				{
					case MOOS_TIMING:
					{
						//we have a fancy new DB upstream...
						//one that support Asynchronous Clients
						CMOOSMsg TimingMsg = m_InBox.front();
						m_InBox.pop_front();

						UpdateMOOSSkew(TimingMsg.GetDouble(),
								TimingMsg.GetTime(),
								MOOSLocalTime());
						break;
					}
					case MOOS_NULL_MSG:
					{
						//looks like we have an old fashioned DB which sends timing
						//info at the front of every packet in a null message
						//we have no corresponding outgoing packet so not much we can
						//do other than imagine it tooks as long to send to the
						//DB as to receive...
						double dfTimeSentFromDB = m_InBox.front().GetDouble();
						double dfSkew = dfTimeSentFromDB-dfLocalRxTime;
						double dfTimeSentToDBApprox =dfTimeSentFromDB+dfSkew;

						m_InBox.pop_front();

						UpdateMOOSSkew(dfTimeSentToDBApprox,
								dfTimeSentFromDB,
								dfLocalRxTime);

						break;

					}
				}
			}

			m_bMailPresent = !m_InBox.empty();
		}
		m_InLock.UnLock();

		//and here we can optionally give users an indication
		//that mail has arrived...
		if(m_pfnMailCallBack!=NULL && m_bMailPresent)
		{
			bool bUserResult = (*m_pfnMailCallBack)(m_pMailCallBackParam);
			if(!bUserResult)
				MOOSTrace("user mail callback returned false..is all ok?\n");
		}
	}
	catch(const CMOOSException & e)
	{
		MOOSTrace("Exception in DoReading() : %s\n",e.m_sReason);
		return false;
	}

	return true;
}

bool MOOSAsyncCommClient::IsRunning()
{
	return WritingThread_.IsThreadRunning() || ReadingThread_.IsThreadRunning();
}

void MOOSAsyncCommClient::DoBanner()
{
    if(m_bQuiet)
        return ;

	MOOSTrace("****************************************************\n");
	MOOSTrace("*       This is an Asynchronous MOOS Client        *\n");
	MOOSTrace("*       c. P Newman 2001-2012                      *\n");
	MOOSTrace("****************************************************\n");

}

};

