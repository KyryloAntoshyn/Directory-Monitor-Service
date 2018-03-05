#include "windows.h"
#include <iostream>
#include "fstream"
#include <string>
#include <stdio.h> 
#include <tchar.h>
#include <strsafe.h>
#include <vector>
using namespace std;

#define BUFSIZE 512

char service_name_inside[] = "DirectoryMonitorService"; // Внутреннее имя сервиса
char service_name_outside[] = "Directory Monitor Service"; // Внешнее имя сервиса
char service_exe_path[] = "C:/Users/akiri/source/repos/Directory-Monitor-Service/Debug/DirectoryMonitorService.exe"; // Путь к сервису
char service_log_file_path[] = "C:/ServiceInformationFile.log"; // Путь к файлу, в который пишу информацию о статусе сервиса

char name_pipe_read[] = "\\\\.\\pipe\\DirectoryMonitorPipeRead"; // Имя канала, по которому приходит информация от клиента
char name_pipe_write[] = "\\\\.\\pipe\\DirectoryMonitorPipeWrite"; // Имя канала, по которому сервис передаёт инфомацию клиенту


SERVICE_STATUS service_status;
SERVICE_STATUS_HANDLE hServiceStatus;

ofstream out;

void WINAPI ServiceCtrlHandler(DWORD dwControl)
{
	switch (dwControl)
	{
	case SERVICE_CONTROL_STOP: // остановить сервис
		// Записываем в .log-файл состояние о завершении сервиса
		out << "The service is finished." << endl << flush;
		// закрываем файл
		out.close();

		// устанавливаем состояние остановки
		service_status.dwCurrentState = SERVICE_STOPPED;
		// изменить состояние сервиса
		SetServiceStatus(hServiceStatus, &service_status);
		break;

	case SERVICE_CONTROL_SHUTDOWN: // завершить сервис
		service_status.dwCurrentState = SERVICE_STOPPED;
		// изменить состояние сервиса
		SetServiceStatus(hServiceStatus, &service_status);
		break;

	default:
		// увеличиваем значение контрольной точки
		service_status.dwCheckPoint++;
		// оставляем состояние сервиса без изменения
		SetServiceStatus(hServiceStatus, &service_status);
		break;
	}

	return;
}

struct ThreadParams
{
	explicit ThreadParams(HANDLE h = NULL, char * p = NULL) :
		hPipe(h), directory_path(p) {}

	HANDLE  hPipe;
	char* directory_path;
};

vector<string> split(string source, string delimiter)
{
	size_t pos = 0;
	vector<string> tokens;
	while ((pos = source.find(delimiter)) != std::string::npos) {
		tokens.push_back(source.substr(0, pos));
		source.erase(0, pos + delimiter.length());
	}
	return tokens;
}

