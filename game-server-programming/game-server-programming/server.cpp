#include "server.h"

// 전역 변수로 클라이언트와 방의 정보를 저장하는 벡터와 동기화를 위한 뮤텍스 선언
vector<Client*> sockVector; // 연결된 클라이언트 소켓을 관리하는 벡터
vector<Room*> roomVec; // 채팅 방(Room) 객체를 관리하는 벡터
mutex d_lock; // 동기화를 위한 뮤텍스

// Server 클래스의 Start 메서드 정의
void Server::Start(unsigned short port_num, unsigned int thread_pool_size)
{
    assert(thread_pool_size > 0); // 스레드 풀 크기가 0보다 큰지 확인

    // 기본 방을 생성하고 벡터에 추가
    Room* room = new Room("AnterRoom");
    roomVec.push_back(room);

    // Acceptor 객체를 생성하여 서버 소켓을 설정
    acc.reset(new Acceptor(m_ios, port_num));
    acc->Start(); // 수신 대기 시작

    // 지정된 크기만큼 스레드 풀 생성 및 실행
    for (int i = 0; i < thread_pool_size; i++)
    {
        m_thread_pool.create_thread(bind(&Server::Run, this));
    }
}

// Server 클래스의 Run 메서드 정의
void Server::Run()
{
    PrintTid("ThreadStart!"); // 스레드 시작 메시지 출력
    m_ios.run(); // 비동기 작업을 수행하는 io_service 실행
    PrintTid("ThreadFinish!"); // 스레드 종료 메시지 출력
}

// Server 클래스의 Stop 메서드 정의
void Server::Stop()
{
    acc->Stop(); // 수신 대기를 중지
    m_ios.stop(); // io_service 중지
    m_thread_pool.join_all(); // 모든 스레드가 종료될 때까지 대기
}

// Acceptor 클래스의 생성자 정의
Acceptor::Acceptor(asio::io_service& ios, unsigned short port_num) :
    m_ios(ios), m_acc_strand(m_ios),
    m_acceptor(m_ios, asio::ip::tcp::endpoint(asio::ip::address_v4::any(), port_num)),
    m_isStopped(false)
{}

// Acceptor 클래스의 Start 메서드 정의
void Acceptor::Start()
{
    m_acceptor.listen(); // 소켓이 수신 대기 상태로 전환
    InitAccept(); // 새로운 연결 요청 대기 시작
}

// Acceptor 클래스의 InitAccept 메서드 정의
void Acceptor::InitAccept()
{
    sock.reset(new asio::ip::tcp::socket(m_ios)); // 새 소켓 객체 생성

    d_lock.lock(); // 멀티스레드 환경에서 안전하게 작업하기 위해 뮤텍스 잠금
    m_acceptor.async_accept(*sock, m_acc_strand.wrap(bind(&Acceptor::onAccept, this, _1, sock))); // 비동기 수신 시작
    d_lock.unlock(); // 뮤텍스 잠금 해제
}

// 비동기 수신이 완료되면 호출되는 콜백 함수
void Acceptor::onAccept(const boost::system::error_code& ec, shared_ptr<asio::ip::tcp::socket> sock)
{
    if (!ec)
    {
        Service* serv = new Service(sock, m_ios); // 새 서비스 객체 생성
        m_acc_strand.post(bind(&Service::StartHandling, serv)); // 비동기로 서비스 시작
    }
    else
    {
        // 오류가 발생했을 때 오류 메시지 출력
        std::cout << "Error occured! Error code = "
            << ec.value()
            << ". Message: " << ec.message()
            << endl;
    }

    if (!m_isStopped.load())
    {
        InitAccept(); // 서버가 중지되지 않은 경우 다시 수신 대기
    }
    else
    {
        m_acceptor.close(); // 서버가 중지된 경우 수신 소켓 닫기
    }
}

// Acceptor 클래스의 Stop 메서드 정의
void Acceptor::Stop()
{
	m_isStopped = true;
}

