#include "windows.h"
#include <iostream>
#include "fstream"
#include <string>
using namespace std;

char service_name_inside[] = "DirectoryMonitorService"; // Внутреннее имя сервиса
char service_name_outside[] = "Directory Monitor Service"; // Внешнее имя сервиса
char service_exe_path[] = "C:/Users/Anastasia/source/repos/Directory-Monitor-Service/Debug/DirectoryMonitorService.exe"; // Путь к сервису
char service_log_file_path[] = "C:/ServiceInformationFile.log"; // Путь к файлу, в который пишу информацию о статусе сервиса

char name_pipe_read[] = "\\\\.\\pipe\\DirectoryMonitorPipeRead"; // Имя канала, по которому приходит информация от клиента
char name_pipe_write[] = "\\\\.\\pipe\\DirectoryMonitorPipeWrite"; // Имя канала, по которому сервис передаёт инфомацию клиенту

SERVICE_STATUS service_status;
SERVICE_STATUS_HANDLE hServiceStatus;

ofstream out;

HANDLE hNamedPipeRead;
HANDLE hNamedPipeWrite;

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

DWORD WINAPI DirectoryProcess(LPVOID lpParam)
{
	// Создаю именованный канал, по которому сервис отправляет клиенту изменения в нужной папке
	hNamedPipeWrite = CreateNamedPipe(
		name_pipe_write, // имя канала
		PIPE_ACCESS_OUTBOUND, // пишем в канал
		PIPE_TYPE_MESSAGE | PIPE_WAIT, // синхронная передача сообщений
		1, // максимальное количество экземпляров канала
		0, // размер выходного буфера по умолчанию
		0, // размер входного буфера по умолчанию
		INFINITE, // клиент ждет связь бесконечно долго
		NULL // защита по умолчанию
	);

	// проверяем на успешное создание канала
	if (hNamedPipeWrite == INVALID_HANDLE_VALUE)
	{
		out << "Creation of the named pipe failed." << endl
			<< "The last error code: " << GetLastError() << endl << flush;
		return -1;
	}
	else
		out << "Pipe " << name_pipe_write << " created." << endl << flush;

	// Пишу в файл сообщение об ошибке, связанной с коннектом пользовательского приложения к каналу
	if (!ConnectNamedPipe(hNamedPipeWrite, NULL))
	{
		out << "The connection failed." << endl
			<< "The last error code: " << GetLastError() << endl << flush;
		CloseHandle(hNamedPipeWrite);
		return -1;
	}
	else
		out << "Client successfully connected to " << name_pipe_write << endl << flush;

	// Начинаю мониторить папку
	char directory_path[512];
	strcpy_s(directory_path, (char*)lpParam);
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
	
	if (hFolder == INVALID_HANDLE_VALUE)
	{
		out << "Unable to open directory!" << endl << flush;
		CloseHandle(hFolder);
		return -1;
	}
	
	OVERLAPPED PollingOverlap;
	
	FILE_NOTIFY_INFORMATION* pNotify;
	int offset;
	PollingOverlap.OffsetHigh = 0;
	PollingOverlap.hEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
	
	char changes_in_directory_msg[512];
	
	while (result)
	{
		result = ReadDirectoryChangesW(
			hFolder,
			&buf,
			sizeof(buf),
			FALSE,
			FILE_NOTIFY_CHANGE_FILE_NAME |
			FILE_NOTIFY_CHANGE_SIZE |
			FILE_NOTIFY_CHANGE_DIR_NAME |
			FILE_NOTIFY_CHANGE_ATTRIBUTES |
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
			switch (pNotify->Action)
			{
			case FILE_ACTION_ADDED:
				strcpy_s(changes_in_directory_msg, "File was added to directory.");
				break;
			case FILE_ACTION_REMOVED:
				strcpy_s(changes_in_directory_msg, "File was deleted from directory.");
				break;
			case FILE_ACTION_MODIFIED:
				strcpy_s(changes_in_directory_msg, "File was changed with its attributes or creation time.");
				break;
			case FILE_ACTION_RENAMED_OLD_NAME:
				strcpy_s(changes_in_directory_msg, "The old file name has changed.");
				break;
			case FILE_ACTION_RENAMED_NEW_NAME:
				strcpy_s(changes_in_directory_msg, "File name change to brand new one occured.");
				break;
			default:
				strcpy_s(changes_in_directory_msg, "File was added to directory.");
				break;
			}

			// Отправляем сообщение об изменении в файле клиенту
			DWORD cbWritten;
			WriteFile(hNamedPipeWrite, changes_in_directory_msg, strlen(changes_in_directory_msg) + 1, &cbWritten, NULL);
			out << "Service sends via named pipe information: " << changes_in_directory_msg << endl << flush;

			offset += pNotify->NextEntryOffset;
		} while (pNotify->NextEntryOffset); //(offset != 0);
	}
	return 0;
}