DWORD WINAPI DirectoryChangesProcessThread(LPVOID lpParam) 
{
	ThreadParams * parameters = (ThreadParams*)lpParam;

	char directory_path[512];
	strcpy_s(directory_path, parameters->directory_path);

	char buf[2048];
	DWORD nRet;
	BOOL result = TRUE;

	HANDLE hFolder = CreateFile(
		directory_path,
		GENERIC_READ | FILE_LIST_DIRECTORY,
		FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
		NULL,
		OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED,
		NULL
	);

	char directory_changes[512];
	DWORD cbWritten;

	if (hFolder == INVALID_HANDLE_VALUE)
	{
		strcpy_s(directory_changes, "bad_dir");
		WriteFile(parameters->hPipe, directory_changes, strlen(directory_changes) + 1, &cbWritten, NULL);

		out << "Unable to open directory! Service is sending an error message to client." << endl << flush;
		CloseHandle(hFolder);
		return -1;
	}
	else
		out << "Directory " << directory_path << " is under the monitoring!" << endl << flush;

	OVERLAPPED PollingOverlap;

	FILE_NOTIFY_INFORMATION* pNotify;
	int offset;
	PollingOverlap.OffsetHigh = 0;
	PollingOverlap.hEvent = CreateEvent(NULL, TRUE, FALSE, NULL);

	string ch = "a";

	while (result)
	{
		result = ReadDirectoryChangesW(
			hFolder,
			&buf,
			sizeof(buf),
			ch == "a" ? TRUE : FALSE, // Отслеживать ли дерево
			FILE_NOTIFY_CHANGE_FILE_NAME | // Виды изменений
			FILE_NOTIFY_CHANGE_DIR_NAME |
			FILE_NOTIFY_CHANGE_ATTRIBUTES |
			FILE_NOTIFY_CHANGE_SIZE |
			FILE_NOTIFY_CHANGE_LAST_WRITE |
			FILE_NOTIFY_CHANGE_LAST_ACCESS |
			FILE_NOTIFY_CHANGE_CREATION |
			FILE_NOTIFY_CHANGE_SECURITY,
			&nRet,
			&PollingOverlap,
			NULL);

		WaitForSingleObject(PollingOverlap.hEvent, INFINITE);
		offset = 0;
		do
		{
			pNotify = (FILE_NOTIFY_INFORMATION*)((char*)buf + offset);
			char filename[MAX_PATH];
			WideCharToMultiByte(CP_ACP,
				0,
				pNotify->FileName,
				pNotify->FileNameLength / 2,
				filename,
				MAX_PATH,
				0,
				0);
			filename[pNotify->FileNameLength / 2] = 0;
			char directory_changes[512];
			switch (pNotify->Action)
			{
			case FILE_ACTION_ADDED:
				strcpy_s(directory_changes, "File(directory) was added. File(directory) name is: ");
				strcat_s(directory_changes, filename);
				break;
			case FILE_ACTION_REMOVED:
				strcpy_s(directory_changes, "File(directory) was deleted. File(directory) name is: ");
				strcat_s(directory_changes, filename);
				break;
			case FILE_ACTION_MODIFIED:
				strcpy_s(directory_changes, "File(directory) was changed with its attributes or creation time. File(directory) name is: ");
				strcat_s(directory_changes, filename);
				break;
			case FILE_ACTION_RENAMED_OLD_NAME:
				strcpy_s(directory_changes, "File(directory) old name has changed. File(directory) name is: ");
				strcat_s(directory_changes, filename);
				break;
			case FILE_ACTION_RENAMED_NEW_NAME:
				strcpy_s(directory_changes, "File(directory) name change to a brand new one occured. File(directory) name is: ");
				strcat_s(directory_changes, filename);
				break;
			default:
				strcpy_s(directory_changes, "Error occured during the monitoring.");
				strcat_s(directory_changes, filename);
				break;
			}

			// Отправляем сообщение об изменении в файле клиенту
			WriteFile(parameters->hPipe, directory_changes, strlen(directory_changes) + 1, &cbWritten, NULL);
			out << "Service sends via named pipe information: " << directory_changes << endl << flush;

			offset += pNotify->NextEntryOffset;
		} while (pNotify->NextEntryOffset);
	}

	return 0;
}

DWORD WINAPI DirectoryPathProcessThread(LPVOID lpParam)
{
	HANDLE hNamedPipeRead = (HANDLE)lpParam; // Канал для чтения путей к директориям
	
	BOOL fSuccess = FALSE, fConnected = FALSE;
	DWORD cbRead = 0, dwThreadId = 0;
	HANDLE hNamedPipeWrite = INVALID_HANDLE_VALUE, hThread = NULL;

	char directory_path[512]; // Путь к директории, которую нужно просматривать

	// Бесконечно читаю пути
	while (true)
	{
		// Считывание пути к папке
		fSuccess = ReadFile(
			hNamedPipeRead,
			directory_path,
			512,
			&cbRead,
			NULL
		);

		if (!fSuccess || cbRead == 0)
		{
			if (GetLastError() == ERROR_BROKEN_PIPE)
			{
				out << "Client disconnected." << endl
					<< "The last error code: " << GetLastError() << endl << flush;
			}
			else
			{
				out << "Read file failed." << endl
					<< "The last error code: " << GetLastError() << endl << flush;
			}
			break;
		}

		// Создаю новый канал для клиента
		hNamedPipeWrite = CreateNamedPipe(
			name_pipe_write,          // pipe name 
			PIPE_ACCESS_OUTBOUND,      // write access 
			PIPE_TYPE_MESSAGE |       // message type pipe 
			PIPE_READMODE_MESSAGE |   // message-read mode 
			PIPE_WAIT,                // blocking mode 
			PIPE_UNLIMITED_INSTANCES, // max. instances  
			BUFSIZE,                  // output buffer size 
			BUFSIZE,                  // input buffer size 
			0,                        // client time-out 
			NULL);                    // default security attribute

		if (hNamedPipeWrite == INVALID_HANDLE_VALUE)
		{
			out << "Create named pipe failed." << endl
				<< "The last error code: " << GetLastError() << endl << flush;
			return -1;
		}

		fConnected = ConnectNamedPipe(hNamedPipeWrite, NULL) ?
			TRUE : (GetLastError() == ERROR_PIPE_CONNECTED);

		if (fConnected)
		{
			out << "Client connected, creating a processing thread." << endl << flush;

			ThreadParams* parameters = new ThreadParams(hNamedPipeWrite, directory_path);

			// Создаю поток для мониторинга конкретного пути
			HANDLE hThread = CreateThread(
				NULL,							// no security attribute 
				0,								// default stack size 
				DirectoryChangesProcessThread,  // thread proc
				parameters,						// thread parameter 
				0,								// not suspended 
				&dwThreadId);					// returns thread ID 

			if (hThread == NULL)
			{
				out << "Create thread pipe failed." << endl
					<< "The last error code: " << GetLastError() << endl << flush;
				return -1;
			}
			else
				CloseHandle(hThread);
		}
		else
			// The client could not connect, so close the pipe. 
			CloseHandle(hNamedPipeWrite);
	}

	DisconnectNamedPipe(hNamedPipeRead);
	CloseHandle(hNamedPipeRead);

	return 0;
}