// Service 클래스의 생성자 정의
void Service::StartHandling()
{
	PrintTid("StartHandling"); // 현재 쓰레드 ID를 출력하고, "StartHandling"이라는 상태 메시지를 출력

	d_lock.lock(); // 뮤텍스(d_lock)를 잠금. 쓰레드 간 동기화 보장
	m_sock->read_some(asio::buffer(m_request)); // 소켓으로부터 데이터를 읽어와서 m_request 버퍼에 저장

	m_client = new Client(m_sock, m_request); // 새로운 Client 객체를 생성하고 m_sock과 m_request를 전달
	roomVec[0]->m_sockVector.push_back(m_client); // 첫 번째 방의 소켓 벡터에 새로운 클라이언트를 추가
	sockVector.push_back(m_client); // 전체 소켓 벡터에도 클라이언트를 추가
	cout << "Now Client Count : " << sockVector.size() << endl; // 현재 클라이언트 수를 출력
	d_lock.unlock(); // 뮤텍스 잠금 해제

	m_nickName = m_request; // 클라이언트의 요청을 닉네임으로 설정
	PrintTid("WaitReading"); // 현재 쓰레드 ID를 출력하고, "WaitReading" 상태 메시지를 출력
	memset(m_request, '\0', MAXBUF); // m_request 버퍼를 초기화

	d_lock.lock(); // 뮤텍스(d_lock)를 다시 잠금
	m_sock->async_read_some(asio::buffer(m_request, MAXBUF), m_io_strand.wrap(bind(&Service::onRequestReceived, this, _1, _2)));
	// 비동기적으로 데이터를 읽어와 m_request 버퍼에 저장하고, 읽기가 완료되면 onRequestReceived 함수를 호출하도록 설정
	d_lock.unlock(); // 뮤텍스 잠금 해제
}


