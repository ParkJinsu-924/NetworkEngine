#include "NetServer.h"
#include "NetUtil.h"
#include "ThreadLocalMemoryPool.h"

NetServer::NetServer()
: m_iAtomicCurrentClientCnt(0)
, m_iAtomicSessionUID(0)
, m_MessagePool(3000)
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

	if (bind(m_listenSocket, (SOCKADDR*)&addr, sizeof(addr)) == SOCKET_ERROR)
		return false;

	bool bNagleOpt = tcpNagleOn;
	if (setsockopt(m_listenSocket, IPPROTO_TCP, TCP_NODELAY, (const char*)&bNagleOpt, sizeof(bNagleOpt)) == SOCKET_ERROR)
		return false;

	m_hIocp = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, NULL, workerThreadCnt);
	if (m_hIocp == NULL)
		return false;

	// init session array
	m_SessionArray = new (std::nothrow) SESSION[maxUserCnt];
	if (m_SessionArray == nullptr)
		return false;

	m_MaxClientCnt = maxUserCnt;

	for (int sessionIndex = 0; sessionIndex < maxUserCnt; ++sessionIndex)
	{
		m_queueSessionIndexArray.push(sessionIndex);
	}

	if (listen(m_listenSocket, SOMAXCONN) == SOCKET_ERROR)
		return false;

	// create thread
	for (int i = 0; i < workerThreadCnt; ++i)
	{
		m_vecWorkerThread.push_back(std::thread([this]()
												{ WorkerThread(); }));
	}

	m_vecSendThread.push_back(std::thread([this]()
										  { SendThread(); }));

	m_AcceptThread = std::thread([this]()
								 { AcceptThread(); });

	return true;
}

void NetServer::PostRecv(SESSION* pSession)
{
	if (pSession == nullptr)
		return;

	RingBuffer& recvQ = pSession->recvQ;

	int freeSize = (int)recvQ.free_space();
	int directEnqueueSize = (int)recvQ.direct_enqueue_size();

	int	   bufCount = 1;
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

	PreventRelease(pSession);

	DWORD flags = 0;
	int	  result = WSARecv(pSession->sessionSocket, recvBuf, bufCount, nullptr, &flags, &pSession->recvOverlapped, nullptr);
	if (result == SOCKET_ERROR && WSAGetLastError() != WSA_IO_PENDING)
	{
		NetUtil::PrintError(WSAGetLastError(), __LINE__);
		UnlockPrevent(pSession);
	}
}

void NetServer::PostSend(SESSION* pSession)
{
	if (pSession == nullptr)
		return;

	auto& sendPendingQ = pSession->sendPendingQ;
	if (!sendPendingQ.empty())
		return;

	auto&  sendQ = pSession->sendQ;
	WSABUF sendBuf[MAX_WSABUF_SIZE];

	int		 wsaBufIdx = 0;
	MESSAGE* pMessage = nullptr;
	while (sendQ.try_pop(pMessage))
	{
		if (pMessage == nullptr)
			continue;

		sendBuf[wsaBufIdx].buf = (char*)pMessage;
		sendBuf[wsaBufIdx].len = sizeof(pMessage->header) + pMessage->header.length;

		++wsaBufIdx;

		sendPendingQ.push(pMessage);

		if (wsaBufIdx >= MAX_WSABUF_SIZE)
			break;
	}

	pSession->ResetSendOverlapped();

	PreventRelease(pSession);

	DWORD flags = 0;
	int	  result = WSASend(pSession->sessionSocket, sendBuf, wsaBufIdx, nullptr, flags, &pSession->sendOverlapped, nullptr);
	if (result == SOCKET_ERROR && WSAGetLastError() != WSA_IO_PENDING)
	{
		NetUtil::PrintError(WSAGetLastError(), __LINE__);
		UnlockPrevent(pSession);
	}
}

