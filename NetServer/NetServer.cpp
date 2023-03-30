#include "NetServer.h"

constexpr int TOTAL_MESSAGE_COUNT = 5000;

NetServer::NetServer()
: m_iAtomicCurrentClientCnt(0)
, m_llAtomicSessionUID(0)
{
}

bool NetServer::Start(const char* ip, short port, int workerThreadCnt, bool tcpNagleOn, int maxUserCnt)
{
	WSADATA wsaData;
	if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0)
		return false;

	m_listenSocket = socket(AF_INET, SOCK_STREAM, NULL);
	if (m_listenSocket == INVALID_SOCKET)
		return false;

	SOCKADDR_IN addr;
	addr.sin_family = AF_INET;
	InetPtonA(AF_INET, ip, &addr.sin_addr);
	addr.sin_port = htons(port);

	if(bind(m_listenSocket, (SOCKADDR*)&addr, sizeof(addr)) == SOCKET_ERROR)
		return false;

	bool bNagleOpt = tcpNagleOn;
	if (setsockopt(m_listenSocket, IPPROTO_TCP, TCP_NODELAY, (const char*)&bNagleOpt, sizeof(bNagleOpt)) == SOCKET_ERROR)
		return false;

	m_hIocp = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, NULL, workerThreadCnt);
	if (m_hIocp == NULL)
		return false;

	//init pool
	m_pSessionPool = new (std::nothrow) MemoryPool<SESSION>(maxUserCnt);
	m_pMessagePool = new (std::nothrow) MemoryPool<MESSAGE>(TOTAL_MESSAGE_COUNT);
	if (m_pSessionPool == nullptr || m_pMessagePool == nullptr)
		return false;

	m_MaxClientCnt = maxUserCnt;

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

	return true;
}

void NetServer::PostRecv(SESSION* pSession)
{
	if (pSession == nullptr)
		return;

	RingBuffer& recvQ = pSession->recvQ;

	int freeSize = recvQ.free_space();
	int directEnqueueSize = recvQ.direct_enqueue_size();

	int	bufCount = 1;
	WSABUF recvBuf[2];
	recvBuf[0].buf = recvQ.head_pointer();
	recvBuf[0].len = directEnqueueSize;
	if (directEnqueueSize < freeSize)
	{
		recvBuf[1].buf = recvQ.start_pointer();
		recvBuf[1].len = freeSize - directEnqueueSize;
		++bufCount;
	}

	pSession->ResetRecvOverlapped();

	DWORD flags = 0;
	int   result = WSARecv(pSession->sessionSocket, recvBuf, bufCount, nullptr, &flags, &pSession->recvOverlapped, nullptr);
	if (result == SOCKET_ERROR && WSAGetLastError() != WSA_IO_PENDING)
	{
		PrintError(WSAGetLastError(), __LINE__);
		//need to release session or run iocount decrement logic here
	}
}

void NetServer::PostSend(SESSION* pSession)
{
	if (pSession == nullptr)
		return;

	auto& sessionSendPendingMessageQ = pSession->sendPendingQ;
	if (!sessionSendPendingMessageQ.empty())
		return;

	constexpr int MAX_HOLD_MESSAGE = 128;
	auto&		  sessionSendMessageQ = pSession->sendQ;
	if (sessionSendMessageQ.unsafe_size() > MAX_HOLD_MESSAGE)
	{
		//Call Release Session
		return;
	}

	WSABUF sendBuf[MAX_HOLD_MESSAGE];

	int		 wsaBufIdx = 0;
	MESSAGE* pMessage = nullptr;
	while (sessionSendMessageQ.try_pop(pMessage))
	{
		if (pMessage == nullptr)
		{
			//Call Release Session
			return;
		}

		sendBuf[wsaBufIdx].buf = (char*)&pMessage->header;
		sendBuf[wsaBufIdx].len = sizeof(pMessage->header) + pMessage->header.length;

		++wsaBufIdx;

		sessionSendPendingMessageQ.push(pMessage);
	}

	--wsaBufIdx;

	DWORD flags = 0;
	pSession->ResetSendOverlapped();
	int result = WSASend(pSession->sessionSocket, sendBuf, wsaBufIdx, nullptr, flags, &pSession->sendOverlapped, nullptr);
	if (result == SOCKET_ERROR && WSAGetLastError() != WSA_IO_PENDING)
	{
		PrintError(WSAGetLastError(), __LINE__);
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

	MESSAGE* pMessage = m_pMessagePool->Allocate();
	if (pMessage == nullptr)
		return;

	pMessage->header.length = size;
	memcpy(pMessage->payload, pPacket, size);

	pSession->sendQ.push(pMessage);
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
			PrintError(WSAGetLastError(), __LINE__);
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
			if (pSession == nullptr)
				continue;

			if (pSession->sendQ.empty())
				continue;

			PostSend(pSession);
		}

		//Send Loop 돌고난 이후, ReleasePending 을 돌면서 Release해줄 애들은 Release
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

		if (m_iAtomicCurrentClientCnt >= m_MaxClientCnt)
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
			PrintError(WSAGetLastError(), __LINE__);
			//release session
		}

		if (header.length >= RINGBUFFER_SIZE - headerSize)
		{
			PrintError(WSAGetLastError(), __LINE__);
			//release session
		}

		if ((short)(useSize - headerSize) < header.length)
			break;

		//need to change
		/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
		char* pBuffer = new char[header.length]; // MemoryPoolTLS::Alloc(header.length)
		recvQ.move_tail(headerSize);
		recvQ.peek((char*)pBuffer, header.length);
		recvQ.move_tail(header.length);

		OnRecv(pSession->sessionUID, pBuffer, header.length);

		delete[] pBuffer;
		/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
	}

	PostRecv(pSession);
}

void NetServer::AfterSendProcess(SESSION* pSession)
{
	if (pSession == nullptr)
		return;

	auto& sessionSendPeningMessageQ = pSession->sendPendingQ;

	MESSAGE* pMessage = nullptr;
	while (sessionSendPeningMessageQ.try_pop(pMessage))
	{
		if (pMessage == nullptr)
			continue;

		m_pMessagePool->Deallocate(pMessage);
	}
}

SESSION* NetServer::AllocateSession()
{
	if (m_pSessionPool == nullptr)
		return nullptr;

	return m_pSessionPool->Allocate();
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

void NetServer::PrintError(int errorcode, int line)
{
	std::cout << "ERROR : Need to release : ErrorCode : " << errorcode << " : LINE : " << line << std::endl;
}