void WINAPI ServiceMain(DWORD dwArgc, LPTSTR *lpszArgv)
{
	// регистрируем обработчик управляющих команд для сервиса
	hServiceStatus = RegisterServiceCtrlHandler(
		service_name_inside,
		ServiceCtrlHandler
	);

	if (!hServiceStatus)
	{
		out.open(service_log_file_path);
		out << "Register service control handler failed.";
		out.close();

		return;
	}

	// инициализируем структуру состояния сервиса
	service_status.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
	service_status.dwCurrentState = SERVICE_START_PENDING;
	service_status.dwControlsAccepted = SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN;
	service_status.dwWin32ExitCode = ERROR_SERVICE_SPECIFIC_ERROR;
	service_status.dwServiceSpecificExitCode = 0;
	service_status.dwCheckPoint = 0;
	service_status.dwWaitHint = 5000;

	// устанавливаем состояние сервиса
	if (!SetServiceStatus(hServiceStatus, &service_status))
	{
		out.open(service_log_file_path);
		out << "Set service status 'SERVICE_START_PENDING' failed.";
		out.close();

		return;
	}

	// определяем сервис как работающий
	service_status.dwCurrentState = SERVICE_RUNNING;
	// нет ошибок
	service_status.dwWin32ExitCode = NO_ERROR;
	// устанавливаем новое состояние сервиса
	if (!SetServiceStatus(hServiceStatus, &service_status))
	{
		out.open(service_log_file_path);
		out << "Set service status 'START_PENDING' failed.";
		out.close();

		return;
	}

	// Пишу в .log-файл обновлённое состояние сервиса
	out.open(service_log_file_path);
	out << "Service is creating general thread..." << endl << flush;
	out << "Service name is: " << lpszArgv[0] << endl << flush;

	BOOL   fConnected = FALSE;
	DWORD  dwThreadId = 0;
	HANDLE hNamedPipeRead = INVALID_HANDLE_VALUE, hThread = NULL;

	for (;;)
	{
		// Создаю новый канал для клиента
		hNamedPipeRead = CreateNamedPipe(
			name_pipe_read,           // pipe name 
			PIPE_ACCESS_INBOUND,      // read access 
			PIPE_TYPE_MESSAGE |       // message type pipe 
			PIPE_READMODE_MESSAGE |   // message-read mode 
			PIPE_WAIT,                // blocking mode 
			PIPE_UNLIMITED_INSTANCES, // max. instances  
			BUFSIZE,                  // output buffer size 
			BUFSIZE,                  // input buffer size 
			0,                        // client time-out 
			NULL);                    // default security attribute

		if (hNamedPipeRead == INVALID_HANDLE_VALUE)
		{
			out << "Create named pipe failed." << endl
				<< "The last error code: " << GetLastError() << endl << flush;
			return;
		}

		fConnected = ConnectNamedPipe(hNamedPipeRead, NULL) ?
			TRUE : (GetLastError() == ERROR_PIPE_CONNECTED);

		if (fConnected)
		{
			out << "Client connected, creating a processing thread." << endl << flush;

			// Create a thread for this client. 
			hThread = CreateThread(
				NULL,              // no security attribute 
				0,                 // default stack size 
				DirectoryPathProcessThread,    // thread proc
				(LPVOID)hNamedPipeRead,    // thread parameter 
				0,                 // not suspended 
				&dwThreadId);      // returns thread ID 

			if (hThread == NULL)
			{
				out << "Create thread pipe failed." << endl
					<< "The last error code: " << GetLastError() << endl << flush;
				return;
			}
			else 
				CloseHandle(hThread);
		}
		else
			// The client could not connect, so close the pipe. 
			CloseHandle(hNamedPipeRead);
	}
}

