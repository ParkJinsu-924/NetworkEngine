﻿#include "NetServer.h"

NetServer::NetServer()
: m_iAtomicCurrentClientCnt(0)
, m_llAtomicSessionUID(0)
{
}

bool NetServer::Start(const char* ip, short port, int workerThreadCnt, bool tcpNagleOn, int maxUserCnt)
{
	int iReturnValue = 0;

	WSADATA wsaData;
	iReturnValue = WSAStartup(MAKEWORD(2, 2), &wsaData);
	if (iReturnValue != 0)
		return false;

	m_listenSocket = socket(AF_INET, SOCK_STREAM, NULL);
	if (m_listenSocket == INVALID_SOCKET)
		return false;

	SOCKADDR_IN addr;
	addr.sin_family = AF_INET;
	InetPtonA(AF_INET, ip, &addr.sin_addr);
	addr.sin_port = htons(port);

	iReturnValue = bind(m_listenSocket, (SOCKADDR*)&addr, sizeof(addr));
	if (iReturnValue == SOCKET_ERROR)
		return false;

	bool bNagleOpt = tcpNagleOn;
	iReturnValue = setsockopt(m_listenSocket, IPPROTO_TCP, TCP_NODELAY, (const char*)&bNagleOpt, sizeof(bNagleOpt));
	if (iReturnValue == SOCKET_ERROR)
		return false;

	m_hIocp = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, NULL, workerThreadCnt);
	if (m_hIocp == NULL)
		return false;

	//init pool
	m_pSessionPool = new (std::nothrow) MemoryPool<SESSION>(maxUserCnt);
	if (m_pSessionPool == nullptr)
		return false;

	m_iMaxClientCnt = maxUserCnt;

	if (listen(m_listenSocket, SOMAXCONN) == SOCKET_ERROR)
		return false;

	for (int i = 0; i < workerThreadCnt; ++i)
	{
		m_vecWorkerThread.push_back(std::thread([this]() {
			WorkerThread();
		}));
	}

	m_vecSendThread.push_back(std::thread([this]() {
		SendThread();
	}));

	m_AcceptThread = std::thread([this]() {
		AcceptThread();
	});
}

void NetServer::PostRecv(SESSION* pSession)
{
	if (pSession == nullptr)
		return;

	RingBuffer& recvQ = pSession->recvQ;

	int iFreeSize = recvQ.free_space();
	int iDirectEnqueueSize = recvQ.direct_enqueue_size();

	int	bufCount = 1;
	WSABUF recvBuf[2];
	recvBuf[0].buf = recvQ.head_pointer();
	recvBuf[0].len = iDirectEnqueueSize;
	if (iDirectEnqueueSize < iFreeSize)
	{
		recvBuf[1].buf = recvQ.start_pointer();
		recvBuf[1].len = iFreeSize - iDirectEnqueueSize;
		++bufCount;
	}

	pSession->ResetRecvOverlapped();

	DWORD flags = 0;
	int   result = WSARecv(pSession->sessionSocket, recvBuf, bufCount, nullptr, &flags, &pSession->recvOverlapped, nullptr);
	if (result == SOCKET_ERROR && WSAGetLastError() != WSA_IO_PENDING)
	{
		//need to release session or run iocount decrement logic here
	}
}

void NetServer::PostSend(SESSION* pSession)
{
	if (pSession == nullptr)
		return;

	std::lock_guard<std::mutex> lock(pSession->sendLock);

	RingBuffer& sendQ = pSession->sendQ;

	size_t useSize = sendQ.size_in_use();
	if (useSize == 0)
		return;

	size_t directDequeueSize = sendQ.direct_dequeue_size();

	int	bufCount = 1;
	WSABUF sendBuf[2];
	sendBuf[0].buf = sendQ.tail_pointer();
	sendBuf[0].len = directDequeueSize;
	if (useSize > directDequeueSize)
	{
		sendBuf[1].buf = sendQ.start_pointer();
		sendBuf[1].len = useSize - directDequeueSize;
		++bufCount;
	}

	DWORD flags = 0;
	pSession->ResetSendOverlapped();
	InterlockedExchange8((char*)&pSession->sendFlag, true);
	int result = WSASend(m_listenSocket, sendBuf, bufCount, nullptr, flags, &pSession->sendOverlapped, nullptr);
	if (result == SOCKET_ERROR && WSAGetLastError() != WSA_IO_PENDING)
	{
		//release session
	}
}

void NetServer::Send(long long sessionUID, char* pPacket, int size)
{
	if (pPacket == nullptr)
		return;

	SESSION* pSession = GetSession(sessionUID);
	if (pSession == nullptr)
		return;

	if (pSession->sendQ.put(pPacket, size))
	{
		//Release Session
	}

	PostSend(pSession);
}