DWORD WINAPI ClientPathsProcess(LPVOID lpParam)
{
	// Создаю именованный канал для чтения информации от клиента о том, какую папку нужно мониторить
	 hNamedPipeRead = CreateNamedPipe(
		name_pipe_read, // имя канала
		PIPE_ACCESS_INBOUND, // читаем из канала
		PIPE_TYPE_MESSAGE | PIPE_WAIT, // синхронная передача сообщений
		1, // максимальное количество экземпляров канала
		0, // размер выходного буфера по умолчанию
		0, // размер входного буфера по умолчанию
		INFINITE, // время ожидания
		NULL // защита по умолчанию
	);

	// проверяем на успешное создание канала
	if (hNamedPipeRead == INVALID_HANDLE_VALUE)
	{
		out << "Creation of the named pipe failed." << endl
			<< "The last error code: " << GetLastError() << endl << flush;
		return -1;
	}
	else
		out << "Pipe " << name_pipe_read << " created." << endl << flush;

	// Пишу в файл сообщение об ошибке, связанной с коннектом пользовательского приложения к каналу
	if (!ConnectNamedPipe(hNamedPipeRead, NULL))
	{
		out << "The connection failed." << endl
			<< "The last error code: " << GetLastError() << endl << flush;
		CloseHandle(hNamedPipeRead);
		return -1;
	}
	else
		out << "Client successfully connected to " << name_pipe_read << endl << flush;

	char directory_path_msg[512]; // Директория, которую нужно просматривать
	DWORD cbRead;
	
	HANDLE hPathsProcessThread = NULL;

	// Бесконечно читаю путь к дикректории от клиента
	while (true)
	{	
		// Читаем от клиента путь к папке, которую нужно мониторить
		ReadFile(hNamedPipeRead, directory_path_msg, 512, &cbRead, NULL);

		// Если получен новый путь к мониторингу от клиента (длина сообщения не равна нулю)
		if (cbRead != 0)
		{
			out << "Client has sent a new path for monitoring directory: " << directory_path_msg << endl << flush;

			// Создаю поток для записи клиенту
			if (hPathsProcessThread != NULL)
			{
				TerminateThread(hPathsProcessThread, 0);
				CloseHandle(hNamedPipeWrite);
				DisconnectNamedPipe(hNamedPipeWrite);
			}

			hPathsProcessThread = CreateThread(NULL, 0, DirectoryProcess, directory_path_msg, THREAD_ALL_ACCESS, NULL);
			ResumeThread(hPathsProcessThread);
		}
	}

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

	// Создаю поток для чтения от клиента
	HANDLE hPathsProcessThread = CreateThread(NULL, 0, ClientPathsProcess, NULL, THREAD_ALL_ACCESS, NULL);
	ResumeThread(hPathsProcessThread);
	CloseHandle(hPathsProcessThread);

	out << "Thread for reading directory paths is created!" << endl << flush;
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

	// Остановка сервиса
	ControlService(hService, SERVICE_CONTROL_STOP, &service_status);

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
		switch (service_status.dwCurrentState)
		{

		case SERVICE_CONTINUE_PENDING:
			cout << "Current service status is: SERVICE_CONTINUE_PENDING" << endl;
			break;
		case SERVICE_PAUSE_PENDING:
			cout << "Current service status is: SERVICE_PAUSE_PENDING" << endl;
			break;
		case SERVICE_PAUSED:
			cout << "Current service status is: SERVICE_PAUSED" << endl;
			break;
		case SERVICE_RUNNING:
			cout << "Current service status is: SERVICE_RUNNING" << endl;
			break;
		case SERVICE_START_PENDING:
			cout << "Current service status is: SERVICE_START_PENDING" << endl;
			break;
		case SERVICE_STOP_PENDING:
			cout << "Current service status is: SERVICE_STOP_PENDING" << endl;
			break;
		case SERVICE_STOPPED:
			cout << "Current service status is: SERVICE_STOPPED" << endl;
			break;
		}
	}

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