bool InstallService()
{
	SC_HANDLE hServiceControlManager, hService;

	// связываемся с менеджером сервисов
	hServiceControlManager = OpenSCManager(
		NULL,
		NULL,
		SC_MANAGER_CREATE_SERVICE
	);
	if (hServiceControlManager == NULL)
	{
		cout << "Open service control manager failed." << endl
			<< "The last error code: " << GetLastError() << endl
			<< "Press any key to exit." << endl;
		cin.get();

		return false;
	}
	cout << "Service control manager is opened." << endl
		<< "Press any key to continue." << endl;
	cin.get();

	// устанавливаем новый сервис
	hService = CreateService(
		hServiceControlManager,
		service_name_inside,
		service_name_outside,
		SERVICE_ALL_ACCESS,
		SERVICE_WIN32_OWN_PROCESS,
		SERVICE_DEMAND_START,
		SERVICE_ERROR_NORMAL,
		service_exe_path,
		NULL,
		NULL,
		NULL,
		NULL,
		NULL
	);

	if (hService == NULL)
	{
		cout << "Create service failed." << endl
			<< "The last error code: " << GetLastError() << endl
			<< "Press any key to exit." << endl;
		cin.get();

		// закрываем дескриптор менеджера сервисов
		CloseServiceHandle(hServiceControlManager);

		return false;
	}

	cout << "Service is installed." << endl
		<< "Press any key to exit." << endl;
	cin.get();

	// Закрываем дескрипторы
	CloseServiceHandle(hService);
	CloseServiceHandle(hServiceControlManager);

	return true;
}

bool RemoveService()
{
	SC_HANDLE hServiceControlManager, hService;

	// связываемся с менеджером сервисов
	hServiceControlManager = OpenSCManager(
		NULL,
		NULL,
		SC_MANAGER_ALL_ACCESS
	);
	if (hServiceControlManager == NULL)
	{
		cout << "Open service control manager failed." << endl
			<< "The last error code: " << GetLastError() << endl
			<< "Press any key to exit." << endl;
		cin.get();

		return false;
	}
	cout << "Service control manager is opened." << endl
		<< "Press any key to continue." << endl;
	cin.get();

	// Открываем сервис
	hService = OpenService(
		hServiceControlManager,
		service_name_inside,
		SERVICE_STOP | DELETE
	);
	if (hService == NULL)
	{
		cout << "Open service failed." << endl
			<< "The last error code: " << GetLastError() << endl
			<< "Press any key to exit." << endl;
		cin.get();

		// Закрываем дескриптор менеджера сервисов
		CloseServiceHandle(hServiceControlManager);

		return false;
	}

	// Удаляем сервис
	if (!DeleteService(hService))
	{
		cout << "Delete service failed." << endl
			<< "The last error code: " << GetLastError() << endl
			<< "Press any key to exit." << endl;
		cin.get();

		// Закрываем дескрипторы
		CloseServiceHandle(hService);
		CloseServiceHandle(hServiceControlManager);

		return false;
	}

	cout << "Service was succesfully removed." << endl
		<< "Press any key to exit." << endl;
	cin.get();

	// Закрываем дескрипторы
	CloseServiceHandle(hService);
	CloseServiceHandle(hServiceControlManager);

	return true;
}

bool RunService()
{
	SC_HANDLE hServiceControlManager, hService;

	// Связываемся с менеджером сервисов
	hServiceControlManager = OpenSCManager(
		NULL,
		NULL,
		SC_MANAGER_CONNECT
	);
	if (hServiceControlManager == NULL)
	{
		cout << "Open service control manager failed." << endl
			<< "The last error code: " << GetLastError() << endl
			<< "Press any key to exit." << endl;
		cin.get();

		return false;
	}

	cout << "Service control manager is opened." << endl
		<< "Press any key to continue." << endl;
	cin.get();

	// Открываем сервис
	hService = OpenService(
		hServiceControlManager,
		service_name_inside,
		SERVICE_ALL_ACCESS
	);
	if (hService == NULL)
	{
		cout << "Open service failed." << endl
			<< "The last error code: " << GetLastError() << endl
			<< "Press any key to exit." << endl;
		cin.get();

		// Закрываем дескриптор менеджера сервисов
		CloseServiceHandle(hServiceControlManager);

		return false;
	}

	cout << "Service is opened." << endl
		<< "Press any key to continue." << endl;
	cin.get();

	// Старт сервиса
	if (!StartService(
		hService,
		0, // 0 параметров
		NULL // null-указатель на массив с параметрами
	))
	{
		cout << "Start service failed." << endl
			<< "The last error code: " << GetLastError() << endl
			<< "Press any key to exit." << endl;
		cin.get();

		// Закрываем дескрипторы
		CloseServiceHandle(hService);
		CloseServiceHandle(hServiceControlManager);

		return false;
	}

	cout << "Service is started." << endl
		<< "Press any key to exit." << endl;
	cin.get();

	// Закрываем дескрипторы
	CloseServiceHandle(hService);
	CloseServiceHandle(hServiceControlManager);

	return true;
}