void NetServer::WorkerThread()
{
	while (true)
	{
		SESSION*	pSession = nullptr;
		OVERLAPPED* pOverlapped = nullptr;
		DWORD		transferredBytes = 0;

		//available error : ERROR_OPERATION_ABORTED, ERROR_ABANDONED_WAIT_0, WAIT_TIMEOUT
		BOOL success = GetQueuedCompletionStatus(m_hIocp, &transferredBytes, (PULONG_PTR)&pSession, &pOverlapped, INFINITE);
		if (!success || pOverlapped == nullptr)
		{
			PostQueuedCompletionStatus(m_hIocp, NULL, NULL, NULL);
			break;
		}

		if (transferredBytes == 0 || pOverlapped->Internal == ERROR_OPERATION_ABORTED)
		{
			//ReleaseSession, need IOCOUNT
		}

		if (&pSession->recvOverlapped == pOverlapped) //recv complete
		{
			AfterRecvProcess(pSession, transferredBytes);
		}
		else if (&pSession->sendOverlapped == pOverlapped) //send complete
		{
			AfterSendProcess(pSession);
		}
	}
}

void NetServer::SendThread()
{
	while (true)
	{
		for (auto sessionPair : m_unmapActiveSession)
		{
			SESSION* pSession = sessionPair.second;
			if (pSession->sendFlag == true)
				continue;

			if (pSession->sendQ.empty())
				continue;

			PostSend(pSession);
		}
	}
}

void NetServer::AcceptThread()
{
	while (true)
	{
		SOCKADDR_IN addr;
		int			size = sizeof(addr);
		SOCKET		acceptSocket = accept(m_listenSocket, (SOCKADDR*)&addr, &size);
		if (acceptSocket == INVALID_SOCKET)
		{
			switch (WSAGetLastError())
			{
				case WSAECONNRESET:
					continue;
				case WSAENOBUFS:
					continue;
				default:
					continue;
			}
		}

		if (m_iAtomicCurrentClientCnt >= m_iMaxClientCnt)
		{
			closesocket(acceptSocket);
			continue;
		}

		char clientIP[46];
		InetNtopA(AF_INET, (const void*)&addr.sin_addr.s_addr, clientIP, sizeof(clientIP));
		if (!OnConnectionRequest(clientIP, addr.sin_port))
		{
			closesocket(acceptSocket);
			continue;
		}

		SESSION* pSession = AllocateSession();
		if (pSession == nullptr)
		{
			closesocket(acceptSocket);
			continue;
		}

		if (CreateIoCompletionPort((HANDLE)acceptSocket, m_hIocp, (ULONG_PTR)pSession, NULL) == NULL)
		{
			std::cout << "fail to attach socket in iocp, errno : " << WSAGetLastError() << std::endl;
			closesocket(acceptSocket);
			DeallocateSession(pSession);
			continue;
		}

		pSession->sessionSocket = acceptSocket;
		pSession->sessionUID = m_llAtomicSessionUID++;

		m_unmapActiveSession.insert(std::make_pair(pSession->sessionUID, pSession));

		OnClientJoin(pSession->sessionUID);
	}
}

void NetServer::AfterRecvProcess(SESSION* pSession, DWORD transferredBytes)
{
	if (pSession == nullptr)
		return;

	while (true)
	{
		HEADER		header;
		RingBuffer& recvQ = pSession->recvQ;

		const size_t headerSize = sizeof(header);
		const size_t useSize = recvQ.size_in_use();
		if (useSize <= sizeof(header))
			break;

		if (recvQ.peek((char*)&header, headerSize) == false)
		{
			//release session
		}

		if (header.length >= RINGBUFFER_SIZE - headerSize)
		{
			//release session
		}

		if ((short)(useSize - headerSize) < header.length)
			break;

		char* pBuffer = new char[header.length]; // MemoryPoolTLS::Alloc(header.length)
		recvQ.move_tail(headerSize);
		recvQ.peek((char*)pBuffer, header.length);
		recvQ.move_tail(header.length);

		OnRecv(pSession->sessionUID, pBuffer);

		delete[] pBuffer;
	}

	PostRecv(pSession);
}

void NetServer::AfterSendProcess(SESSION* pSession)
{
	if (pSession == nullptr)
		return;

	if (pSession->sendQ.size_in_use() > 0)
		PostSend(pSession);
}

SESSION* NetServer::AllocateSession()
{
	if (m_pSessionPool == nullptr)
		return nullptr;

	m_pSessionPool->Allocate();
}

bool NetServer::DeallocateSession(SESSION* pSession)
{
	if (m_pSessionPool == nullptr || pSession == nullptr)
		return false;

	pSession->Reset();
	m_pSessionPool->Deallocate(pSession);

	return true;
}

SESSION* NetServer::GetSession(SESSION_UID sessionUID)
{
	auto it = m_unmapActiveSession.find(sessionUID);
	if (it == m_unmapActiveSession.end())
		return nullptr;

	return it->second;
}

int main()
{
}