void Service::onRequestReceived(const boost::system::error_code& ec, size_t bytes_transferred)
{
	if (exitClient || m_whisperbool) return; // 클라이언트가 종료하거나, 귓속말 모드인 경우 함수 종료

	if (ec) // 에러가 발생한 경우
	{
		std::cout << "[" << (*m_sock).local_endpoint() << "]"
			<< "Error occured! Error code = "
			<< ec.value()
			<< ". Message: " << ec.message()
			<< endl; // 에러 메시지 출력
		onFinish(m_sock); // 소켓을 종료하는 함수 호출
		return;
	}

	if (bytes_transferred == 0) // 전송된 바이트가 없는 경우
	{
		std::cout << "[" << (*m_sock).local_endpoint() << "]"
			<< "Good Bye Client!" << endl; // 클라이언트가 연결을 종료한 것으로 간주하고 메시지 출력
		onFinish(m_sock); // 소켓 종료 함수 호출
		return;
	}

	PrintTid("StartWriting"); // 현재 쓰레드 ID를 출력하고, "StartWriting" 상태 메시지를 출력

	if (strcmp(m_request, "/w") == 0) // 만약 클라이언트의 요청이 "/w" (귓속말 명령어)인 경우
	{
		m_whisperbool = true; // 귓속말 모드 활성화
		m_response = "Input NickName!"; // 닉네임 입력 요청 메시지 설정
		d_lock.lock(); // 뮤텍스 잠금
		m_sock->async_write_some(asio::buffer(m_response), bind(&Service::Whisper, this, _1, _2));
		// 비동기적으로 닉네임 입력 요청 메시지를 전송하고, 전송 완료 후 Whisper 함수를 호출하도록 설정
		d_lock.unlock(); // 뮤텍스 잠금 해제
		return;
	}

	else if ((strcmp(m_request, "/create") == 0) && (m_client->m_roomNum == 0)) // 방 생성 명령어를 받은 경우, 클라이언트가 현재 방에 속해있지 않을 때
	{
		m_response = "Input RoomName!"; // 방 이름 입력 요청 메시지 설정
		d_lock.lock(); // 뮤텍스 잠금
		m_sock->async_write_some(asio::buffer(m_response), bind(&Service::createRoom, this, _1, _2));
		// 비동기적으로 방 이름 입력 요청 메시지를 전송하고, 전송 완료 후 createRoom 함수를 호출하도록 설정
		d_lock.unlock(); // 뮤텍스 잠금 해제
		return;
	}

	else if ((strcmp(m_request, "/enter") == 0) && (m_client->m_roomNum == 0)) // 방 입장 명령어를 받은 경우, 클라이언트가 현재 방에 속해있지 않을 때
	{
		m_response = "Input RoomName!"; // 방 이름 입력 요청 메시지 설정
		d_lock.lock(); // 뮤텍스 잠금
		m_sock->async_write_some(asio::buffer(m_response), bind(&Service::EnterRoom, this, _1, _2));
		// 비동기적으로 방 이름 입력 요청 메시지를 전송하고, 전송 완료 후 EnterRoom 함수를 호출하도록 설정
		d_lock.unlock(); // 뮤텍스 잠금 해제
		return;
	}

	else if ((strcmp(m_request, "/exit") == 0) && (m_client->m_roomNum != 0)) // 방 나가기 명령어를 받은 경우, 클라이언트가 현재 방에 속해 있을 때
	{
		roomVec[0]->m_sockVector.push_back(m_client); // 클라이언트를 첫 번째 방으로 이동

		for (int i = 0; i < roomVec[m_client->m_roomNum]->m_sockVector.size(); i++) // 클라이언트가 속한 방에서 클라이언트를 찾기 위해 반복문 실행
		{
			if (roomVec[m_client->m_roomNum]->m_sockVector[i] == m_client) // 클라이언트를 찾은 경우
			{
				roomVec[m_client->m_roomNum]->m_sockVector.erase((roomVec[m_client->m_roomNum]->m_sockVector.begin() + i)); // 벡터에서 클라이언트 제거

				if (roomVec[m_client->m_roomNum]->m_sockVector.size() == 0) // 방에 남아있는 클라이언트가 없는 경우
				{
					roomVec.erase(roomVec.begin() + m_client->m_roomNum); // 빈 방을 벡터에서 제거
				}
				break; // 반복문 종료
			}
		}

		m_client->m_roomNum = 0; // 클라이언트의 방 번호를 0으로 설정 (어느 방에도 속하지 않음을 의미)

		d_lock.lock(); // 뮤텍스 잠금
		m_sock->async_read_some(asio::buffer(m_request, MAXBUF), m_io_strand.wrap(bind(&Service::onRequestReceived, this, _1, _2)));
		// 비동기적으로 데이터를 읽어와 m_request 버퍼에 저장하고, 읽기가 완료되면 onRequestReceived 함수를 호출하도록 설정
		d_lock.unlock(); // 뮤텍스 잠금 해제
		return;
	}

	else if (strcmp(m_request, "/room") == 0) // 현재 존재하는 방 정보를 요청한 경우
	{
		system::error_code ec;
		m_response = "";

		for (auto oneOfRoom : roomVec) // 모든 방을 순회하면서
		{
			m_response = m_response + "[" + oneOfRoom->m_roomName + "]" + " [Size : " + to_string(oneOfRoom->m_sockVector.size()) + "] [Owner : " + oneOfRoom->m_sockVector[0]->m_nickName + "]\n";
			// 각 방의 이름, 인원 수, 방장 이름을 m_response에 추가
		}

		d_lock.lock(); // 뮤텍스 잠금
		m_client->m_sock->write_some(asio::buffer(m_response), ec); // 방 정보를 클라이언트에게 전송
		d_lock.unlock(); // 뮤텍스 잠금 해제

		if (ec) // 전송 중 에러가 발생한 경우
		{
			std::cout << "Error occured! Error code = "
				<< ec.value()
				<< ". Message: " << ec.message(); // 에러 메시지 출력
		}

		memset(m_request, '\0', MAXBUF); // m_request 버퍼 초기화

		d_lock.lock(); // 뮤텍스 잠금
		m_sock->async_read_some(asio::buffer(m_request, MAXBUF), m_io_strand.wrap(bind(&Service::onRequestReceived, this, _1, _2)));
		// 비동기적으로 데이터를 읽어와 m_request 버퍼에 저장하고, 읽기가 완료되면 onRequestReceived 함수를 호출하도록 설정
		d_lock.unlock(); // 뮤텍스 잠금 해제
		return;
	}

	m_request[bytes_transferred] = '\0'; // 수신된 데이터의 끝에 널 문자를 추가하여 문자열 종료
	m_response = m_nickName + " : " + m_request; // 클라이언트의 닉네임과 수신된 메시지를 합쳐 m_response에 저장
	m_io_strand.post(bind(&Service::EchoSend, this, m_response)); // EchoSend 함수를 호출하여 클라이언트에게 메시지 에코
}