bool NetServer::Send(long long sessionUID, MESSAGE* pMessage)
{
	if (pMessage == nullptr)
		return false;

	SESSION* pSession = GetSession(sessionUID);
	if (pSession == nullptr)
	{
		FreeMessage(pMessage);
		return false;
	}

	std::lock_guard<std::mutex> lock(pSession->lock);

	if (pSession->sessionUID != sessionUID || pSession->IsReleased())
	{
		FreeMessage(pMessage);
		return false;
	}

	pSession->sendQ.push(pMessage);

	return true;
}

bool NetServer::Disconnect(SESSION_UID sessionUID)
{
	SESSION* pSession = GetSession(sessionUID);
	if (pSession == nullptr)
		return false;

	std::lock_guard<std::mutex> lock(pSession->lock);

	if (pSession->IsReleased())
		return false;

	if (pSession->sessionUID != sessionUID)
		return false;

	shutdown(pSession->sessionSocket, SD_BOTH);

	return true;
}

MESSAGE* NetServer::AllocateMessage()
{
	MESSAGE* pMessage = m_MessagePool.Allocate();
	if (pMessage == nullptr)
		return nullptr;

	pMessage->Reset();

	return pMessage;
}

bool NetServer::FreeMessage(MESSAGE* pMessage)
{
	if (pMessage == nullptr)
		return false;

	m_MessagePool.Free(pMessage);
	return true;
}

void NetServer::WorkerThread()
{
	while (true)
	{
		SESSION*	pSession = nullptr;
		OVERLAPPED* pOverlapped = nullptr;
		DWORD		transferredBytes = 0;

		// available error : ERROR_OPERATION_ABORTED, ERROR_ABANDONED_WAIT_0, WAIT_TIMEOUT
		GetQueuedCompletionStatus(m_hIocp, &transferredBytes, (PULONG_PTR)&pSession, &pOverlapped, INFINITE);
		if (pOverlapped == nullptr)
		{
			PostQueuedCompletionStatus(m_hIocp, NULL, NULL, NULL);
			break;
		}

		if (transferredBytes == 0 || pOverlapped->Internal == ERROR_OPERATION_ABORTED)
		{
			NetUtil::PrintError(WSAGetLastError(), __LINE__);
			UnlockPrevent(pSession);
			continue;
		}

		if (&pSession->recvOverlapped == pOverlapped) // recv complete
		{
			AfterRecvProcess(pSession, transferredBytes);
		}
		else if (&pSession->sendOverlapped == pOverlapped) // send complete
		{
			AfterSendProcess(pSession);
		}

		UnlockPrevent(pSession);
	}
}

