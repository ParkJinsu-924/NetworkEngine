#include "NetClient.h"
#include "GlobalValue.h"

#define PRINT_ERROR() PrintError(WSAGetLastError(), __LINE__);

NetClient::NetClient()
: m_MessagePool(3000)
{
}

bool NetClient::Connect(const char* ip, short port, bool tcpNagleOn)
{
	if (ip == nullptr)
		return false;

	WSADATA wsa;
	if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
		return false;

	const SOCKET clientSocket = socket(AF_INET, SOCK_STREAM, 0);
	if (clientSocket == INVALID_SOCKET)
		return false;

	GetSession().Reset();
	GetSession().sessionSocket = clientSocket;

	bool isSocketSetSuccess = true;

	BOOL flag = true;
	if (setsockopt(clientSocket, SOL_SOCKET, SO_KEEPALIVE, (char*)&flag, sizeof(flag)) == SOCKET_ERROR)
		isSocketSetSuccess = false;

	DWORD optval = true;
	if (setsockopt(clientSocket, IPPROTO_TCP, TCP_NODELAY, (char*)&optval, sizeof(optval)) == SOCKET_ERROR)
		isSocketSetSuccess = false;

	linger lingerOpt;
	lingerOpt.l_onoff = 1;
	lingerOpt.l_linger = 0;
	if (setsockopt(clientSocket, SOL_SOCKET, SO_LINGER, (char*)&lingerOpt, sizeof(lingerOpt)) == SOCKET_ERROR)
		isSocketSetSuccess = false;

	if (isSocketSetSuccess == false)
	{
		closesocket(clientSocket);
		return false;
	}

	constexpr int WORKER_THREAD_CNT = 4;
	m_hIocp = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, NULL, WORKER_THREAD_CNT);
	if (m_hIocp == NULL)
	{
		closesocket(clientSocket);
		return false;
	}

	// create thread
	for (int i = 0; i < WORKER_THREAD_CNT; ++i)
	{
		m_vecWorkerThread.push_back(std::thread([this]() { WorkerThread(); }));
	}
	m_SendThread = std::thread([this]() { SendThread(); });

	// connect
	SOCKADDR_IN addr;
	addr.sin_family = AF_INET;
	InetPtonA(AF_INET, ip, &addr.sin_addr);
	addr.sin_port = htons(port);
	if (connect(clientSocket, (SOCKADDR*)&addr, sizeof(addr)) == SOCKET_ERROR)
	{
		closesocket(clientSocket);
		return false;
	}

	if (CreateIoCompletionPort((HANDLE)clientSocket, m_hIocp, NULL, NULL) == NULL)
	{
		closesocket(clientSocket);
		return false;
	}

	GetSession().SetReleaseState(false);

	PostRecv();

	return true;
}

bool NetClient::Send(MESSAGE* pMessage)
{
	if (pMessage == nullptr)
		return false;

	std::lock_guard<std::mutex> lock(GetSession().lock);
	if (GetSession().IsReleased())
	{
		FreeMessage(pMessage);
		return false;
	}

	GetSession().sendQ.push(pMessage);
	return true;
}

bool NetClient::Disconnect()
{
	std::lock_guard<std::mutex> lock(GetSession().lock);

	if (GetSession().IsReleased())
		return false;

	shutdown(GetSession().sessionSocket, SD_BOTH);

	return true;
}

MESSAGE* NetClient::AllocateMessage()
{
	MESSAGE* pMessage = m_MessagePool.Allocate();
	if (pMessage == nullptr)
		return nullptr;

	pMessage->Reset();

	return pMessage;
}

bool NetClient::FreeMessage(MESSAGE* pMessage)
{
	if (pMessage == nullptr)
		return false;

	m_MessagePool.Free(pMessage);
	return true;
}

void NetClient::WorkerThread()
{
	while (true)
	{
		OVERLAPPED* pOverlapped = nullptr;
		DWORD		transferredBytes = 0;
		SESSION*	pSession = nullptr;

		GetQueuedCompletionStatus(m_hIocp, &transferredBytes, (PULONG_PTR)&pSession, &pOverlapped, INFINITE);
		if (pOverlapped == nullptr)
		{
			PostQueuedCompletionStatus(m_hIocp, NULL, NULL, NULL);
			break;
		}

		// error_operation_aborted for CancelIo
		if (transferredBytes == 0 || pOverlapped->Internal == ERROR_OPERATION_ABORTED)
		{
			UnlockPrevent();
			continue;
		}

		if (&GetSession().recvOverlapped == pOverlapped)
		{
			AfterRecvProcess(transferredBytes);
		}
		else if (&GetSession().sendOverlapped == pOverlapped)
		{
			AfterSendProcess();
		}

		UnlockPrevent();
	}
}

void NetClient::SendThread()
{
	while (true)
	{
		Sleep(1);

		PostSend();
	}
}

void NetClient::PostRecv()
{
	RingBuffer& recvQ = GetSession().recvQ;

	int freeSize = (int)recvQ.free_space();
	int directEnqueueSize = (int)recvQ.direct_enqueue_size();

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

	GetSession().ResetRecvOverlapped();
	PreventRelease();

	DWORD flags = 0;
	int   result = WSARecv(GetSession().sessionSocket, recvBuf, bufCount, nullptr, &flags, &GetSession().recvOverlapped, nullptr);
	if (result == SOCKET_ERROR && WSAGetLastError() != WSA_IO_PENDING)
	{
		PRINT_ERROR();
		UnlockPrevent();
	}
}