void Service::Whisper(const boost::system::error_code& ec, size_t bytes_transferred)
{
	if (ec)
	{
		std::cout << "[" << (*m_sock).local_endpoint() << "]"
			<< "Error occured! Error code = "
			<< ec.value()
			<< ". Message: " << ec.message()
			<< endl;
		onFinish(m_sock);
		return;
	}

	memset(m_request, '\0', MAXBUF);

	d_lock.lock();
	m_sock->async_read_some(asio::buffer(m_request, MAXBUF), bind(&Service::Whisper2, this, _1, _2));
	d_lock.unlock();
}

void Service::Whisper2(const boost::system::error_code& ec, size_t bytes_transferred)
{
	if (exitClient) return;

	if (ec)
	{
		std::cout << "[" << (*m_sock).local_endpoint() << "]"
			<< "Error occured! Error code = "
			<< ec.value()
			<< ". Message: " << ec.message()
			<< endl;
		onFinish(m_sock);
		return;
	}

	if (bytes_transferred == 0)
	{
		std::cout << "[" << (*m_sock).local_endpoint() << "]"
			<< "Good Bye Client!" << endl;
		onFinish(m_sock);
		return;
	}

	m_whisperNickName = m_request;
	cout << "[" << m_nickName << "->" << m_whisperNickName << " -- Whisper.]" << endl;
	m_response = "Input Message!";

	d_lock.lock();
	m_sock->async_write_some(asio::buffer(m_response), bind(&Service::Whisper3, this, _1, _2));
	d_lock.unlock();
}

void Service::Whisper3(const boost::system::error_code& ec, size_t bytes_transferred)
{
	if (ec)
	{
		std::cout << "[" << (*m_sock).local_endpoint() << "]"
			<< "Error occured! Error code = "
			<< ec.value()
			<< ". Message: " << ec.message()
			<< endl;
		onFinish(m_sock);
		return;
	}

	memset(m_request, '\0', MAXBUF);

	d_lock.lock();
	m_sock->async_read_some(asio::buffer(m_request, MAXBUF), bind(&Service::WhisperLast, this, _1, _2));
	d_lock.unlock();
}

void Service::WhisperLast(const boost::system::error_code& ec, size_t bytes_transferred)
{
	if (exitClient) return;

	if (ec)
	{
		std::cout << "[" << (*m_sock).local_endpoint() << "]"
			<< "Error occured! Error code = "
			<< ec.value()
			<< ". Message: " << ec.message()
			<< endl;
		onFinish(m_sock);
		return;
	}

	if (bytes_transferred == 0)
	{
		std::cout << "[" << (*m_sock).local_endpoint() << "]"
			<< "Good Bye Client!" << endl;
		onFinish(m_sock);
		return;
	}

	m_request[bytes_transferred] = '\0';
	m_response = "[Whisper]" + m_nickName + " : " + m_request;

	m_ios.post(bind(&Service::WhisperSend, this, m_whisperNickName, m_response));
	m_whisperbool = false;
}