bool StopService()
{
	SC_HANDLE hServiceControlManager, hService;

	// связываемся с менеджером сервисов
	hServiceControlManager = OpenSCManager(
		NULL, // локальная машина
		NULL, // активная база данных сервисов
		SC_MANAGER_CONNECT // соединение с менеджером сервисов
	);
	if (hServiceControlManager == NULL)
	{
		cout << "Open service control manager failed." << endl
			<< "The last error code: " << GetLastError() << endl
			<< "Press any key to exit." << endl;
		cin.get();

		return false;
	}
	cout << "Service control manager is opened." << endl
		<< "Press any key to continue." << endl;
	cin.get();

	// открываем сервис
	hService = OpenService(
		hServiceControlManager, // дексриптор менеджера сервисов
		service_name_inside, // внутреннее имя сервиса
		SERVICE_ALL_ACCESS // полный доступ к сервису
	);

	if (hService == NULL)
	{
		cout << "Open service failed." << endl
			<< "The last error code: " << GetLastError() << endl
			<< "Press any key to exit." << endl;
		cin.get();

		// Закрываем дескриптор менеджера сервисов
		CloseServiceHandle(hServiceControlManager);

		return false;
	}

	if (!QueryServiceStatus(
		hService,
		&service_status
	))
	{
		cout << "An error occured while quering service." << endl;
		return false;
	}
	else
	{
		if (service_status.dwCurrentState == SERVICE_STOPPED)
		{
			cout << "Service is already stopped!" << endl;
			return true;
		}
	}

	// Остановка сервиса
	ControlService(hService, SERVICE_CONTROL_STOP, &service_status);

	cout << "Service has been stopped.\n\n";

	// Закрытие каналов и остановка потоков сервиса
	cout << "Service is going to close named pipes and stop threads:" << endl;

	cout << "Pipe for writing changes in directory was disconnected and its handle was closed." << endl;
	//DisconnectNamedPipe(hNamedPipeWrite);
	//CloseHandle(hNamedPipeWrite);

	cout << "Thread for monitoring directory for changes was terminated and its handle was closed." << endl;
	//TerminateThread(hDirectoryMonitorThread, 0);
	//CloseHandle(hDirectoryMonitorThread);

	cout << "Pipe for reading directory paths was disconnected and its handle was closed." << endl;
	//DisconnectNamedPipe(hNamedPipeRead);
	//CloseHandle(hNamedPipeRead);

	cout << "Thread for processing new directory paths for client was terminated and its handle was closed." << endl;
	//TerminateThread(hPathsProcessThread, 0);
	//CloseHandle(hPathsProcessThread);

	return true;
}

int main(int argc, char* argv[])
{
	if (argc < 2) // Запускаю диспетчер сервиса
	{
		SERVICE_TABLE_ENTRY service_table[] =
		{
			{ service_name_inside, ServiceMain },
			{ NULL, NULL } // обязательно, больше сервисов нет
		};

		// запускаем диспетчер сервиса
		if (!StartServiceCtrlDispatcher(service_table))
		{
			out.open(service_log_file_path);
			out << "Start service control dispatcher failed with error: " << GetLastError();
			out.close();

			return 0;
		}
	}
	else if (strcmp(argv[argc - 1], "install") == 0) // Установка сервиса
	{
		InstallService();
	}
	else if (strcmp(argv[argc - 1], "remove") == 0) // Удаление сервиса
	{
		RemoveService();
	}
	else if (strcmp(argv[argc - 1], "run") == 0) // Запуск сервиса
	{
		RunService();
	}
	else if (strcmp(argv[argc - 1], "stop") == 0) // Остановка сервиса
	{
		StopService();
	}

	return 0;
}