void NetClient::PostSend()
{
	// when sendPendingQ is not empty, send Process is not over
	if (!GetSession().sendPendingQ.empty() || GetSession().sendQ.empty())
		return;

	WSABUF sendBuf[MAX_WSABUF_SIZE];

	int		 wsaBufIdx = 0;
	MESSAGE* pMessage = nullptr;
	while (GetSession().sendQ.try_pop(pMessage))
	{
		if (pMessage == nullptr)
		{
			// Call Release Session
			return;
		}

		sendBuf[wsaBufIdx].buf = (char*)pMessage;
		sendBuf[wsaBufIdx].len = sizeof(pMessage->header) + pMessage->header.length;

		++wsaBufIdx;

		GetSession().sendPendingQ.push(pMessage);

		if (wsaBufIdx >= MAX_WSABUF_SIZE)
			break;
	}

	GetSession().ResetSendOverlapped();
	PreventRelease();

	DWORD flags = 0;
	int   result = WSASend(GetSession().sessionSocket, sendBuf, wsaBufIdx, nullptr, flags, &GetSession().sendOverlapped, nullptr);
	if (result == SOCKET_ERROR && WSAGetLastError() != WSA_IO_PENDING)
	{
		PRINT_ERROR();
		UnlockPrevent();
	}
}

void NetClient::AfterRecvProcess(DWORD transferredBytes)
{
	RingBuffer& recvQ = GetSession().recvQ;
	recvQ.move_head(transferredBytes);

	while (true)
	{
		HEADER header;

		const size_t headerSize = sizeof(header);
		const size_t useSize = recvQ.size_in_use();
		if (useSize <= sizeof(header))
			break;

		if (recvQ.peek((char*)&header, headerSize) == false)
		{
			PRINT_ERROR();
			break;
		}

		if (header.length >= RINGBUFFER_SIZE - headerSize)
		{
			PRINT_ERROR();
			break;
		}

		if ((short)(useSize - headerSize) < header.length)
			break;

		MESSAGE* pMessage = AllocateMessage();
		if (pMessage == nullptr)
			return;

		recvQ.peek((char*)pMessage, headerSize + header.length);
		recvQ.move_tail(headerSize + header.length);

		OnRecv(pMessage);
	}

	PostRecv();
}

void NetClient::AfterSendProcess()
{
	MESSAGE* pMessage = nullptr;
	while (GetSession().sendPendingQ.try_pop(pMessage))
	{
		if (pMessage == nullptr)
			continue;

		FreeMessage(pMessage);
	}
}

void NetClient::ReleaseSession()
{
	std::lock_guard<std::mutex> lock(GetSession().lock);

	if (GetSession().IsReleased())
		return;

	GetSession().SetReleaseState(true);

	closesocket(GetSession().sessionSocket);

	MESSAGE* pMessage = nullptr;
	while (GetSession().sendQ.try_pop(pMessage))
	{
		if (pMessage == nullptr)
			continue;

		FreeMessage(pMessage);
	}

	while (GetSession().sendPendingQ.try_pop(pMessage))
	{
		if (pMessage == nullptr)
			continue;

		FreeMessage(pMessage);
	}

	OnDisconnect();
}

bool NetClient::PreventRelease()
{
	if (GetSession().IsReleased())
		return false;

	++GetSession().ioCount;

	return true;
}

bool NetClient::UnlockPrevent()
{
	if (GetSession().IsReleased())
		return true;

	if (--GetSession().ioCount == 0)
	{
		ReleaseSession();
	}
	return true;
}

void NetClient::PrintError(int errorcode, int line)
{
	std::cout << "ERROR : Need to release : ErrorCode : " << errorcode << " : LINE : " << line << std::endl;
}

class TestClient : public NetClient
{
	void OnRecv(MESSAGE* pMessage)
	{
		std::cout << pMessage->GetPayload() << std::endl;
		FreeMessage(pMessage);
	}

	void OnDisconnect()
	{
		std::cout << "Disconnected!!!!!!!!!!!" << std::endl;
	}
};

TestClient nl;

void SendingThread()
{
	const char* payload = "abcdefghijkmlnopqrstuvwxyz1234567890";
	while (true)
	{
		MESSAGE* pMessage = nl.AllocateMessage();
		pMessage->put(payload, rand() % 37);
		pMessage->put("\0", 1);

		if (!nl.Send(pMessage))
		{
			std::cout << "Can't Send !" << std::endl;
		}
		Sleep(800);
	}
}

std::chrono::steady_clock::time_point startTime;

void DisconnectThread()
{
	while (true)
	{
		auto currentTime = std::chrono::high_resolution_clock::now();
		auto elapsedTime = std::chrono::duration_cast<std::chrono::seconds>(currentTime - startTime).count();
		
		if (elapsedTime >= 10)
		{
			nl.Disconnect();
			break;
		}
	}
}

int main()
{
	startTime = std::chrono::high_resolution_clock::now();
	bool f = nl.Connect("127.0.0.1", 27931, false);
	if (f == false)
	{
		std::cout << "sdfsdf" << std::endl;
	}

	std::thread th1([]() { SendingThread(); });

	//std::thread th5([]() { DisconnectThread(); });

	Sleep(INFINITE);
}
