#pragma once
#pragma comment(lib, "ws2_32.lib")
#include <iostream>
#include <winsock2.h>
#include <WS2tcpip.h>
#include <vector>
#include <thread>
#include <atomic>
#include <concurrent_unordered_map.h>
#include <concurrent_queue.h>

#include "RingBuffer.h"
#include "Protocol.h"
#include "ThreadLocalMemoryPool.h"

#include "GlobalValue.h"

using SESSION_UID = long long;

class SESSION
{
public:
	SESSION()
	: recvQ(RINGBUFFER_SIZE)
	{
	}

	void Reset()
	{
		sessionSocket = 0;
		sessionUID = 0;
		releaseFlag = true;
		sessionIndex = 0;
		ZeroMemory(&recvOverlapped, sizeof(recvOverlapped));
		ZeroMemory(&sendOverlapped, sizeof(sendOverlapped));
		recvQ.Reset();
		ioCount = 0;
	}

	void ResetRecvOverlapped()
	{
		ZeroMemory(&recvOverlapped, sizeof(recvOverlapped));
	}

	void ResetSendOverlapped()
	{
		ZeroMemory(&sendOverlapped, sizeof(sendOverlapped));
	}

	void Lock() { lock.lock(); }
	void Unlock() { lock.unlock(); }

	void SetReleaseState(bool release) { releaseFlag = release; }
	bool IsReleased() { return releaseFlag; }

	SOCKET									sessionSocket;
	SESSION_UID								sessionUID;
	bool									releaseFlag;
	int										sessionIndex;
	OVERLAPPED								recvOverlapped;
	OVERLAPPED								sendOverlapped;
	RingBuffer								recvQ;
	std::atomic<int>						ioCount;
	std::mutex								lock;
	Concurrency::concurrent_queue<MESSAGE*> sendQ;
	Concurrency::concurrent_queue<MESSAGE*> sendPendingQ;
};

class NetServer
{
public:
	NetServer();

	bool Start(const char* ip, short port, int workerThreadCnt, bool tcpNagleOn, int maxUserCnt);
	bool Send(SESSION_UID sessionUID, MESSAGE* pPacket);
	bool Disconnect(SESSION_UID sessionUID);

	//Message
	MESSAGE* AllocateMessage();
	bool	 FreeMessage(MESSAGE* pMessage);

protected:
	virtual bool OnConnectionRequest(char* pClientIP, short port) = 0;
	virtual void OnRecv(SESSION_UID sessionUID, MESSAGE* pMessage) = 0;
	virtual void OnClientJoin(SESSION_UID sessionUID) = 0;
	virtual void OnClientLeave(SESSION_UID sessionUID) = 0;

private:
	void WorkerThread();
	void AcceptThread();
	void SendThread();

private:
	void AfterRecvProcess(SESSION* pSession, DWORD transferredBytes);
	void AfterSendProcess(SESSION* pSession);
	void PostRecv(SESSION* pSession);
	void PostSend(SESSION* pSession);

	SESSION* GetSession(SESSION_UID sessionUID);
	void	 ReleaseSession(SESSION* pSession);
	bool	 PreventRelease(SESSION* pSession);
	bool	 UnlockPrevent(SESSION* pSession);

private:
	SOCKET					 m_listenSocket;
	HANDLE					 m_hIocp;
	std::vector<std::thread> m_vecWorkerThread;
	std::vector<std::thread> m_vecSendThread;
	std::thread				 m_AcceptThread;
	std::atomic<int>		 m_AtomicCurrentClientCount;
	std::atomic<int>		 m_AtomicSessionUID;
	int						 m_MaxClientCnt;

	SESSION* m_SessionArray = nullptr;

	Concurrency::concurrent_queue<int> m_queueSessionIndexArray;
	ThreadLocalMemoryPool<MESSAGE>	 m_MessagePool;
};