void NetServer::SendThread()
{
	while (true)
	{
		for (int idx = 0; idx < m_MaxClientCnt; ++idx)
		{
			SESSION* pSession = &m_SessionArray[idx];
			if (pSession == nullptr)
				continue;

			if (pSession->sessionSocket == 0)
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

		if (m_iAtomicCurrentClientCnt >= m_MaxClientCnt)
		{
			closesocket(acceptSocket);
			continue;
		}

		char clientIP[46];
		InetNtopA(AF_INET, (const void*)&addr.sin_addr.s_addr, clientIP, sizeof(clientIP));
		if (!OnConnectionRequest(clientIP, addr.sin_port))
		{
			--m_iAtomicCurrentClientCnt;
			closesocket(acceptSocket);
			continue;
		}

		int sessionIdx;
		if (!m_queueSessionIndexArray.try_pop(sessionIdx))
		{
			closesocket(acceptSocket);
			continue;
		}

		SESSION* pSession = &m_SessionArray[sessionIdx];
		if (pSession == nullptr)
		{
			closesocket(acceptSocket);
			continue;
		}

		if (CreateIoCompletionPort((HANDLE)acceptSocket, m_hIocp, (ULONG_PTR)pSession, NULL) == NULL)
		{
			std::cout << "fail to attach socket in iocp, errno : " << WSAGetLastError() << std::endl;
			closesocket(acceptSocket);
			continue;
		}

		++m_iAtomicCurrentClientCnt;

		pSession->sessionSocket = acceptSocket;
		pSession->sessionUID = NetUtil::MakeSessionUID(sessionIdx, ++m_iAtomicSessionUID);
		pSession->SetReleaseState(false);

		PreventRelease(pSession);

		OnClientJoin(pSession->sessionUID);

		PostRecv(pSession);

		UnlockPrevent(pSession);
	}
}

void NetServer::AfterRecvProcess(SESSION* pSession, DWORD transferredBytes)
{
	if (pSession == nullptr)
		return;

	RingBuffer& recvQ = pSession->recvQ;
	recvQ.move_head(transferredBytes);

	while (true)
	{
		HEADER header;

		const size_t headerSize = sizeof(header);
		const size_t useSize = recvQ.size_in_use();
		if (useSize <= sizeof(header))
			break;

		if (recvQ.peek((char*)&header, headerSize) == false)
			return;

		if (header.length >= RINGBUFFER_SIZE - headerSize)
			return;

		if (static_cast<short>(useSize - headerSize) < header.length)
			break;

		MESSAGE* pMessage = AllocateMessage();
		if (pMessage == nullptr)
			return;

		recvQ.peek((char*)pMessage, headerSize + header.length);
		recvQ.move_tail(headerSize + header.length);

		OnRecv(pSession->sessionUID, pMessage);
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

		FreeMessage(pMessage);
	}
}

SESSION* NetServer::GetSession(SESSION_UID sessionUID)
{
	int sessionIdx = NetUtil::GetSessionIndexPart(sessionUID);
	if (sessionIdx >= m_MaxClientCnt)
		return nullptr;

	if (m_SessionArray[sessionIdx].sessionUID == sessionUID)
		return &m_SessionArray[sessionIdx];

	return nullptr;
}

void NetServer::ReleaseSession(SESSION* pSession)
{
	if (pSession == nullptr)
		return;

	std::lock_guard<std::mutex> lock(pSession->lock);

	if (pSession->IsReleased())
		return;

	pSession->SetReleaseState(true);

	closesocket(pSession->sessionSocket);

	OnClientLeave(pSession->sessionUID);

	MESSAGE* pMessage = nullptr;
	while (pSession->sendQ.try_pop(pMessage))
	{
		if (pMessage == nullptr)
			continue;

		FreeMessage(pMessage);
	}

	while (pSession->sendPendingQ.try_pop(pMessage))
	{
		if (pMessage == nullptr)
			continue;

		FreeMessage(pMessage);
	}

	int sessionIndex = NetUtil::GetSessionIndexPart(pSession->sessionUID);

	pSession->Reset();

	m_queueSessionIndexArray.push(sessionIndex);

	--m_iAtomicCurrentClientCnt;
}

bool NetServer::PreventRelease(SESSION* pSession)
{
	if (pSession == nullptr)
		return false;

	if (pSession->IsReleased())
		return false;

	++pSession->ioCount;
	return true;
}

bool NetServer::UnlockPrevent(SESSION* pSession)
{
	if (pSession == nullptr)
		return false;

	if (pSession->IsReleased())
		return false;

	if (--pSession->ioCount == 0)
	{
		ReleaseSession(pSession);
	}
	return true;
}

class TestServer : public NetServer
{
	bool OnConnectionRequest(char* pClientIP, short port)
	{
		return true;
	}

	void OnClientJoin(SESSION_UID sessionUID)
	{
		std::cout << "Client come : " << sessionUID << std::endl;
	}

	void OnRecv(SESSION_UID sessionUID, MESSAGE* pMessage)
	{
		Send(sessionUID, pMessage);
	}

	void OnClientLeave(SESSION_UID sessionUID)
	{
	}
};

TestServer server;

int main()
{
	server.Start("0.0.0.0", 27931, 5, false, 400);
	Sleep(INFINITE);
}