void Service::WhisperSend(string nickName, string response)
{
	system::error_code ec;

	for (int i = 0; i < sockVector.size(); i++)
	{
		if (sockVector[i]->m_nickName == nickName)
		{
			d_lock.lock();
			sockVector[i]->m_sock->write_some(asio::buffer(response), ec);
			d_lock.unlock();

			if (ec)
			{
				std::cout << "[" << (*m_sock).local_endpoint() << "]"
					<< "Error occured! Error code = "
					<< ec.value()
					<< ". Message: " << ec.message()
					<< endl;
				onFinish(m_sock);
				return;
			}
			break;
		}
	}

	memset(m_request, '\0', MAXBUF);

	d_lock.lock();
	m_sock->async_read_some(asio::buffer(m_request, MAXBUF), m_io_strand.wrap(bind(&Service::onRequestReceived, this, _1, _2)));
	d_lock.unlock();
}

void Service::createRoom(const boost::system::error_code& ec, size_t bytes_transferred)
{
	if (ec)
	{
		std::cout << "[" << (*m_sock).local_endpoint() << "]"
			<< "Error occured! Error code = "
			<< ec.value()
			<< ". Message: " << ec.message()
			<< endl;
		onFinish(m_sock);
		return;
	}

	memset(m_request, '\0', MAXBUF);

	d_lock.lock();
	m_sock->async_read_some(asio::buffer(m_request, MAXBUF), bind(&Service::createRoom2, this, _1, _2));
	d_lock.unlock();
}

void Service::createRoom2(const boost::system::error_code& ec, size_t bytes_transferred)
{
	if (exitClient) return;

	if (ec)
	{
		std::cout << "[" << (*m_sock).local_endpoint() << "]"
			<< "Error occured! Error code = "
			<< ec.value()
			<< ". Message: " << ec.message()
			<< endl;
		onFinish(m_sock);
		return;
	}

	if (bytes_transferred == 0)
	{
		std::cout << "[" << (*m_sock).local_endpoint() << "]"
			<< "Good Bye Client!" << endl;
		onFinish(m_sock);
		return;
	}

	string roomName = m_request + '\0';
	Room* room = new Room(roomName);
	roomVec.push_back(room);
	room->m_sockVector.push_back(m_client);

	for (int i = 0; i < roomVec[0]->m_sockVector.size(); i++)
	{
		if (roomVec[0]->m_sockVector[i] == m_client)
		{
			roomVec[0]->m_sockVector.erase(roomVec[0]->m_sockVector.begin() + i);
			break;
		}
	}

	cout << "Room Created : " << roomName << endl;

	for (int i = 0; i < roomVec.size(); i++)
	{
		if (roomName == roomVec[i]->m_roomName)
		{
			m_client->m_roomNum = i;
			break;
		}
	}

	memset(m_request, '\0', MAXBUF);

	d_lock.lock();
	m_sock->async_read_some(asio::buffer(m_request, MAXBUF), m_io_strand.wrap(bind(&Service::onRequestReceived, this, _1, _2)));
	d_lock.unlock();
}

void Service::EnterRoom(const boost::system::error_code& ec, size_t bytes_transferred)
{
	if (ec)
	{
		std::cout << "[" << (*m_sock).local_endpoint() << "]"
			<< "Error occured! Error code = "
			<< ec.value()
			<< ". Message: " << ec.message()
			<< endl;
		onFinish(m_sock);
		return;
	}

	memset(m_request, '\0', MAXBUF);

	d_lock.lock();
	m_sock->async_read_some(asio::buffer(m_request, MAXBUF), bind(&Service::EnterRoom2, this, _1, _2));
	d_lock.unlock();
}

