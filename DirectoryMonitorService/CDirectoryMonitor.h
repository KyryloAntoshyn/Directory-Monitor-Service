#pragma once
#include "windows.h"

class CDirectoryMonitor
{
public:
	CDirectoryMonitor();
	~CDirectoryMonitor();
	bool MonitorDirectory(HANDLE hNamePipe);
};

