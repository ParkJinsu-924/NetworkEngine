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

#include "MemoryPool.h"
#include "RingBuffer.h"
#include "Protocol.h"

class SESSION
{
public:
	SESSION()
	: recvQ(RINGBUFFER_SIZE)
	{
	}

	void Reset()
	{
		sessionUID = 0;
		sessionSocket = 0;
		ZeroMemory(&recvOverlapped, sizeof(recvOverlapped));
		ZeroMemory(&sendOverlapped, sizeof(sendOverlapped));
		recvQ.Reset();
	}

	void ResetRecvOverlapped()
	{
		ZeroMemory(&recvOverlapped, sizeof(recvOverlapped));
	}

	void ResetSendOverlapped()
	{
		ZeroMemory(&sendOverlapped, sizeof(sendOverlapped));
	}

	SOCKET									sessionSocket;
	long long								sessionUID;
	OVERLAPPED								recvOverlapped;
	OVERLAPPED								sendOverlapped;
	RingBuffer								recvQ;
	std::atomic<int>						ioCount;
	Concurrency::concurrent_queue<MESSAGE*> sendQ;
	Concurrency::concurrent_queue<MESSAGE*> sendPendingQ;
};

class NetServer
{
public: 
	using SESSION_UID = long long;

public:
	NetServer();

	bool Start(const char* ip, short port, int workerThreadCnt, bool tcpNagleOn, int maxUserCnt);

	void PostRecv(SESSION* pSession);
	void PostSend(SESSION* pSession);

	virtual bool OnConnectionRequest(char* pClientIP, short port) = 0;
	virtual void OnRecv(SESSION_UID sessionUID, const char* pPacket, int size) = 0;
	virtual void OnClientJoin(SESSION_UID sessionUID) = 0;

	void Send(long long sessionUID, char* pPacket, int size);

private:
	void WorkerThread();
	void AcceptThread();
	void SendThread();

private:
	void AfterRecvProcess(SESSION* pSession, DWORD transferredBytes);
	void AfterSendProcess(SESSION* pSession);

	SESSION* AllocateSession();
	bool	 DeallocateSession(SESSION* pSession);

	SESSION* GetSession(SESSION_UID sessionUID);
	void	 ReleaseSession(SESSION* pSession);

	void PrintError(int errorcode, int line);

private:
	SOCKET					 m_listenSocket;
	HANDLE					 m_hIocp;
	std::vector<std::thread> m_vecWorkerThread;
	std::vector<std::thread> m_vecSendThread;
	std::thread				 m_AcceptThread;
	std::atomic<int>		 m_iAtomicCurrentClientCnt;
	std::atomic<SESSION_UID> m_llAtomicSessionUID;
	int						 m_MaxClientCnt;

	Concurrency::concurrent_unordered_map<SESSION_UID, SESSION*> m_unmapActiveSession;
	Concurrency::concurrent_unordered_map<SESSION_UID, SESSION*> m_ummapAcceptPending;
	Concurrency::concurrent_unordered_map<SESSION_UID, SESSION*> m_ummapReleasePending;

	MemoryPool<SESSION>* m_pSessionPool;
	MemoryPool<MESSAGE>* m_pMessagePool;
};