void Service::EnterRoom2(const boost::system::error_code& ec, size_t bytes_transferred)
{
	if (exitClient) return;

	if (ec)
	{
		std::cout << "[" << (*m_sock).local_endpoint() << "]"
			<< "Error occured! Error code = "
			<< ec.value()
			<< ". Message: " << ec.message()
			<< endl;
		onFinish(m_sock);
		return;
	}

	if (bytes_transferred == 0)
	{
		std::cout << "[" << (*m_sock).local_endpoint() << "]"
			<< "Good Bye Client!" << endl;
		onFinish(m_sock);
		return;
	}

	string roomName = m_request;

	for (int i = 0; i < roomVec.size(); i++)
	{
		if (roomName == roomVec[i]->m_roomName)
		{
			roomVec[i]->m_sockVector.push_back(m_client);
			m_client->m_roomNum = i;
			cout << "[" << m_client->m_roomNum << " : " << m_nickName << " Enter Room : " << roomName << "]" << endl;

			for (int i = 0; i < roomVec[0]->m_sockVector.size(); i++)
			{
				if (roomVec[0]->m_sockVector[i] == m_client)
				{
					roomVec[0]->m_sockVector.erase(roomVec[0]->m_sockVector.begin() + i);
					break;
				}
			}
			break;
		}
	}

	memset(m_request, '\0', MAXBUF);

	d_lock.lock();
	m_sock->async_read_some(asio::buffer(m_request, MAXBUF), m_io_strand.wrap(bind(&Service::onRequestReceived, this, _1, _2)));
	d_lock.unlock();
}

void Service::EchoSend(string response)
{
	system::error_code ec;

	for (auto oneOfSock : roomVec[m_client->m_roomNum]->m_sockVector)
	{
		d_lock.lock();
		oneOfSock->m_sock->write_some(asio::buffer(m_response), ec);
		d_lock.unlock();

		if (ec)
		{
			std::cout << "Error occured! Error code = "
				<< ec.value()
				<< ". Message: " << ec.message();
		}
	}

	memset(m_request, '\0', MAXBUF);

	d_lock.lock();
	m_sock->async_read_some(asio::buffer(m_request, MAXBUF), m_io_strand.wrap(bind(&Service::onRequestReceived, this, _1, _2)));
	d_lock.unlock();
}

void Service::onFinish(shared_ptr<asio::ip::tcp::socket> sock)
{
	for (int i = 0; i < sockVector.size(); i++)
	{
		if (sockVector[i]->m_sock == sock)
		{
			d_lock.lock();
			sockVector.erase(sockVector.begin() + i);
			cout << "now client count : " << sockVector.size() << endl;
			d_lock.unlock();
		}
	}

	for (int i = 0; i < roomVec[m_client->m_roomNum]->m_sockVector.size(); i++)
	{
		if (roomVec[m_client->m_roomNum]->m_sockVector[i]->m_sock == sock)
		{
			d_lock.lock();
			roomVec[m_client->m_roomNum]->m_sockVector.erase(roomVec[m_client->m_roomNum]->m_sockVector.begin() + i);
			cout << "now Room[" << roomVec[m_client->m_roomNum]->m_roomName << "] client count : " << roomVec[m_client->m_roomNum]->m_sockVector.size() << endl;
			d_lock.unlock();
		}
	}

	sock->close();
	exitClient = true;
	delete this;
}

void PrintTid(std::string mes)
{
	d_lock.lock();
	std::cout << "[" << this_thread::get_id() << "]  " << mes << std::endl;
	d_lock.unlock();
}

void PrintClientsInfo()
{
	for (int i = 0; i < sockVector.size(); i++)
	{
		cout << "[" << i << "]"
			<< sockVector[i]->m_nickName << endl;
	}
}

void PrintRoomsInfo()
{
	for (int i = 0; i < roomVec.size(); i++)
	{
		cout << "[" << i << "]"
			<< roomVec[i]->m_roomName << endl;
	}
}